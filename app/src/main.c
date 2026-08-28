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

#define SENSOR_READ_INTERVAL_S   600
#define BATTERY_READ_INTERVAL_S  64800
#define SED_LONG_POLL_MS       3000
#define OTA_LONG_POLL_MS         500

#define RECOVERY_QUEUE_RETRY_MS 30000
#define RECOVERY_BACKOFF_1_S    300
#define RECOVERY_BACKOFF_2_S    600
#define RECOVERY_BACKOFF_MAX_S  900

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
static atomic_t zigbee_network_ready;
static bool ota_in_progress;
static bool running_image_confirmed;
static struct k_work_delayable recovery_work;
static struct k_work_delayable periodic_rearm_work;
static atomic_t recovery_active;
static atomic_t recovery_callback_pending;
static atomic_t recovery_callback_running;
static atomic_t recovery_manual_kick;
static atomic_t recovery_epoch;
static atomic_t recovery_queued_epoch;
static atomic_t factory_reset_active;
static uint32_t recovery_attempt;
static uint32_t recovery_backoff_s = RECOVERY_BACKOFF_1_S;
static int64_t recovery_last_request_ms;
static uint32_t periodic_rearm_retries;

struct measurement_snapshot {
	zb_int16_t temperature;
	zb_uint16_t humidity;
	zb_uint8_t battery_voltage;
	zb_uint8_t battery_percentage;
};

enum button_report_frame {
	BUTTON_REPORT_FRAME_TEMPERATURE,
	BUTTON_REPORT_FRAME_HUMIDITY,
	BUTTON_REPORT_FRAME_POWER_CONFIG,
};

struct button_report_transaction {
	struct measurement_snapshot values;
	uint32_t sequence_id;
	uint32_t pending;
	uint32_t requested;
	uint32_t measurement_failed;
	uint32_t queued;
	uint32_t completed;
	uint32_t failed;
	uint32_t cancelled;
	uint32_t rerouted_to_recovery;
	enum button_report_frame frame;
	bool active;
};

static struct button_report_transaction button_report;

enum recovery_initial_trigger {
	RECOVERY_INITIAL_DEFAULT_HELPER,
	RECOVERY_INITIAL_PARENT_LOSS,
	RECOVERY_INITIAL_MANUAL,
};

static void recovery_manual_request(void);
static void recovery_end(const char *reason);
static void recovery_enter(const char *reason, enum recovery_initial_trigger initial);
static void short_button_action_cb(zb_uint8_t param);
static void periodic_rearm_retry(zb_bufid_t bufid);
static void periodic_rearm_work_handler(struct k_work *work);
static void button_report_start_next(void);
static void set_ota_poll_mode(bool enabled);

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
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.date_code, "20260827", 8);
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
    dev_ctx.battery_series_count = 2;
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
    if (series_count < 1 || series_count > 4) {
        LOG_WRN("Series count %u out of range (1-4). Defaulting to 1.", series_count);
        series_count = 1;
    }

    int32_t mv_per_cell = battery_mv / series_count;
    *battery_pct_zcl = calculate_pct_from_lookup(mv_per_cell, dev_ctx.battery_type);

	LOG_INF("Battery: %d mV (ZCL=%u), %u%% (ZCL=%u)",
		battery_mv, *battery_zcl, *battery_pct_zcl / 2, *battery_pct_zcl);

	return 0;
}

static zb_zcl_status_t set_measurement_attribute(zb_uint16_t cluster_id,
						 zb_uint16_t attr_id, void *value)
{
	return zb_zcl_set_attr_val(FROSTBEE_ENDPOINT, cluster_id,
		ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, value, ZB_FALSE);
}

static bool measurement_attributes_present(void)
{
	return zb_zcl_get_attr_desc_a(FROSTBEE_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID) != NULL &&
		zb_zcl_get_attr_desc_a(FROSTBEE_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID) != NULL &&
		zb_zcl_get_attr_desc_a(FROSTBEE_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID) != NULL &&
		zb_zcl_get_attr_desc_a(FROSTBEE_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID) != NULL;
}

static int commit_sensor_values(zb_int16_t temperature, zb_uint16_t humidity)
{
	zb_int16_t old_temperature = dev_ctx.temp_measure_value;
	zb_uint16_t old_humidity = dev_ctx.hum_measure_value;
	zb_zcl_status_t temp_status;
	zb_zcl_status_t humidity_status;
	zb_zcl_status_t rollback_status;

	temp_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &temperature);
	humidity_status = set_measurement_attribute(
		ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &humidity);
	if (temp_status == ZB_ZCL_STATUS_SUCCESS &&
	    humidity_status == ZB_ZCL_STATUS_SUCCESS) {
		return 0;
	}

	LOG_ERR("Sensor attribute commit failed: temperature=%u humidity=%u",
		temp_status, humidity_status);
	rollback_status = set_measurement_attribute(
		ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &old_temperature);
	LOG_WRN("Sensor rollback: temperature=%u", rollback_status);
	rollback_status = set_measurement_attribute(
		ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &old_humidity);
	LOG_WRN("Sensor rollback: humidity=%u", rollback_status);
	return -EIO;
}

