/*
 * Frostbee - Zigbee Temperature & Humidity Sensor
 *
 * nRF52840 Dongle + Sensirion SHT40 via I2C
 * Zigbee Sleepy End Device with ZCL clusters:
 *   - Basic, Identify, Power Configuration
 *   - Temperature Measurement, Relative Humidity
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <ram_pwrdn.h>

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>
#include <zb_nrf_platform.h>
#include "zb_mem_config_custom.h"
#include "zb_frostbee.h"

LOG_MODULE_REGISTER(frostbee, LOG_LEVEL_INF);

/* Forward declarations */
static void sensor_periodic(zb_bufid_t bufid);
static void battery_periodic(zb_bufid_t bufid);
static void sensor_read(void);
static void battery_read(void);
static void sensor_and_battery_read(void);

/* Measurement intervals in seconds (used for ZBOSS alarm scheduling). */
#define SENSOR_READ_INTERVAL_S   600    /* 10 minutes - temp/humidity */
#define BATTERY_READ_INTERVAL_S  86400  /* 24 hours - battery voltage */

/* Reset button timing (milliseconds) */
#define BUTTON_DEBOUNCE_MS         100    /* Ignore edges within this window */
#define BUTTON_SHORT_PRESS_MAX_MS  1000   /* < 1s = short press (force sensor read) */
#define BUTTON_FACTORY_RESET_MS    5000   /* >= 5s = factory reset */

/* Reset button GPIO */
#define RESET_BUTTON_NODE DT_ALIAS(sw0)

/* Zigbee network persistence:
 * By default, network data is kept across reboots (no rejoin needed).
 * Factory reset (5s button press) triggers NVRAM erase on next boot.
 * The erase flag is stored in settings subsystem and survives reboot.
 */

/* Basic cluster metadata */
#define FROSTBEE_INIT_BASIC_APP_VERSION    1
#define FROSTBEE_INIT_BASIC_STACK_VERSION  10
#define FROSTBEE_INIT_BASIC_HW_VERSION     1
#define FROSTBEE_INIT_BASIC_MANUF_NAME     "Frostbee"
#define FROSTBEE_INIT_BASIC_MODEL_ID       "FBE_TH_1"
#define FROSTBEE_INIT_BASIC_DATE_CODE      "20250201"
#define FROSTBEE_INIT_BASIC_LOCATION_DESC  ""
#define FROSTBEE_INIT_BASIC_PH_ENV         ZB_ZCL_BASIC_ENV_UNSPECIFIED

/* Temperature measurement range: -40.00 C to +125.00 C (SHT40 spec) */
#define FROSTBEE_TEMP_MIN_VALUE  (-4000)
#define FROSTBEE_TEMP_MAX_VALUE  12500

/* Humidity measurement range: 0.00% to 100.00% */
#define FROSTBEE_HUM_MIN_VALUE   0
#define FROSTBEE_HUM_MAX_VALUE   10000

/* ─── Device context (ZCL attribute storage) ─── */

struct zb_device_ctx {
	zb_zcl_basic_attrs_ext_t basic_attr;
	zb_zcl_identify_attrs_t  identify_attr;

	/* Power configuration */
	zb_uint8_t battery_voltage;
	zb_uint8_t battery_percentage;
	zb_uint8_t battery_size;
	zb_uint8_t battery_quantity;
	zb_uint8_t battery_rated_voltage;
	zb_uint8_t battery_alarm_mask;
	zb_uint8_t battery_voltage_min_threshold;

	/* Temperature measurement */
	zb_int16_t  temp_measure_value;
	zb_int16_t  temp_min_value;
	zb_int16_t  temp_max_value;
	zb_uint16_t temp_tolerance;

	/* Humidity measurement */
	zb_uint16_t hum_measure_value;
	zb_uint16_t hum_min_value;
	zb_uint16_t hum_max_value;
};

static struct zb_device_ctx dev_ctx;

/* Sensor device handle */
static const struct device *sht;

/* ─── Battery voltage measurement ─── */

