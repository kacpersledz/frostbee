/*
 * Frostbee - Zigbee Temperature & Humidity Sensor
 *
 * Production app ported from validated app_uf2 Zigbee SED baseline.
 * OTA-specific behavior stays out of the runtime path until revalidated.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/dfu/mcuboot.h>

#if __has_include(<app_version.h>)
#include <app_version.h>
#define FROSTBEE_SW_VERSION APP_VERSION_STRING
#else
#define FROSTBEE_SW_VERSION "dev"
#endif

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_fota.h>
#include <zb_nrf_platform.h>

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
/* The zigbee_fota library exposes its endpoint from the implementation. */
extern zb_af_endpoint_desc_t zigbee_fota_client_ep;

#if FROSTBEE_ENDPOINT == CONFIG_ZIGBEE_FOTA_ENDPOINT
#error "Frostbee endpoint and Zigbee OTA endpoint must be different."
#endif
#endif

#include "zb_mem_config_custom.h"
#include "zb_frostbee.h"

LOG_MODULE_REGISTER(frostbee, LOG_LEVEL_INF);

#define REPORT_INTERVAL_S      15
#define SED_LONG_POLL_MS       3000
#define OTA_LONG_POLL_MS       500

#define BUTTON_DEBOUNCE_MS         100
#define BUTTON_SHORT_PRESS_MAX_MS  1000
#define BUTTON_FACTORY_RESET_MS    5000

#define ADC_NODE        DT_NODELABEL(adc)
#define ADC_CHANNEL_ID  5
#define ADC_RESOLUTION  12
#define ADC_VREF_MV     600
#define ADC_GAIN_FACTOR 6
#define VDIV_FACTOR     2

#define FROSTBEE_TEMP_MIN_VALUE  (-4000)
#define FROSTBEE_TEMP_MAX_VALUE  12500
#define FROSTBEE_HUM_MIN_VALUE   0
#define FROSTBEE_HUM_MAX_VALUE   10000

#define RESET_BUTTON_NODE DT_ALIAS(sw0)

struct zb_device_ctx {
	zb_zcl_basic_attrs_ext_t basic_attr;
	zb_zcl_identify_attrs_t identify_attr;
	zb_uint8_t battery_type;         /* ID 0xff01 */
    zb_uint8_t battery_series_count; /* ID 0xff02 */
	zb_uint8_t battery_voltage;
	zb_uint8_t battery_percentage;
	zb_int16_t temp_measure_value;
	zb_int16_t temp_min_value;
	zb_int16_t temp_max_value;
	zb_uint16_t temp_tolerance;
	zb_uint16_t hum_measure_value;
	zb_uint16_t hum_min_value;
	zb_uint16_t hum_max_value;
};

static struct zb_device_ctx dev_ctx;

static const struct device *sht = DEVICE_DT_GET(DT_NODELABEL(sht40));
static const struct device *adc_dev = DEVICE_DT_GET(ADC_NODE);
static const struct gpio_dt_spec vbat_enable = GPIO_DT_SPEC_GET(DT_NODELABEL(vbat_en), gpios);

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;
static struct k_work_delayable debounce_work;
static struct k_work_delayable long_press_work;
static int64_t button_press_time;
static bool button_pressed_state;
static atomic_t long_press_handled;
#endif

static K_MUTEX_DEFINE(sensor_mutex);
static bool zigbee_network_ready;
static bool ota_in_progress;
static bool running_image_confirmed;
static uint32_t forced_reports_requested;
static uint32_t forced_reports_attempted;
static uint32_t forced_reports_completed;
static uint32_t forced_reports_failed;

static struct adc_channel_cfg adc_cfg = {
	.gain = ADC_GAIN_1_6,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	.channel_id = ADC_CHANNEL_ID,
	.input_positive = SAADC_CH_PSELP_PSELP_AnalogInput5,
};