static int commit_battery_values(zb_uint8_t voltage, zb_uint8_t percentage)
{
	zb_uint8_t old_voltage = dev_ctx.battery_voltage;
	zb_uint8_t old_percentage = dev_ctx.battery_percentage;
	zb_zcl_status_t voltage_status;
	zb_zcl_status_t percentage_status;
	zb_zcl_status_t rollback_status;

	voltage_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &voltage);
	percentage_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &percentage);
	if (voltage_status == ZB_ZCL_STATUS_SUCCESS &&
	    percentage_status == ZB_ZCL_STATUS_SUCCESS) {
		return 0;
	}

	LOG_ERR("Battery attribute commit failed: voltage=%u percentage=%u",
		voltage_status, percentage_status);
	rollback_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &old_voltage);
	LOG_WRN("Battery rollback: voltage=%u", rollback_status);
	rollback_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &old_percentage);
	LOG_WRN("Battery rollback: percentage=%u", rollback_status);
	return -EIO;
}

static int commit_measurement_snapshot(const struct measurement_snapshot *values)
{
	struct measurement_snapshot old = {
		.temperature = dev_ctx.temp_measure_value,
		.humidity = dev_ctx.hum_measure_value,
		.battery_voltage = dev_ctx.battery_voltage,
		.battery_percentage = dev_ctx.battery_percentage,
	};
	zb_zcl_status_t status[4];
	zb_zcl_status_t rollback_status;

	if (!measurement_attributes_present()) {
		LOG_ERR("Button report attribute descriptor validation failed");
		return -ENOENT;
	}

	status[0] = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, (void *)&values->temperature);
	status[1] = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, (void *)&values->humidity);
	status[2] = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		(void *)&values->battery_voltage);
	status[3] = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		(void *)&values->battery_percentage);

	LOG_INF("Button report %u attribute commits: temp=%u hum=%u voltage=%u percentage=%u",
		button_report.sequence_id, status[0], status[1], status[2], status[3]);
	if (status[0] == ZB_ZCL_STATUS_SUCCESS &&
	    status[1] == ZB_ZCL_STATUS_SUCCESS &&
	    status[2] == ZB_ZCL_STATUS_SUCCESS &&
	    status[3] == ZB_ZCL_STATUS_SUCCESS) {
		return 0;
	}

	rollback_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &old.temperature);
	LOG_WRN("Button report %u rollback temperature=%u",
		button_report.sequence_id, rollback_status);
	rollback_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &old.humidity);
	LOG_WRN("Button report %u rollback humidity=%u",
		button_report.sequence_id, rollback_status);
	rollback_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &old.battery_voltage);
	LOG_WRN("Button report %u rollback voltage=%u",
		button_report.sequence_id, rollback_status);
	rollback_status = set_measurement_attribute(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		&old.battery_percentage);
	LOG_WRN("Button report %u rollback percentage=%u",
		button_report.sequence_id, rollback_status);
	return -EIO;
}

static int sensor_report(void)
{
	zb_int16_t temperature;
	zb_uint16_t humidity;
	int ret;

	k_mutex_lock(&sensor_mutex, K_FOREVER);
	ret = read_sensor_once(&temperature, &humidity);
	if (ret < 0) {
		LOG_ERR("Sensor read failed: %d", ret);
	} else {
		ret = commit_sensor_values(temperature, humidity);
	}
	k_mutex_unlock(&sensor_mutex);
	return ret;
}

static int battery_report(void)
{
	zb_uint8_t voltage;
	zb_uint8_t percentage;
	int ret;

	k_mutex_lock(&sensor_mutex, K_FOREVER);
	ret = read_battery_once(&voltage, &percentage);
	if (ret < 0) {
		LOG_ERR("Battery read failed: %d", ret);
	} else {
		ret = commit_battery_values(voltage, percentage);
	}
	k_mutex_unlock(&sensor_mutex);
	return ret;
}

static void measurement_update(void)
{
	(void)sensor_report();
	(void)battery_report();
}

static void measurement_now_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);
	measurement_update();
}