/* ADC configuration for P0.29 (AIN5) */
#define ADC_NODE        DT_NODELABEL(adc)
#define ADC_CHANNEL_ID  5
#define ADC_RESOLUTION  12
#define ADC_VREF_MV     600   /* Internal reference: 0.6V */
#define ADC_GAIN_FACTOR 6     /* Gain 1/6 means multiply by 6 */

/* Voltage divider: R1=10kΩ, R2=10kΩ (divides by 2) */
#define VDIV_FACTOR     2

static const struct device *adc_dev;

static struct adc_channel_cfg adc_cfg = {
	.gain = ADC_GAIN_1_6,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	.channel_id = ADC_CHANNEL_ID,
	.input_positive = SAADC_CH_PSELP_PSELP_AnalogInput5,  /* AIN5 = P0.29 */
};

static int16_t adc_sample_buffer;

static struct adc_sequence adc_seq = {
	.channels = BIT(ADC_CHANNEL_ID),
	.buffer = &adc_sample_buffer,
	.buffer_size = sizeof(adc_sample_buffer),
	.resolution = ADC_RESOLUTION,
};

/* GPIO to enable voltage divider (P0.02) - active LOW = connected to GND */
static const struct gpio_dt_spec vbat_enable = GPIO_DT_SPEC_GET(DT_NODELABEL(vbat_en), gpios);

/* Mutex to protect sensor access from concurrent reads */
static K_MUTEX_DEFINE(sensor_mutex);

/* Reset button */
#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;
static int64_t button_press_time;
static bool button_pressed_state;  /* Track logical state */
static bool long_press_handled;    /* Already triggered factory reset */
static struct k_work_delayable debounce_work;
static struct k_work_delayable factory_reset_work;

static void do_factory_reset(zb_uint8_t param)
{
	ARG_UNUSED(param);

	LOG_WRN("Factory reset - leaving network and erasing NVRAM");
	/* This function leaves the network, erases NVRAM, and reboots.
	 * It's called from ZBOSS context via ZB_SCHEDULE_APP_CALLBACK.
	 */
	zb_bdb_reset_via_local_action(param);
	/* Note: zb_bdb_reset_via_local_action will trigger a reboot internally */
}

static void factory_reset_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Check if button is still held */
	if (gpio_pin_get_dt(&reset_button) == 1) {
		long_press_handled = true;
		/* Schedule factory reset in ZBOSS context */
		ZB_SCHEDULE_APP_CALLBACK(do_factory_reset, 0);
	}
}

static void debounce_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Read the settled state after debounce period */
	int pressed = gpio_pin_get_dt(&reset_button);

	/* Only act if state actually changed */
	if (pressed == button_pressed_state) {
		return;
	}
	button_pressed_state = pressed;

	if (pressed) {
		/* Button pressed - record time and schedule factory reset check */
		button_press_time = k_uptime_get();
		long_press_handled = false;
		k_work_schedule(&factory_reset_work, K_MSEC(BUTTON_FACTORY_RESET_MS));
		LOG_INF("Button pressed - hold 5s for factory reset");
	} else {
		/* Button released - cancel factory reset, check for short press */
		k_work_cancel_delayable(&factory_reset_work);

		if (long_press_handled) {
			LOG_INF("Button released (monitoring now active)");
			long_press_handled = false;  /* Ready for next press */
		} else {
			int64_t hold_time = k_uptime_get() - button_press_time;

			if (hold_time < BUTTON_SHORT_PRESS_MAX_MS) {
				LOG_INF("Short press - forcing sensor+battery read");
				sensor_and_battery_read();
			} else {
				LOG_INF("Button released after %lld ms (no action)", hold_time);
			}
		}
	}
}