static int16_t adc_sample_buffer;
static struct adc_sequence adc_seq = {
	.channels = BIT(ADC_CHANNEL_ID),
	.buffer = &adc_sample_buffer,
	.buffer_size = sizeof(adc_sample_buffer),
	.resolution = ADC_RESOLUTION,
};

ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST_EXT(
	basic_attr_list,
	&dev_ctx.basic_attr.zcl_version,
	&dev_ctx.basic_attr.app_version,
	&dev_ctx.basic_attr.stack_version,
	&dev_ctx.basic_attr.hw_version,
	dev_ctx.basic_attr.mf_name,
	dev_ctx.basic_attr.model_id,
	dev_ctx.basic_attr.date_code,
	&dev_ctx.basic_attr.power_source,
	dev_ctx.basic_attr.location_id,
	&dev_ctx.basic_attr.ph_env,
	dev_ctx.basic_attr.sw_ver);

ZB_ZCL_DECLARE_IDENTIFY_CLIENT_ATTRIB_LIST(identify_client_attr_list);
ZB_ZCL_DECLARE_IDENTIFY_SERVER_ATTRIB_LIST(identify_server_attr_list, &dev_ctx.identify_attr.identify_time);

static zb_zcl_attr_t power_config_attr_list[] = {
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY | ZB_ZCL_ATTR_ACCESS_REPORTING,
		ZB_ZCL_NON_MANUFACTURER_SPECIFIC,
		(void *)&dev_ctx.battery_voltage
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY | ZB_ZCL_ATTR_ACCESS_REPORTING,
		ZB_ZCL_NON_MANUFACTURER_SPECIFIC,
		(void *)&dev_ctx.battery_percentage
	},
	{
        0xff01, // Battery Type
        ZB_ZCL_ATTR_TYPE_U8,
        ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        ZB_ZCL_NON_MANUFACTURER_SPECIFIC,
        (void *)&dev_ctx.battery_type
    },
    {
        0xff02, // Series Count
        ZB_ZCL_ATTR_TYPE_U8,
        ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        ZB_ZCL_NON_MANUFACTURER_SPECIFIC,
        (void *)&dev_ctx.battery_series_count
    },
	{
		ZB_ZCL_NULL_ID,
		0,
		0,
		ZB_ZCL_NON_MANUFACTURER_SPECIFIC,
		NULL
	}
};

ZB_ZCL_DECLARE_TEMP_MEASUREMENT_ATTRIB_LIST(
	temp_measurement_attr_list,
	&dev_ctx.temp_measure_value,
	&dev_ctx.temp_min_value,
	&dev_ctx.temp_max_value,
	&dev_ctx.temp_tolerance);

ZB_ZCL_DECLARE_REL_HUMIDITY_MEASUREMENT_ATTRIB_LIST(
	humidity_attr_list,
	&dev_ctx.hum_measure_value,
	&dev_ctx.hum_min_value,
	&dev_ctx.hum_max_value);

ZB_DECLARE_FROSTBEE_CLUSTER_LIST(
	frostbee_clusters,
	basic_attr_list,
	identify_client_attr_list,
	identify_server_attr_list,
	power_config_attr_list,
	temp_measurement_attr_list,
	humidity_attr_list);

ZB_DECLARE_FROSTBEE_EP(
	frostbee_ep,
	FROSTBEE_ENDPOINT,
	frostbee_clusters);

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
ZBOSS_DECLARE_DEVICE_CTX_2_EP(frostbee_ctx, zigbee_fota_client_ep, frostbee_ep);
#else
ZBOSS_DECLARE_DEVICE_CTX_1_EP(frostbee_ctx, frostbee_ep);
#endif

static int compare_int16(const void *a, const void *b)
{
	return (*(int16_t *)a - *(int16_t *)b);
}

static void log_fixed2(const char *label, int32_t value)
{
	int32_t abs_value = value < 0 ? -value : value;
	LOG_INF("%s: %s%d.%02d", label, value < 0 ? "-" : "", abs_value / 100, abs_value % 100);
}