static void button_report_log_counters(void)
{
	uint32_t accounted = button_report.completed +
		button_report.measurement_failed + button_report.failed +
		button_report.cancelled + button_report.rerouted_to_recovery +
		button_report.pending + (button_report.active ? 1U : 0U);

	LOG_INF("Button report counters: requested=%u measurement_failed=%u queued=%u completed=%u failed=%u cancelled=%u rerouted=%u pending=%u in_flight=%u",
		button_report.requested, button_report.measurement_failed,
		button_report.queued, button_report.completed, button_report.failed,
		button_report.cancelled, button_report.rerouted_to_recovery,
		button_report.pending, button_report.active ? 1U : 0U);
	if (accounted != button_report.requested) {
		LOG_ERR("Button report accounting mismatch: requested=%u accounted=%u",
			button_report.requested, accounted);
	}
}

static zb_ret_t button_report_submit_frame(enum button_report_frame frame);

static void button_report_send_cb(zb_uint8_t bufid)
{
	zb_zcl_command_send_status_t *send_status;
	zb_ret_t status;

	if (bufid == ZB_BUF_INVALID) {
		status = ZB_ZCL_STATUS_ABORT;
	} else {
		send_status = ZB_BUF_GET_PARAM(bufid, zb_zcl_command_send_status_t);
		status = send_status->status;
		zb_buf_free(bufid);
	}

	LOG_INF("Button report %u frame=%u callback status=%d",
		button_report.sequence_id, button_report.frame, status);
	if (!button_report.active) {
		LOG_WRN("Ignoring stale button report callback");
		return;
	}
	if (status != RET_OK) {
		LOG_ERR("Button report %u routing failed: frame=%u status=%d",
			button_report.sequence_id, button_report.frame, status);
		button_report.failed++;
		button_report.active = false;
		button_report_log_counters();
		button_report_start_next();
		return;
	}

	if (button_report.frame == BUTTON_REPORT_FRAME_POWER_CONFIG) {
		button_report.completed++;
		button_report.active = false;
		LOG_INF("Button report %u completed: 4 attributes in 3 frames",
			button_report.sequence_id);
		button_report_log_counters();
		button_report_start_next();
		return;
	}

	button_report.frame++;
	status = button_report_submit_frame(button_report.frame);
	if (status != RET_OK) {
		button_report.failed++;
		button_report.active = false;
		button_report_log_counters();
		button_report_start_next();
	}
}

static zb_ret_t button_report_submit_frame(enum button_report_frame frame)
{
	zb_bufid_t bufid;
	zb_uint8_t *cmd_ptr;
	zb_uint16_t destination = 0;
	zb_uint16_t cluster_id;
	zb_ret_t ret;

	bufid = zb_buf_get_out();
	if (bufid == ZB_BUF_INVALID) {
		LOG_ERR("Button report %u frame=%u allocation failed",
			button_report.sequence_id, frame);
		return RET_NO_MEMORY;
	}

	cmd_ptr = ZB_ZCL_START_PACKET(bufid);
	ZB_ZCL_CONSTRUCT_GENERAL_COMMAND_REQ_FRAME_CONTROL_A(cmd_ptr,
		ZB_ZCL_FRAME_DIRECTION_TO_CLI,
		ZB_ZCL_NOT_MANUFACTURER_SPECIFIC,
		ZB_ZCL_ENABLE_DEFAULT_RESPONSE);
	ZB_ZCL_CONSTRUCT_COMMAND_HEADER(cmd_ptr, ZB_ZCL_GET_SEQ_NUM(),
		ZB_ZCL_CMD_REPORT_ATTRIB);

	switch (frame) {
	case BUTTON_REPORT_FRAME_TEMPERATURE:
		cluster_id = ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
		ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr,
			ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID);
		ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_ATTR_TYPE_S16);
		ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr,
			(zb_uint16_t)button_report.values.temperature);
		break;
	case BUTTON_REPORT_FRAME_HUMIDITY:
		cluster_id = ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
		ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr,
			ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
		ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_ATTR_TYPE_U16);
		ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, button_report.values.humidity);
		break;
	case BUTTON_REPORT_FRAME_POWER_CONFIG:
		cluster_id = ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
		ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr,
			ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID);
		ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_ATTR_TYPE_U8);
		ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, button_report.values.battery_voltage);
		ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr,
			ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);
		ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, ZB_ZCL_ATTR_TYPE_U8);
		ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, button_report.values.battery_percentage);
		break;
	default:
		zb_buf_free(bufid);
		return RET_INVALID_PARAMETER;
	}

	button_report.frame = frame;
	ret = zb_zcl_finish_and_send_packet(bufid, cmd_ptr,
		(const zb_addr_u *)(const void *)&destination,
		ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT, 0,
		FROSTBEE_ENDPOINT, ZB_AF_HA_PROFILE_ID, cluster_id,
		button_report_send_cb);
	LOG_INF("Button report %u frame=%u cluster=0x%04x submit=%d",
		button_report.sequence_id, frame, cluster_id, ret);
	if (ret != RET_OK) {
		/* Ownership transfers only when the stack accepts the send. */
		zb_buf_free(bufid);
		return ret;
	}
	if (frame == BUTTON_REPORT_FRAME_POWER_CONFIG) {
		button_report.queued++;
		LOG_INF("Button report %u queued: 4 attributes in 3 frames",
			button_report.sequence_id);
	}
	return RET_OK;
}