static void button_callback(const struct device *dev, struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* On any edge, schedule debounce check - it will read settled state */
	k_work_reschedule(&debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

static int button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&reset_button)) {
		LOG_ERR("Reset button GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&reset_button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure reset button: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("Failed to configure button interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&button_cb_data, button_callback,
			   BIT(reset_button.pin));
	gpio_add_callback(reset_button.port, &button_cb_data);

	k_work_init_delayable(&debounce_work, debounce_handler);
	k_work_init_delayable(&factory_reset_work, factory_reset_handler);

	/* Initialize state from current pin level.
	 * If button is pressed on boot (e.g., still held from factory reset),
	 * wait for release before monitoring to avoid spurious actions.
	 */
	button_pressed_state = gpio_pin_get_dt(&reset_button);

	LOG_INF("Reset button ready on P0.31 (initial state: %s)",
		button_pressed_state ? "pressed" : "released");

	if (button_pressed_state) {
		LOG_INF("Button held on boot - waiting for release before monitoring");
		/* Wait for button to be released before starting normal monitoring */
		long_press_handled = true;  /* Suppress any actions until released */
	}

	return 0;
}
#endif /* DT_NODE_EXISTS(RESET_BUTTON_NODE) */

/* ─── ZCL attribute lists ─── */

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

ZB_ZCL_DECLARE_IDENTIFY_CLIENT_ATTRIB_LIST(
	identify_client_attr_list);

ZB_ZCL_DECLARE_IDENTIFY_SERVER_ATTRIB_LIST(
	identify_server_attr_list,
	&dev_ctx.identify_attr.identify_time);

/* Custom power config attribute list with batteryPercentageRemaining
 * Using raw attribute descriptors since ZBOSS macros require bat_num suffix
 */