static void clusters_attr_init(void)
{
	dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.app_version = 1;
	dev_ctx.basic_attr.stack_version = 1;
	dev_ctx.basic_attr.hw_version = 1;
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.mf_name, "Frostbee", 8);
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.model_id, "FBE_TH_1", 8);
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.date_code, "20260323", 8);
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.sw_ver, FROSTBEE_SW_VERSION,
		ZB_ZCL_STRING_CONST_SIZE(FROSTBEE_SW_VERSION));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.location_id, "", 0);
	dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
	dev_ctx.basic_attr.ph_env = ZB_ZCL_BASIC_ENV_UNSPECIFIED;

	dev_ctx.identify_attr.identify_time = ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	dev_ctx.battery_voltage = 45;
	dev_ctx.battery_percentage = 200;
	dev_ctx.temp_measure_value = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.temp_min_value = FROSTBEE_TEMP_MIN_VALUE;
	dev_ctx.temp_max_value = FROSTBEE_TEMP_MAX_VALUE;
	dev_ctx.temp_tolerance = 20;
	dev_ctx.hum_measure_value = ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.hum_min_value = FROSTBEE_HUM_MIN_VALUE;
	dev_ctx.hum_max_value = FROSTBEE_HUM_MAX_VALUE;

	dev_ctx.battery_type = BATTERY_TYPE_ALKALINE;
	// To be changed in final version
    dev_ctx.battery_series_count = 3;
}

static int read_sensor_once(zb_int16_t *temp_centi, zb_uint16_t *hum_centi)
{
	struct sensor_value temp;
	struct sensor_value hum;
	int ret;
	int64_t temp_micro;
	int64_t hum_micro;

	if (!device_is_ready(sht)) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch(sht);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret < 0) {
		return ret;
	}

	temp_micro = (int64_t)temp.val1 * 1000000LL + temp.val2;
	hum_micro = (int64_t)hum.val1 * 1000000LL + hum.val2;
	*temp_centi = (zb_int16_t)(temp_micro / 10000);
	*hum_centi = (zb_uint16_t)(hum_micro / 10000);

	log_fixed2("Temperature [C]", *temp_centi);
	log_fixed2("Humidity [%]", *hum_centi);
	return 0;
}

static zb_uint8_t interpolate(int32_t x, int32_t x_low, int32_t y_low, int32_t x_high, int32_t y_high) {
    if (x <= x_low) return y_low;
    if (x >= x_high) return y_high;

    return (zb_uint8_t)(y_low + (y_high - y_low) * (x - x_low) / (x_high - x_low));
}

static zb_uint8_t calculate_pct_from_lookup(int32_t mv, zb_uint8_t type) {
    if (type == BATTERY_TYPE_LITHIUM) {
        if (mv >= 1700) return 200;
        if (mv >= 1600) return interpolate(mv, 1600, 190, 1700, 200);
        if (mv >= 1500) return interpolate(mv, 1500, 170, 1600, 190);
        if (mv >= 1400) return interpolate(mv, 1400, 140, 1500, 170);
        if (mv >= 1300) return interpolate(mv, 1300, 90, 1400, 140);
        if (mv >= 1200) return interpolate(mv, 1200, 20, 1300, 90);
        return 0;
    } else if (type == BATTERY_TYPE_NIMH) {
        if (mv >= 1400) return 200;
        if (mv >= 1300) return interpolate(mv, 1300, 180, 1400, 200);
        if (mv >= 1250) return interpolate(mv, 1250, 150, 1300, 180);
        if (mv >= 1200) return interpolate(mv, 1200, 120, 1250, 150);
        if (mv >= 1150) return interpolate(mv, 1150, 80, 1200, 120);
        if (mv >= 1100) return interpolate(mv, 1100, 30, 1150, 80);
        if (mv >= 1000) return interpolate(mv, 1000, 0, 1100, 30);
        return 0;
    } else { // Alkaline
        if (mv >= 1600) return 200;
        if (mv >= 1500) return interpolate(mv, 1500, 180, 1600, 200);
        if (mv >= 1400) return interpolate(mv, 1400, 150, 1500, 180);
        if (mv >= 1300) return interpolate(mv, 1300, 110, 1400, 150);
        if (mv >= 1200) return interpolate(mv, 1200, 70, 1300, 110);
        if (mv >= 1100) return interpolate(mv, 1100, 30, 1200, 70);
        if (mv >= 1000) return interpolate(mv, 1000, 0, 1100, 30);
        return 0;
    }
}