static bool button_report_begin(void)
{
	int sensor_ret;
	int battery_ret;
	zb_ret_t send_ret;

	button_report.sequence_id++;
	button_report.active = true;
	k_mutex_lock(&sensor_mutex, K_FOREVER);
	sensor_ret = read_sensor_once(&button_report.values.temperature,
		&button_report.values.humidity);
	battery_ret = read_battery_once(&button_report.values.battery_voltage,
		&button_report.values.battery_percentage);
	if (sensor_ret < 0 || battery_ret < 0) {
		k_mutex_unlock(&sensor_mutex);
		button_report.measurement_failed++;
		button_report.active = false;
		LOG_ERR("Button report %u measurement failed: sensor=%d battery=%d",
			button_report.sequence_id, sensor_ret, battery_ret);
		button_report_log_counters();
		return false;
	}

	LOG_INF("Button report %u measurement ready: temp=%d humidity=%u voltage=%u percentage=%u",
		button_report.sequence_id, button_report.values.temperature,
		button_report.values.humidity, button_report.values.battery_voltage,
		button_report.values.battery_percentage);
	if (commit_measurement_snapshot(&button_report.values) < 0) {
		k_mutex_unlock(&sensor_mutex);
		button_report.failed++;
		button_report.active = false;
		button_report_log_counters();
		return false;
	}
	k_mutex_unlock(&sensor_mutex);

	send_ret = button_report_submit_frame(BUTTON_REPORT_FRAME_TEMPERATURE);
	if (send_ret != RET_OK) {
		button_report.failed++;
		button_report.active = false;
		button_report_log_counters();
		return false;
	}
	return true;
}

static void button_report_start_next(void)
{
	while (!button_report.active && button_report.pending > 0) {
		button_report.pending--;
		if (atomic_get(&factory_reset_active) || zb_bdb_is_factory_new() ||
		    ota_in_progress) {
			button_report.cancelled++;
			LOG_INF("Queued button report cancelled by OTA/factory reset state");
			button_report_log_counters();
			continue;
		}
		if (atomic_get(&recovery_active) ||
		    !atomic_get(&zigbee_network_ready) || !ZB_JOINED()) {
			button_report.rerouted_to_recovery++;
			LOG_INF("Queued button report rerouted to recovery");
			if (!atomic_get(&recovery_active)) {
				recovery_enter("queued button report while network unavailable",
					RECOVERY_INITIAL_MANUAL);
			} else {
				recovery_manual_request();
			}
			button_report_log_counters();
			continue;
		}
		if (button_report_begin()) {
			return;
		}
	}
}

