/*
 * Frostbee UF2 battlefield app
 *
 * Purpose:
 * - Validate sensor reads, battery measurement, and button logic
 * - Keep logs accessible over USB CDC ACM
 * - Stay fully independent from Zigbee/OTA until behavior is stable
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
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(frostbee_uf2, LOG_LEVEL_DBG);

/* Intervals */
#define SENSOR_READ_INTERVAL_S   600    /* 10 minutes */
#define BATTERY_READ_INTERVAL_S  64800  /* 18 hours */
#define BATTERY_TICK_DIV         (BATTERY_READ_INTERVAL_S / SENSOR_READ_INTERVAL_S)

/* Button timing */
#define BUTTON_DEBOUNCE_MS         100
#define BUTTON_SHORT_PRESS_MAX_MS  1000
#define BUTTON_FACTORY_RESET_MS    5000

/* ADC configuration for P0.29 (AIN5) */
#define ADC_NODE        DT_NODELABEL(adc)
#define ADC_CHANNEL_ID  5
#define ADC_RESOLUTION  12
#define ADC_VREF_MV     600
#define ADC_GAIN_FACTOR 6
#define VDIV_FACTOR     2

#define RESET_BUTTON_NODE DT_ALIAS(sw0)

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

static struct k_work_delayable measure_work;
static atomic_t force_measure_now;
static uint32_t periodic_tick;
static K_MUTEX_DEFINE(sensor_mutex);

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

static int compare_int16(const void *a, const void *b)
{
	return (*(int16_t *)a - *(int16_t *)b);
}

static void log_fixed2(const char *label, int32_t value)
{
	int32_t abs_value = value < 0 ? -value : value;
	LOG_INF("%s: %s%d.%02d", label, value < 0 ? "-" : "",
		abs_value / 100, abs_value % 100);
}

static void read_sensor_once(void)
{
	struct sensor_value temp;
	struct sensor_value hum;
	int ret;
	int64_t temp_micro;
	int64_t hum_micro;
	int32_t temp_centi;
	int32_t hum_centi;

	if (!device_is_ready(sht)) {
		LOG_ERR("SHT4X not ready");
		return;
	}

	k_mutex_lock(&sensor_mutex, K_FOREVER);

	ret = sensor_sample_fetch(sht);
	if (ret < 0) {
		LOG_ERR("sensor_sample_fetch failed: %d", ret);
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	ret = sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		LOG_ERR("TEMP channel read failed: %d", ret);
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	ret = sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret < 0) {
		LOG_ERR("HUM channel read failed: %d", ret);
		k_mutex_unlock(&sensor_mutex);
		return;
	}

	k_mutex_unlock(&sensor_mutex);

	temp_micro = (int64_t)temp.val1 * 1000000LL + temp.val2;
	hum_micro = (int64_t)hum.val1 * 1000000LL + hum.val2;
	temp_centi = (int32_t)(temp_micro / 10000);
	hum_centi = (int32_t)(hum_micro / 10000);

	log_fixed2("Temperature [C]", temp_centi);
	log_fixed2("Humidity [%]", hum_centi);
}

static void read_battery_once(void)
{
	int ret;
	int16_t samples[5];
	int32_t adc_mv;
	int32_t battery_mv;
	int32_t percentage_raw;
	uint8_t battery_zcl;
	uint8_t battery_pct;

	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC device not ready");
		return;
	}

	ret = gpio_pin_configure_dt(&vbat_enable, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to enable voltage divider: %d", ret);
		return;
	}

	k_msleep(2);

	for (int i = 0; i < 5; i++) {
		ret = adc_read(adc_dev, &adc_seq);
		if (ret < 0) {
			LOG_ERR("ADC read %d failed: %d", i, ret);
			gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);
			return;
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
	battery_zcl = (uint8_t)((battery_mv + 50) / 100);

	percentage_raw = ((battery_mv - 3000) * 200) / 1500;
	if (percentage_raw < 0) {
		percentage_raw = 0;
	}
	if (percentage_raw > 200) {
		percentage_raw = 200;
	}
	battery_pct = (uint8_t)percentage_raw;

	LOG_INF("Battery: %d mV (ZCL=%u), %u%% (ZCL=%u)",
		battery_mv, battery_zcl, battery_pct / 2, battery_pct);
}

static void measure_work_handler(struct k_work *work)
{
	bool force = atomic_get(&force_measure_now) != 0;

	ARG_UNUSED(work);

	if (force) {
		atomic_set(&force_measure_now, 0);
		LOG_INF("Forced measurement triggered by button");
		read_sensor_once();
		read_battery_once();
		return;
	}

	if (periodic_tick == 0U || (periodic_tick % BATTERY_TICK_DIV) == 0U) {
		read_battery_once();
	}

	read_sensor_once();
	periodic_tick++;

	k_work_schedule(&measure_work, K_SECONDS(SENSOR_READ_INTERVAL_S));
}

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
static void long_press_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (gpio_pin_get_dt(&reset_button) == 1) {
		atomic_set(&long_press_handled, 1);
		LOG_WRN("Button held >= 5s. Factory reset disabled in UF2 battlefield app.");
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
		atomic_set(&force_measure_now, 1);
		k_work_reschedule(&measure_work, K_NO_WAIT);
		LOG_INF("Short press: forced sensor + battery read");
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

int main(void)
{
	int ret;

	LOG_INF("Frostbee UF2 battlefield app boot");

	try_enable_usb_logs();

	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC not ready");
	}

	if (!gpio_is_ready_dt(&vbat_enable)) {
		LOG_ERR("VBAT enable GPIO not ready");
	} else {
		ret = gpio_pin_configure_dt(&vbat_enable, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Failed to set VBAT enable pin idle: %d", ret);
		}
	}

	ret = adc_channel_setup(adc_dev, &adc_cfg);
	if (ret < 0) {
		LOG_ERR("ADC channel setup failed: %d", ret);
	}

#if DT_NODE_EXISTS(RESET_BUTTON_NODE)
	ret = button_init();
	if (ret < 0) {
		LOG_ERR("Button init failed: %d", ret);
	}
#endif

	k_work_init_delayable(&measure_work, measure_work_handler);
	k_work_schedule(&measure_work, K_NO_WAIT);

	while (1) {
		k_sleep(K_SECONDS(60));
	}
}