static int read_battery_once(zb_uint8_t *battery_zcl, zb_uint8_t *battery_pct_zcl)
{
	int ret;
	int16_t samples[5];
	int32_t adc_mv;
	int32_t battery_mv;

	if (!device_is_ready(adc_dev)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&vbat_enable, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}

	k_msleep(2);

	for (int i = 0; i < 5; i++) {
		ret = adc_read(adc_dev, &adc_seq);
		if (ret < 0) {
			gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);
			return ret;
		}

		samples[i] = adc_sample_buffer;
		if (i < 4) {
			k_usleep(500);
		}
	}

	gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);

	qsort(samples, 5, sizeof(int16_t), compare_int16);

	adc_mv = ((int32_t)(samples[1] + samples[2] + samples[3]) / 3);
	adc_mv = (adc_mv * ADC_VREF_MV * ADC_GAIN_FACTOR) / 4095;
	battery_mv = adc_mv * VDIV_FACTOR;
	*battery_zcl = (zb_uint8_t)((battery_mv + 50) / 100);

    uint8_t series_count = dev_ctx.battery_series_count;
    if (series_count == 0) {
        LOG_WRN("Invalid series count (0), defaulting to 1 to prevent crash");
        series_count = 1;
    }

    int32_t mv_per_cell = battery_mv / series_count;
    *battery_pct_zcl = calculate_pct_from_lookup(mv_per_cell, dev_ctx.battery_type);

	LOG_INF("Battery: %d mV (ZCL=%u), %u%% (ZCL=%u)",
		battery_mv, *battery_zcl, *battery_pct_zcl / 2, *battery_pct_zcl);

	return 0;
}

static void measurement_update(zb_bool_t force_report)
{
	zb_int16_t temp_centi;
	zb_uint16_t hum_centi;
	zb_uint8_t battery_zcl;
	zb_uint8_t battery_pct_zcl;
	int ret;

	if (force_report) {
		forced_reports_attempted++;
	}

	k_mutex_lock(&sensor_mutex, K_FOREVER);

	ret = read_sensor_once(&temp_centi, &hum_centi);
	if (ret < 0) {
		LOG_ERR("Sensor read failed: %d", ret);
		if (force_report) {
			forced_reports_failed++;
		}
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	ret = read_battery_once(&battery_zcl, &battery_pct_zcl);
	if (ret < 0) {
		LOG_ERR("Battery read failed: %d", ret);
		if (force_report) {
			forced_reports_failed++;
		}
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	dev_ctx.temp_measure_value = temp_centi;
	dev_ctx.hum_measure_value = hum_centi;
	dev_ctx.battery_voltage = battery_zcl;
	dev_ctx.battery_percentage = battery_pct_zcl;

	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
		(zb_uint8_t *)&dev_ctx.temp_measure_value,
		force_report);

	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
		(zb_uint8_t *)&dev_ctx.hum_measure_value,
		force_report);

	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		(zb_uint8_t *)&dev_ctx.battery_voltage,
		force_report);

	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		(zb_uint8_t *)&dev_ctx.battery_percentage,
		force_report);

	if (force_report) {
		forced_reports_completed++;
		LOG_INF("Forced report counters: requested=%u attempted=%u completed=%u failed=%u",
			forced_reports_requested,
			forced_reports_attempted,
			forced_reports_completed,
			forced_reports_failed);
	}

	k_mutex_unlock(&sensor_mutex);
}