static void sensor_periodic(zb_bufid_t bufid)
{
	ARG_UNUSED(bufid);
	if (!atomic_get(&zigbee_network_ready) || atomic_get(&recovery_active)) {
		return;
	}
	if (!ota_in_progress) {
		(void)sensor_report();
	}
	ZB_SCHEDULE_APP_ALARM(sensor_periodic, 0,
			      (zb_time_t)SENSOR_READ_INTERVAL_S *
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
}

static void battery_periodic(zb_bufid_t bufid)
{
	ARG_UNUSED(bufid);
	if (!atomic_get(&zigbee_network_ready) || atomic_get(&recovery_active)) {
		return;
	}
	if (!ota_in_progress) {
		(void)battery_report();
	}
	ZB_SCHEDULE_APP_ALARM(battery_periodic, 0,
			      (zb_time_t)BATTERY_READ_INTERVAL_S *
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
}

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
static void do_factory_reset(zb_uint8_t param)
{
	LOG_WRN("Factory reset requested, leaving network and erasing NVRAM");
	atomic_set(&factory_reset_active, 1);

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
	if (ota_in_progress) {
		LOG_WRN("Aborting OTA because factory reset was requested");
		zigbee_fota_abort();
		set_ota_poll_mode(false);
	}
#endif

	recovery_end("factory reset");
	atomic_set(&zigbee_network_ready, 0);
	(void)ZB_SCHEDULE_APP_ALARM_CANCEL(sensor_periodic, 0);
	(void)ZB_SCHEDULE_APP_ALARM_CANCEL(battery_periodic, 0);
	k_work_cancel_delayable(&periodic_rearm_work);
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
		zb_ret_t ret = zigbee_schedule_callback(short_button_action_cb, 0);

		if (ret != RET_OK) {
			LOG_WRN("Short press callback queue failed: %d", ret);
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

static void set_ota_poll_mode(bool enabled)
{
	/* Runtime poll control is ZBOSS-owned; callers must run in ZBOSS context. */
	zb_time_t interval = enabled ? OTA_LONG_POLL_MS : SED_LONG_POLL_MS;

	ota_in_progress = enabled;
	zb_zdo_pim_set_long_poll_interval(interval);
	LOG_INF("OTA poll mode: enabled=%d interval=%ums", enabled, interval);
}

static void cancel_periodic_alarms(void)
{
	zb_ret_t ret;

	ret = ZB_SCHEDULE_APP_ALARM_CANCEL(sensor_periodic, 0);
	if (ret != RET_OK && ret != RET_NOT_FOUND) {
		LOG_WRN("Sensor alarm cancel failed: %d", ret);
	}
	ret = ZB_SCHEDULE_APP_ALARM_CANCEL(battery_periodic, 0);
	if (ret != RET_OK && ret != RET_NOT_FOUND) {
		LOG_WRN("Battery alarm cancel failed: %d", ret);
	}
	k_work_cancel_delayable(&periodic_rearm_work);
}

static void schedule_periodic_alarms(void);

static void periodic_rearm_retry(zb_bufid_t bufid)
{
	ARG_UNUSED(bufid);
	schedule_periodic_alarms();
}

static void schedule_periodic_alarms(void)
{
	zb_ret_t sensor_ret;
	zb_ret_t battery_ret;

	if (!atomic_get(&zigbee_network_ready) || atomic_get(&recovery_active)) {
		return;
	}

	(void)ZB_SCHEDULE_APP_ALARM_CANCEL(sensor_periodic, 0);
	(void)ZB_SCHEDULE_APP_ALARM_CANCEL(battery_periodic, 0);
	sensor_ret = ZB_SCHEDULE_APP_ALARM(sensor_periodic, 0,
		(zb_time_t)SENSOR_READ_INTERVAL_S * ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
	battery_ret = ZB_SCHEDULE_APP_ALARM(battery_periodic, 0,
		(zb_time_t)BATTERY_READ_INTERVAL_S * ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
	if (sensor_ret == RET_OK && battery_ret == RET_OK) {
		periodic_rearm_retries = 0;
		k_work_cancel_delayable(&periodic_rearm_work);
		return;
	}

	/* Keep the pair all-or-nothing so a retry cannot duplicate one alarm. */
	(void)ZB_SCHEDULE_APP_ALARM_CANCEL(sensor_periodic, 0);
	(void)ZB_SCHEDULE_APP_ALARM_CANCEL(battery_periodic, 0);
	periodic_rearm_retries++;
	LOG_WRN("Periodic alarm rearm failed: sensor=%d battery=%d retry=%u; retrying in %dms",
		sensor_ret, battery_ret, periodic_rearm_retries,
		RECOVERY_QUEUE_RETRY_MS);
	k_work_reschedule(&periodic_rearm_work, K_MSEC(RECOVERY_QUEUE_RETRY_MS));
}

static void periodic_rearm_work_handler(struct k_work *work)
{
	zb_ret_t ret;

	ARG_UNUSED(work);
	if (!atomic_get(&zigbee_network_ready) || atomic_get(&recovery_active)) {
		return;
	}

	ret = zigbee_schedule_callback(periodic_rearm_retry, 0);
	if (ret != RET_OK) {
		LOG_WRN("Periodic rearm callback queue failed: %d; retrying in %dms",
			ret, RECOVERY_QUEUE_RETRY_MS);
		k_work_reschedule(&periodic_rearm_work,
			K_MSEC(RECOVERY_QUEUE_RETRY_MS));
	}
}

static void recovery_schedule_next(uint32_t delay_s)
{
	if (atomic_get(&recovery_active)) {
		k_work_reschedule(&recovery_work, K_SECONDS(delay_s));
		LOG_INF("Recovery next burst in %us (epoch=%ld attempt=%u)", delay_s,
			(long)atomic_get(&recovery_epoch), recovery_attempt);
	}
}

static void recovery_note_trigger(const char *trigger)
{
	int64_t now = k_uptime_get();
	int64_t elapsed = recovery_last_request_ms ? now - recovery_last_request_ms : 0;

	recovery_attempt++;
	recovery_last_request_ms = now;
	LOG_WRN("Recovery trigger dispatched: %s epoch=%ld attempt=%u uptime_ms=%lld interval_ms=%lld",
		trigger, (long)atomic_get(&recovery_epoch), recovery_attempt, now, elapsed);
}

static void recovery_attempt_cb(zb_uint8_t param)
{
	atomic_val_t queued_epoch = atomic_get(&recovery_queued_epoch);
	bool manual;
	uint32_t next_delay = recovery_backoff_s;

	ARG_UNUSED(param);
	atomic_set(&recovery_callback_pending, 0);
	atomic_set(&recovery_callback_running, 1);

	if (!atomic_get(&recovery_active) || atomic_get(&factory_reset_active) ||
	    atomic_get(&zigbee_network_ready) || zb_bdb_is_factory_new() ||
	    queued_epoch != atomic_get(&recovery_epoch)) {
		LOG_INF("Ignoring stale recovery callback (queued=%ld current=%ld active=%ld ready=%d factory_new=%d reset=%ld)",
			(long)queued_epoch, (long)atomic_get(&recovery_epoch),
			(long)atomic_get(&recovery_active),
			(int)atomic_get(&zigbee_network_ready),
			zb_bdb_is_factory_new(), (long)atomic_get(&factory_reset_active));
		atomic_set(&recovery_callback_running, 0);
		return;
	}
	manual = atomic_set(&recovery_manual_kick, 0) != 0;

	if (ZB_JOINED()) {
		zb_bdb_initiate_tc_rejoin(ZB_UNDEFINED_BUFFER);
		recovery_note_trigger(manual ? "manual TC rejoin" : "TC rejoin");
	} else {
		user_input_indicate();
		recovery_note_trigger(manual ? "manual helper resume" : "helper resume");
	}

	if (!manual) {
		if (recovery_backoff_s == RECOVERY_BACKOFF_1_S) {
			recovery_backoff_s = RECOVERY_BACKOFF_2_S;
		} else {
			recovery_backoff_s = RECOVERY_BACKOFF_MAX_S;
		}
		next_delay = recovery_backoff_s;
	}

	atomic_set(&recovery_callback_running, 0);
	if (atomic_get(&recovery_manual_kick)) {
		/* A press raced with this callback after its atomic consume. */
		k_work_reschedule(&recovery_work, K_NO_WAIT);
	} else {
		recovery_schedule_next(next_delay);
	}
}

static void recovery_work_handler(struct k_work *work)
{
	zb_ret_t ret;
	atomic_val_t epoch;

	ARG_UNUSED(work);
	if (!atomic_get(&recovery_active) || atomic_get(&factory_reset_active) ||
	    atomic_get(&zigbee_network_ready)) {
		return;
	}
	if (atomic_get(&recovery_callback_running) ||
	    !atomic_cas(&recovery_callback_pending, 0, 1)) {
		k_work_reschedule(&recovery_work, K_MSEC(RECOVERY_QUEUE_RETRY_MS));
		return;
	}

	epoch = atomic_get(&recovery_epoch);
	atomic_set(&recovery_queued_epoch, epoch);
	ret = zigbee_schedule_callback(recovery_attempt_cb, 0);
	if (ret != RET_OK) {
		atomic_set(&recovery_callback_pending, 0);
		LOG_WRN("Recovery callback queue failed: %d; retrying in %dms", ret,
			RECOVERY_QUEUE_RETRY_MS);
		k_work_reschedule(&recovery_work, K_MSEC(RECOVERY_QUEUE_RETRY_MS));
		return;
	}
	LOG_INF("Recovery callback queued (epoch=%ld manual=%ld)", (long)epoch,
		(long)atomic_get(&recovery_manual_kick));
}

static void recovery_end(const char *reason)
{
	if (atomic_set(&recovery_active, 0)) {
		LOG_INF("Recovery ended: %s (epoch=%ld attempts=%u)", reason,
			(long)atomic_get(&recovery_epoch), recovery_attempt);
	}
	atomic_inc(&recovery_epoch);
	atomic_set(&recovery_manual_kick, 0);
	k_work_cancel_delayable(&recovery_work);
}

static void recovery_enter(const char *reason, enum recovery_initial_trigger initial)
{
	bool started;

	if (atomic_get(&factory_reset_active) || zb_bdb_is_factory_new()) {
		LOG_INF("Recovery suppressed: %s (factory-new/reset)", reason);
		return;
	}

	started = atomic_cas(&recovery_active, 0, 1);
	atomic_set(&zigbee_network_ready, 0);
	if (!started) {
		LOG_INF("Recovery already active: %s", reason);
		return;
	}

	atomic_inc(&recovery_epoch);
	atomic_set(&recovery_manual_kick, 0);
	recovery_attempt = 0;
	recovery_backoff_s = RECOVERY_BACKOFF_1_S;
	recovery_last_request_ms = 0;
	cancel_periodic_alarms();

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
	if (ota_in_progress) {
		LOG_WRN("Aborting OTA because Zigbee parent was lost");
		zigbee_fota_abort();
		set_ota_poll_mode(false);
	}
#endif

	LOG_WRN("Recovery started: %s epoch=%ld joined=%d factory_new=%d", reason,
		(long)atomic_get(&recovery_epoch), ZB_JOINED(), zb_bdb_is_factory_new());

	if (initial == RECOVERY_INITIAL_DEFAULT_HELPER) {
		recovery_note_trigger("SDK helper burst");
	} else if (ZB_JOINED()) {
		zb_bdb_initiate_tc_rejoin(ZB_UNDEFINED_BUFFER);
		recovery_note_trigger(initial == RECOVERY_INITIAL_MANUAL ?
			"manual TC rejoin" : "TC rejoin bootstrap");
	} else if (bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING)) {
		recovery_note_trigger(initial == RECOVERY_INITIAL_MANUAL ?
			"manual steering bootstrap" : "steering bootstrap");
	} else {
		/* The SDK helper may already own the current burst. */
		recovery_note_trigger("existing SDK helper burst");
	}

	recovery_schedule_next(recovery_backoff_s);
}

static void recovery_manual_request(void)
{
	atomic_set(&recovery_manual_kick, 1);
	k_work_cancel_delayable(&recovery_work);
	if (!atomic_get(&recovery_callback_pending) &&
	    !atomic_get(&recovery_callback_running)) {
		k_work_reschedule(&recovery_work, K_NO_WAIT);
	}
}

static void short_button_action_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);

	if (atomic_get(&factory_reset_active) || zb_bdb_is_factory_new()) {
		LOG_INF("Short press: ignored during factory-new/reset flow");
		return;
	}
	if (atomic_get(&recovery_active) || !atomic_get(&zigbee_network_ready) ||
	    !ZB_JOINED()) {
		if (!atomic_get(&recovery_active)) {
			recovery_enter("short press while network unavailable", RECOVERY_INITIAL_MANUAL);
		} else {
			LOG_INF("Short press: immediate recovery kick requested");
			recovery_manual_request();
		}
		return;
	}
	if (ota_in_progress) {
		LOG_INF("Short press: ignored during OTA transfer");
		return;
	}

	button_report.requested++;
	button_report.pending++;
	LOG_INF("Short press: button report requested=%u pending=%u",
		button_report.requested, button_report.pending);
	button_report_start_next();
}

static bool recovery_signal_success(zb_ret_t status)
{
	return status == RET_OK && ZB_JOINED() && !zb_bdb_is_factory_new();
}

static void network_ready_after_signal(zb_zdo_app_signal_type_t sig)
{
	bool recovery_was_active = atomic_get(&recovery_active);

	recovery_end("network ready");
	atomic_set(&factory_reset_active, 0);
	atomic_set(&zigbee_network_ready, 1);
	periodic_rearm_retries = 0;
	k_work_cancel_delayable(&periodic_rearm_work);
	set_ota_poll_mode(false);
	confirm_running_image();
	LOG_INF("Zigbee joined/rejoined (signal=%d), sensor=%us battery=%us", sig,
		SENSOR_READ_INTERVAL_S, BATTERY_READ_INTERVAL_S);
	if (recovery_was_active) {
		(void)sensor_report();
		(void)battery_report();
	} else {
		measurement_update();
	}
	schedule_periodic_alarms();
}

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
static void ota_evt_handler(const struct zigbee_fota_evt *evt)
{
	switch (evt->id) {
	case ZIGBEE_FOTA_EVT_PROGRESS:
		if (!ota_in_progress) {
			set_ota_poll_mode(true);
		}
		LOG_INF("OTA progress: %d%%", evt->dl.progress);
		break;

	case ZIGBEE_FOTA_EVT_FINISHED:
		LOG_INF("OTA image ready, rebooting into MCUboot");
		set_ota_poll_mode(false);
		sys_reboot(SYS_REBOOT_COLD);
		break;

	case ZIGBEE_FOTA_EVT_ERROR:
		LOG_ERR("OTA transfer failed");
		set_ota_poll_mode(false);
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
	zb_ret_t default_ret;

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
	zigbee_fota_signal_handler(bufid);
#endif

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
	case ZB_BDB_SIGNAL_STEERING:
	case ZB_BDB_SIGNAL_TC_REJOIN_DONE:
		default_ret = zigbee_default_signal_handler(bufid);
		LOG_INF("Zigbee lifecycle signal=%d status=%d default_handler=%d joined=%d factory_new=%d",
			sig, status, default_ret, ZB_JOINED(), zb_bdb_is_factory_new());
		if (recovery_signal_success(status)) {
			network_ready_after_signal(sig);
		} else {
			LOG_WRN("Zigbee lifecycle failed: signal=%d status=%d helper=%d",
				sig, status, default_ret);
			recovery_enter("failed Zigbee lifecycle signal",
				RECOVERY_INITIAL_DEFAULT_HELPER);
		}
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		atomic_set(&zigbee_network_ready, 0);
		default_ret = zigbee_default_signal_handler(bufid);
		LOG_WRN("Zigbee leave signal status=%d default_handler=%d reset=%ld factory_new=%d",
			status, default_ret, (long)atomic_get(&factory_reset_active),
			zb_bdb_is_factory_new());
		if (!atomic_get(&factory_reset_active) && !zb_bdb_is_factory_new()) {
			recovery_enter("unexpected leave", RECOVERY_INITIAL_DEFAULT_HELPER);
		}
		break;

	case ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
		default_ret = zigbee_default_signal_handler(bufid);
		LOG_WRN("No active parent links: status=%d default_handler=%d joined=%d",
			status, default_ret, ZB_JOINED());
		recovery_enter("no active parent links", RECOVERY_INITIAL_PARENT_LOSS);
		break;

	case ZB_NLME_STATUS_INDICATION: {
		zb_zdo_signal_nlme_status_indication_params_t *params =
			ZB_ZDO_SIGNAL_GET_PARAMS(sig_hndler,
				zb_zdo_signal_nlme_status_indication_params_t);

		default_ret = zigbee_default_signal_handler(bufid);
		LOG_WRN("NLME status=0x%02x address=0x%04x signal_status=%d default_handler=%d joined=%d",
			params->nlme_status.status, params->nlme_status.network_addr,
			status, default_ret, ZB_JOINED());
		if (params->nlme_status.status == ZB_NWK_COMMAND_STATUS_PARENT_LINK_FAILURE) {
			recovery_enter("NLME parent link failure", RECOVERY_INITIAL_PARENT_LOSS);
		} else {
			ZB_ERROR_CHECK(default_ret);
		}
		break;
	}

	default:
		default_ret = zigbee_default_signal_handler(bufid);
		ZB_ERROR_CHECK(default_ret);
		break;
	}

	if (bufid) {
		zb_buf_free(bufid);
	}
}

static void frostbee_zcl_cb(zb_uint8_t param)
{
    zb_zcl_device_callback_param_t *device_cb_param =
        ZB_BUF_GET_PARAM(param, zb_zcl_device_callback_param_t);

    /* Check if an attribute was written to over-the-air */
    if (device_cb_param->device_cb_id == ZB_ZCL_SET_ATTR_VALUE_CB_ID) {
        zb_zcl_set_attr_value_param_t *set_attr_param =
            &device_cb_param->cb_param.set_attr_value_param;

        /* Check if the write was to our Power Config cluster */
        if (set_attr_param->cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG) {

            /* If it's our Battery Type or Series Count */
            if (set_attr_param->attr_id == 0xff01 || set_attr_param->attr_id == 0xff02) {
                LOG_INF("Battery configuration changed (Attr: 0x%04x). Recalculating...",
                        set_attr_param->attr_id);

                /* Recalculate local measurement attributes in ZBOSS context.
                 * Explicit button-report sequencing is intentionally separate.
                 */
                ZB_SCHEDULE_APP_CALLBACK(measurement_now_cb, 0);
            }
        }
    }

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
    /* Ensure FOTA callback still gets processed if FOTA is enabled */
    zigbee_fota_zcl_cb(param);
#endif
}

int main(void)
{
	int ret;

	LOG_INF("Frostbee production app boot (OTA package test)");
	k_work_init_delayable(&recovery_work, recovery_work_handler);
	k_work_init_delayable(&periodic_rearm_work, periodic_rearm_work_handler);

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
	zigbee_configure_sleepy_behavior(true);

#if IS_ENABLED(CONFIG_ZIGBEE_FOTA)
	ret = zigbee_fota_init(ota_evt_handler);
	if (ret < 0) {
		LOG_ERR("zigbee_fota_init failed: %d", ret);
		return ret;
	}
#endif

    /* Register our custom callback to handle attribute writes */
    ZB_ZCL_REGISTER_DEVICE_CB(frostbee_zcl_cb);

	ZB_AF_REGISTER_DEVICE_CTX(&frostbee_ctx);
	running_image_confirmed = boot_is_img_confirmed();

	zigbee_enable();

	LOG_INF("Zigbee enabled (%s, sensor=%us battery=%us)",
		FROSTBEE_SW_VERSION, SENSOR_READ_INTERVAL_S, BATTERY_READ_INTERVAL_S);

	while (1) {
		k_sleep(K_FOREVER);
	}
}