zb_zcl_attr_t power_config_attr_list[] = {
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_voltage
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY | ZB_ZCL_ATTR_ACCESS_REPORTING,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_percentage
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID,
		ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_size
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_quantity
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_rated_voltage
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_MASK_ID,
		ZB_ZCL_ATTR_TYPE_8BITMAP,
		ZB_ZCL_ATTR_ACCESS_READ_WRITE,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_alarm_mask
	},
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_MIN_THRESHOLD_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_WRITE,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
		(void *)&dev_ctx.battery_voltage_min_threshold
	},
	{
		ZB_ZCL_NULL_ID,
		0,
		0,
		(ZB_ZCL_NON_MANUFACTURER_SPECIFIC),
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

/* ─── Cluster list, endpoint, device context ─── */

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

ZBOSS_DECLARE_DEVICE_CTX_1_EP(
	frostbee_ctx,
	frostbee_ep);

/* ─── Attribute initialization ─── */

static void clusters_attr_init(void)
{
	/* Basic cluster */
	dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.app_version = FROSTBEE_INIT_BASIC_APP_VERSION;
	dev_ctx.basic_attr.stack_version = FROSTBEE_INIT_BASIC_STACK_VERSION;
	dev_ctx.basic_attr.hw_version = FROSTBEE_INIT_BASIC_HW_VERSION;

	ZB_ZCL_SET_STRING_VAL(
		dev_ctx.basic_attr.mf_name,
		FROSTBEE_INIT_BASIC_MANUF_NAME,
		ZB_ZCL_STRING_CONST_SIZE(FROSTBEE_INIT_BASIC_MANUF_NAME));

	ZB_ZCL_SET_STRING_VAL(
		dev_ctx.basic_attr.model_id,
		FROSTBEE_INIT_BASIC_MODEL_ID,
		ZB_ZCL_STRING_CONST_SIZE(FROSTBEE_INIT_BASIC_MODEL_ID));

	ZB_ZCL_SET_STRING_VAL(
		dev_ctx.basic_attr.date_code,
		FROSTBEE_INIT_BASIC_DATE_CODE,
		ZB_ZCL_STRING_CONST_SIZE(FROSTBEE_INIT_BASIC_DATE_CODE));

	dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;

	ZB_ZCL_SET_STRING_VAL(
		dev_ctx.basic_attr.location_id,
		FROSTBEE_INIT_BASIC_LOCATION_DESC,
		ZB_ZCL_STRING_CONST_SIZE(FROSTBEE_INIT_BASIC_LOCATION_DESC));

	dev_ctx.basic_attr.ph_env = FROSTBEE_INIT_BASIC_PH_ENV;

	/* Identify cluster */
	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	/* Power configuration — 3× AA batteries in series */
	dev_ctx.battery_voltage = 45;  /* 4.5V in units of 100mV (fresh batteries) */
	dev_ctx.battery_percentage = 200; /* 100% (ZCL uses 0.5% units, so 200 = 100%) */
	dev_ctx.battery_size = ZB_ZCL_POWER_CONFIG_BATTERY_SIZE_AA;
	dev_ctx.battery_quantity = 3;  /* 3× AA in series */
	dev_ctx.battery_rated_voltage = 15; /* 1.5V per cell in units of 100mV */
	dev_ctx.battery_alarm_mask = 0;
	dev_ctx.battery_voltage_min_threshold = 30; /* 3.0V alarm threshold (1.0V per cell) */

	/* Temperature measurement */
	dev_ctx.temp_measure_value = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.temp_min_value = FROSTBEE_TEMP_MIN_VALUE;
	dev_ctx.temp_max_value = FROSTBEE_TEMP_MAX_VALUE;
	dev_ctx.temp_tolerance = 20; /* 0.2 C tolerance (SHT40 typical accuracy) */

	/* Humidity measurement */
	dev_ctx.hum_measure_value = ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.hum_min_value = FROSTBEE_HUM_MIN_VALUE;
	dev_ctx.hum_max_value = FROSTBEE_HUM_MAX_VALUE;
}

/* ─── Battery voltage measurement ─── */

/* Compare function for qsort - ascending order */
static int compare_int16(const void *a, const void *b)
{
	return (*(int16_t *)a - *(int16_t *)b);
}

/* Read battery voltage via ADC with voltage divider enable/disable.
 *
 * Circuit: BAT+ → R1(10kΩ) → [P0.29/ADC] → R2(10kΩ) → [P0.02/GPIO] → GND
 *                                        └→ C(0.1µF) → GND
 *
 * Power saving: P0.02 configured as INPUT (high-Z) when not measuring.
 *               Only set to OUTPUT LOW when reading ADC (enables divider).
 *
 * Measurement strategy: Take 5 samples, remove min/max, average middle 3.
 * This eliminates ADC noise and transient spikes.
 *
 * Returns battery voltage in ZCL format (units of 100mV), or 0 on error.
 */
static uint8_t read_battery_voltage(void)
{
	int ret;
	int16_t samples[5];

	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC device not ready");
		return 0;
	}

	/* Enable voltage divider: set P0.02 as OUTPUT LOW (connects R2 to GND) */
	ret = gpio_pin_configure_dt(&vbat_enable, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to enable voltage divider: %d", ret);
		return 0;
	}

	/* Wait for capacitor to charge and voltage to stabilize
	 * RC = 10kΩ × 0.1µF = 1ms, wait 2ms to be safe
	 */
	k_msleep(2);

	/* Take 5 ADC samples */
	for (int i = 0; i < 5; i++) {
		ret = adc_read(adc_dev, &adc_seq);
		if (ret < 0) {
			LOG_ERR("ADC read %d failed: %d", i, ret);
			gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);
			return 0;
		}
		samples[i] = adc_sample_buffer;

		/* Small delay between samples to allow ADC to settle */
		if (i < 4) {
			k_usleep(500);  /* 500µs between samples */
		}
	}

	/* Disable voltage divider: set P0.02 as INPUT (high impedance, ~0µA) */
	gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);

	/* Sort samples to find min/max */
	qsort(samples, 5, sizeof(int16_t), compare_int16);

	/* Average the middle 3 values (exclude min=samples[0] and max=samples[4]) */
	int32_t sum = samples[1] + samples[2] + samples[3];
	int16_t avg_sample = sum / 3;

	LOG_DBG("ADC samples: [%d, %d, %d, %d, %d] → avg of middle 3: %d",
		samples[0], samples[1], samples[2], samples[3], samples[4], avg_sample);

	/* Convert ADC value to millivolts at ADC pin (P0.29)
	 * Formula: mV = (sample × VREF_mV × GAIN_FACTOR) / (2^12 - 1)
	 * Example: sample=2048 → (2048 × 600 × 6) / 4095 = 1800mV
	 */
	int32_t adc_mv = ((int32_t)avg_sample * ADC_VREF_MV * ADC_GAIN_FACTOR) / 4095;

	/* Actual battery voltage (voltage divider is 1:2, so multiply by 2) */
	int32_t battery_mv = adc_mv * VDIV_FACTOR;

	/* Convert to ZCL format: units of 100mV */
	uint8_t battery_zcl = (uint8_t)(battery_mv / 100);

	/* Calculate battery percentage (linear approximation for 3× AA batteries):
	 * 4.5V (fresh) = 100%, 3.0V (depleted) = 0%
	 * ZCL uses 0.5% units, so 200 = 100%, 0 = 0%
	 */
	int32_t percentage_raw = ((battery_mv - 3000) * 200) / 1500;
	if (percentage_raw < 0) {
		percentage_raw = 0;
	}
	if (percentage_raw > 200) {
		percentage_raw = 200;
	}
	uint8_t battery_pct = (uint8_t)percentage_raw;

	LOG_INF("Battery: %d mV (ZCL=%u), %u%% (ZCL=%u)",
		battery_mv, battery_zcl,
		battery_pct / 2, battery_pct);

	/* Update device context */
	dev_ctx.battery_voltage = battery_zcl;
	dev_ctx.battery_percentage = battery_pct;

	return battery_zcl;
}