static void measurement_now_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);
	measurement_update(ZB_TRUE);
}

static void measurement_periodic(zb_bufid_t bufid)
{
	ARG_UNUSED(bufid);
	if (!ota_in_progress) {
		measurement_update(ZB_FALSE);
	}
	ZB_SCHEDULE_APP_ALARM(measurement_periodic, 0,
			      (zb_time_t)REPORT_INTERVAL_S *
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
}

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
static void do_factory_reset(zb_uint8_t param)
{
	ARG_UNUSED(param);

	LOG_WRN("Factory reset requested, leaving network and erasing NVRAM");
	zb_bdb_reset_via_local_action(param);
}

static void long_press_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (gpio_pin_get_dt(&reset_button) == 1) {
		atomic_set(&long_press_handled, 1);
		LOG_WRN("Button held >= 5s, scheduling factory reset");
		ZB_SCHEDULE_APP_CALLBACK(do_factory_reset, 0);
	}
}

static void debounce_handler(struct k_work *work)
{
	int pressed;

	ARG_UNUSED(work);
	pressed = gpio_pin_get_dt(&reset_button);

	if (pressed == button_pressed_state) {
		return;
	}
	button_pressed_state = pressed;

	if (pressed) {
		button_press_time = k_uptime_get();
		atomic_set(&long_press_handled, 0);
		k_work_schedule(&long_press_work, K_MSEC(BUTTON_FACTORY_RESET_MS));
		LOG_INF("Button pressed");
		return;
	}

	k_work_cancel_delayable(&long_press_work);

	if (atomic_get(&long_press_handled)) {
		atomic_set(&long_press_handled, 0);
		LOG_INF("Button released after long press");
		return;
	}

	if ((k_uptime_get() - button_press_time) < BUTTON_SHORT_PRESS_MAX_MS) {
		if (zigbee_network_ready) {
			if (ota_in_progress) {
				LOG_INF("Short press: ignored during OTA transfer");
			} else {
				LOG_INF("Short press: forced report");
				forced_reports_requested++;
				ZB_SCHEDULE_APP_CALLBACK(measurement_now_cb, 0);
			}
		} else {
			LOG_INF("Short press: ignored (not joined yet)");
		}
	} else {
		LOG_INF("Button released (no action)");
	}
}

static void button_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_reschedule(&debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

static int button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&reset_button)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&reset_button, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		return ret;
	}

	gpio_init_callback(&button_cb_data, button_callback, BIT(reset_button.pin));
	gpio_add_callback(reset_button.port, &button_cb_data);

	k_work_init_delayable(&debounce_work, debounce_handler);
	k_work_init_delayable(&long_press_work, long_press_handler);

	button_pressed_state = gpio_pin_get_dt(&reset_button);
	if (button_pressed_state) {
		atomic_set(&long_press_handled, 1);
		LOG_INF("Button held on boot, waiting for release");
	}
	return 0;
}
#endif

static void try_enable_usb_logs(void)
{
	int ret;
	const struct device *cdc_dev;
	uint32_t dtr = 0U;
	int tries = 0;

	ret = usb_enable(NULL);
	if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("usb_enable failed: %d", ret);
		return;
	}

	cdc_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(cdc_dev)) {
		return;
	}

	while (tries < 50 && dtr == 0U) {
		uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
		k_msleep(100);
		tries++;
	}

	if (dtr) {
		LOG_INF("USB terminal connected (DTR set)");
	}
}

static void confirm_running_image(void)
{
	int ret;

	if (running_image_confirmed) {
		return;
	}

	ret = boot_write_img_confirmed();
	if (ret < 0) {
		LOG_ERR("boot_write_img_confirmed failed: %d", ret);
		return;
	}

	running_image_confirmed = true;
	LOG_INF("Confirmed running MCUboot image after Zigbee join");
}

static void set_ota_transfer_mode(bool enabled)
{
    if (enabled) {
    		zigbee_configure_sleepy_behavior(false);
    		zb_zdo_pim_set_long_poll_interval(OTA_LONG_POLL_MS);
    		LOG_INF("OTA mode: FAST");
    	} else {
    		zigbee_configure_sleepy_behavior(true);
    		zb_zdo_pim_set_long_poll_interval(SED_LONG_POLL_MS);
    		LOG_INF("OTA mode: SLEEPY");
    	}
}

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
static void ota_evt_handler(const struct zigbee_fota_evt *evt)
{
	switch (evt->id) {
	case ZIGBEE_FOTA_EVT_PROGRESS:
		if (!ota_in_progress) {
			ota_in_progress = true;
			set_ota_transfer_mode(true);
		}
		LOG_INF("OTA progress: %d%%", evt->dl.progress);
		break;

	case ZIGBEE_FOTA_EVT_FINISHED:
		LOG_INF("OTA image ready, rebooting into MCUboot");
		sys_reboot(SYS_REBOOT_COLD);
		break;

	case ZIGBEE_FOTA_EVT_ERROR:
		LOG_ERR("OTA transfer failed");
		if (ota_in_progress) {
			ota_in_progress = false;
			set_ota_transfer_mode(false);
		}
		break;

	default:
		break;
	}
}
#endif

void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t *sig_hndler = NULL;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sig_hndler);
	zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
	zigbee_fota_signal_handler(bufid);
#endif

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
	case ZB_BDB_SIGNAL_STEERING:
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		if (status == RET_OK) {
			zigbee_network_ready = true;
			confirm_running_image();
			LOG_INF("Zigbee joined/rejoined (signal=%d), reporting each %ds",
				sig, REPORT_INTERVAL_S);
			ZB_SCHEDULE_APP_ALARM_CANCEL(measurement_periodic, 0);
			measurement_update(ZB_FALSE);
			ZB_SCHEDULE_APP_ALARM(measurement_periodic, 0,
					      (zb_time_t)REPORT_INTERVAL_S *
					      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
		} else {
			LOG_WRN("Zigbee signal %d status=%d", sig, status);
		}
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		zigbee_network_ready = false;
		LOG_WRN("Zigbee leave signal");
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		break;

	default:
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		break;
	}

	if (bufid) {
		zb_buf_free(bufid);
	}
}

int main(void)
{
	int ret;

	LOG_INF("Frostbee production app boot (OTA package test)");

	try_enable_usb_logs();

	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&vbat_enable)) {
		LOG_ERR("VBAT enable GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to set VBAT enable pin idle: %d", ret);
		return ret;
	}

	ret = adc_channel_setup(adc_dev, &adc_cfg);
	if (ret < 0) {
		LOG_ERR("ADC channel setup failed: %d", ret);
		return ret;
	}

	if (!device_is_ready(sht)) {
		LOG_ERR("SHT4X not ready");
		return -ENODEV;
	}

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
	ret = button_init();
	if (ret < 0) {
		LOG_ERR("Button init failed: %d", ret);
	}
#endif

	clusters_attr_init();

	zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN);
	set_ota_transfer_mode(false);

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
	ret = zigbee_fota_init(ota_evt_handler);
	if (ret < 0) {
		LOG_ERR("zigbee_fota_init failed: %d", ret);
		return ret;
	}
	ZB_ZCL_REGISTER_DEVICE_CB(zigbee_fota_zcl_cb);
#endif

	ZB_AF_REGISTER_DEVICE_CTX(&frostbee_ctx);
	running_image_confirmed = boot_is_img_confirmed();

	zigbee_enable();

	LOG_INF("Zigbee enabled (%s, %ds reporting)", FROSTBEE_SW_VERSION, REPORT_INTERVAL_S);

	while (1) {
		k_sleep(K_FOREVER);
	}
}