/* ─── Sensor reading & ZCL attribute update ─── */

/* Read temperature/humidity sensor and update ZCL attributes.
 * Thread-safe via mutex - can be called from button or timer context.
 */
static void sensor_read(void)
{
	struct sensor_value temp, hum;
	int ret;

	k_mutex_lock(&sensor_mutex, K_FOREVER);

	if (!device_is_ready(sht)) {
		LOG_ERR("SHT4X not ready, skipping read");
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	ret = sensor_sample_fetch(sht);
	if (ret) {
		LOG_ERR("Sensor fetch failed: %d", ret);
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &hum);

	/* Convert to ZCL format:
	 * Temperature: signed int16 in units of 0.01 C
	 * Humidity: unsigned int16 in units of 0.01 %RH
	 */
	zb_int16_t temp_zcl = (zb_int16_t)(temp.val1 * 100 +
					    temp.val2 / 10000);
	zb_uint16_t hum_zcl = (zb_uint16_t)(hum.val1 * 100 +
					     hum.val2 / 10000);

	LOG_INF("T: %d.%02d C (%d)  H: %d.%02d %%RH (%u)",
		temp.val1, temp.val2 / 10000, temp_zcl,
		hum.val1, hum.val2 / 10000, hum_zcl);

	/* Update ZCL attributes — ZB_FALSE just stores the value.
	 * The ZBOSS reporting engine sends reports automatically
	 * based on the coordinator's Configure Reporting thresholds
	 * (min/max interval, reportable change).
	 */
	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
		(zb_uint8_t *)&temp_zcl,
		ZB_FALSE);

	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
		(zb_uint8_t *)&hum_zcl,
		ZB_FALSE);

	k_mutex_unlock(&sensor_mutex);
}

/* Read battery voltage and update ZCL attributes.
 * Thread-safe - can be called from button or timer context.
 */
static void battery_read(void)
{
	/* Read battery voltage via ADC */
	read_battery_voltage();

	/* Update battery ZCL attributes */
	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		(zb_uint8_t *)&dev_ctx.battery_voltage,
		ZB_FALSE);

	ZB_ZCL_SET_ATTRIBUTE(
		FROSTBEE_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		(zb_uint8_t *)&dev_ctx.battery_percentage,
		ZB_FALSE);
}

/* Read both sensor and battery (used by button handler for on-demand reads).
 * Does not affect periodic timers.
 */
static void sensor_and_battery_read(void)
{
	sensor_read();
	battery_read();
}

/* Periodic sensor read callback (called by Zigbee alarm scheduler). */
static void sensor_periodic(zb_bufid_t bufid)
{
	ARG_UNUSED(bufid);

	sensor_read();

	/* Schedule next periodic sensor read */
	ZB_SCHEDULE_APP_ALARM(sensor_periodic, 0,
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(
				      SENSOR_READ_INTERVAL_S * 1000));
}

/* Periodic battery read callback (called by Zigbee alarm scheduler). */
static void battery_periodic(zb_bufid_t bufid)
{
	ARG_UNUSED(bufid);

	battery_read();

	/* Schedule next periodic battery read.
	 * NOTE: Cannot pass BATTERY_READ_INTERVAL_S * 1000 directly to
	 * ZB_MILLISECONDS_TO_BEACON_INTERVAL because the macro multiplies by
	 * 1000 internally (ms→µs), causing 86400*1000*1000 = 86.4e9 to overflow
	 * uint32_t (~4.29e9 max), which wraps to ~500 s instead of 24 h.
	 * Fix: compute as seconds × beacons_per_second to stay in range.
	 */
	ZB_SCHEDULE_APP_ALARM(battery_periodic, 0,
			      (zb_time_t)BATTERY_READ_INTERVAL_S *
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
}

/* ─── Zigbee signal handler ─── */

void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t *sig_hndler = NULL;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sig_hndler);
	zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
		/* fall-through */
	case ZB_BDB_SIGNAL_STEERING:
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		if (status == RET_OK) {
			LOG_INF("Joined network, starting periodic reads");
			/* Start periodic sensor reading (temp/humidity every 10 min) */
			ZB_SCHEDULE_APP_ALARM(sensor_periodic, 0,
					      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
			/* Start periodic battery reading (voltage every 24h) */
			ZB_SCHEDULE_APP_ALARM(battery_periodic, 0,
					      ZB_MILLISECONDS_TO_BEACON_INTERVAL(1000));
		}
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		/* Factory reset triggered - reboot after leaving network */
#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
		if (long_press_handled) {
			LOG_WRN("Left network after factory reset, rebooting...");
			k_msleep(100);
			sys_reboot(SYS_REBOOT_COLD);
		}
#endif
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		break;

	case ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
		/* Production config partition is empty - this is normal,
		 * we don't use install codes or pre-shared keys. */
		break;

	case ZB_SIGNAL_JOIN_DONE:
		/* Certification testing signal - ignore. */
		break;

	default:
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		break;
	}

	if (bufid) {
		zb_buf_free(bufid);
	}
}

/* ─── Main ─── */

int main(void)
{
	LOG_INF("Frostbee starting - Zigbee SHT40 sensor");

	/* Get sensor device handle */
	sht = DEVICE_DT_GET_ANY(sensirion_sht4x);
	if (sht == NULL) {
		LOG_ERR("SHT4X device not found in devicetree");
		return -ENODEV;
	}
	if (!device_is_ready(sht)) {
		LOG_ERR("SHT4X device not ready");
		return -ENODEV;
	}
	LOG_INF("SHT40 sensor ready");

	/* Initialize ADC for battery voltage measurement */
	adc_dev = DEVICE_DT_GET(ADC_NODE);
	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC device not ready");
		return -ENODEV;
	}

	if (adc_channel_setup(adc_dev, &adc_cfg) < 0) {
		LOG_ERR("ADC channel setup failed");
		return -EIO;
	}
	LOG_INF("ADC ready on P0.29 (AIN5) for battery voltage");

	/* Initialize GPIO for voltage divider control (P0.02)
	 * Start as INPUT (high-Z) to save power - divider is OFF by default
	 */
	if (!gpio_is_ready_dt(&vbat_enable)) {
		LOG_ERR("Battery enable GPIO not ready");
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT) < 0) {
		LOG_ERR("Failed to configure battery enable GPIO");
		return -EIO;
	}
	LOG_INF("Battery voltage divider control ready on P0.02 (default: OFF)");

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
	if (button_init() < 0) {
		LOG_WRN("Reset button init failed - continuing without it");
	}
#endif

	/* Configure as sleepy end device */
	zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN);
	zb_set_keepalive_timeout(ZB_MILLISECONDS_TO_BEACON_INTERVAL(3000));
	zigbee_configure_sleepy_behavior(true);

	/* Power down unused RAM */
	if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY)) {
		power_down_unused_ram();
	}

	/* Register device context and initialize attributes */
	ZB_AF_REGISTER_DEVICE_CTX(&frostbee_ctx);
	clusters_attr_init();

	/* Start Zigbee stack */
	zigbee_enable();

	LOG_INF("Frostbee Zigbee stack started");

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
