/*
 * Copyright (c) 2019-2020 Peter Bigot Consulting, LLC
 * Copyright (c) 2021 Laird Connectivity
 * Copyright (c) 2025 Marcin Lyda <elektromarcin@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef CONFIG_SOC_POSIX
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* Required for gmtime_r */
#endif

#define DT_DRV_COMPAT microchip_mcp79412

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include "mcp79412.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/sys/util.h>
#include <inttypes.h>
#include <time.h>

LOG_MODULE_REGISTER(MCP79412, CONFIG_RTC_LOG_LEVEL);

/* Alarm channels */
#define ALARM0_ID			0
#define ALARM1_ID			1

/* Size of block when writing whole struct */
#define RTC_TIME_REGISTERS_SIZE		sizeof(struct mcp79412_time_registers)
#define RTC_ALARM_REGISTERS_SIZE	sizeof(struct mcp79412_alarm_registers)

/* Largest block size */
#define MAX_WRITE_SIZE                  (RTC_TIME_REGISTERS_SIZE)

/* tm struct uses years since 1900 but unix time uses years since
 * 1970. MCP79412 default year is '1' so the offset is 69
 */
#define UNIX_YEAR_OFFSET		69

/* Macro used to decode BCD to UNIX time to avoid potential copy and paste
 * errors.
 */
#define RTC_BCD_DECODE(reg_prefix) (reg_prefix##_one + reg_prefix##_ten * 10)

struct mcp79412_config {
	struct i2c_dt_spec i2c;
	const struct gpio_dt_spec int_gpios;
	bool vbat_enable;
};

/** @brief Convert bcd time in device registers to rtc_time
 *
 * @param dev the MCP79412 device pointer.
 *
 * @retval returns rtc_time struct.
 */
static struct rtc_time decode_rtc(const struct device *dev)
{
	struct mcp79412_data *data = dev->data;
	time_t time_unix = 0;
	struct rtc_time time = { 0 };

	time.tm_sec = RTC_BCD_DECODE(data->registers.rtc_sec.sec);
	time.tm_min = RTC_BCD_DECODE(data->registers.rtc_min.min);
	time.tm_hour = RTC_BCD_DECODE(data->registers.rtc_hours.hr);
	time.tm_mday = RTC_BCD_DECODE(data->registers.rtc_date.date);
	// tm struct uses 0-6 from sunday as opposed to 1-7 BCD in IC
	time.tm_wday = data->registers.rtc_weekday.weekday - 1;
	// tm struct starts months at 0, mcp79412 starts at 1 
	time.tm_mon = RTC_BCD_DECODE(data->registers.rtc_month.month) - 1;
	// tm struct uses years since 1900 but unix time uses years since 1970 
	time.tm_year = RTC_BCD_DECODE(data->registers.rtc_year.year) +
		UNIX_YEAR_OFFSET;

	time_unix = timeutil_timegm(rtc_time_to_tm(&time));

	LOG_DBG("Unix time is %d\n", (uint32_t)time_unix);

	return time;
}

/** @brief Encode time struct tm into mcp79412 rtc registers
 *
 * @param dev the MCP79412 device pointer.
 * @param time_buffer tm struct containing time to be encoded into mcp79412
 * registers.
 *
 * @retval return 0 on success, or a negative error code from invalid
 * parameter.
 */
static int encode_rtc(const struct device *dev, const struct rtc_time * const time_buffer)
{
	struct mcp79412_data *data = dev->data;
	uint8_t month;
	uint8_t year_since_epoch;

	/* In a tm struct, months start at 0, mcp79412 starts with 1 */
	month = time_buffer->tm_mon + 1;

	if (time_buffer->tm_year < UNIX_YEAR_OFFSET) {
		return -EINVAL;
	}
	year_since_epoch = time_buffer->tm_year - UNIX_YEAR_OFFSET;

	/* Set external oscillator configuration bit */
	data->registers.rtc_sec.start_osc = 1;

	data->registers.rtc_sec.sec_one = time_buffer->tm_sec % 10;
	data->registers.rtc_sec.sec_ten = time_buffer->tm_sec / 10;
	data->registers.rtc_min.min_one = time_buffer->tm_min % 10;
	data->registers.rtc_min.min_ten = time_buffer->tm_min / 10;
	data->registers.rtc_hours.hr_one = time_buffer->tm_hour % 10;
	data->registers.rtc_hours.hr_ten = time_buffer->tm_hour / 10;
	data->registers.rtc_weekday.weekday = time_buffer->tm_wday;
	data->registers.rtc_date.date_one = time_buffer->tm_mday % 10;
	data->registers.rtc_date.date_ten = time_buffer->tm_mday / 10;
	data->registers.rtc_month.month_one = month % 10;
	data->registers.rtc_month.month_ten = month / 10;
	data->registers.rtc_year.year_one = year_since_epoch % 10;
	data->registers.rtc_year.year_ten = year_since_epoch / 10;

	return 0;
}

#if defined(CONFIG_RTC_ALARM)
/** @brief Encode time struct tm into mcp79412 alarm registers
 *
 * @param dev the MCP79412 device pointer.
 * @param time_buffer tm struct containing time to be encoded into mcp79412
 * registers.
 * @param alarm_id alarm ID, can be 0 or 1 for MCP79412.
 *
 * @retval return 0 on success, or a negative error code from invalid
 * parameter.
 */
static int encode_alarm(const struct device *dev, const struct rtc_time * const time_buffer, uint8_t alarm_id)
{
	struct mcp79412_data *data = dev->data;
	uint8_t month;
	struct mcp79412_alarm_registers *alm_regs;

	if (alarm_id == ALARM0_ID) {
		alm_regs = &data->alm0_registers;
	} else if (alarm_id == ALARM1_ID) {
		alm_regs = &data->alm1_registers;
	} else {
		return -EINVAL;
	}
	/* In a tm struct, months start at 0 */
	month = time_buffer->tm_mon + 1;

	alm_regs->alm_sec.sec_one = time_buffer->tm_sec % 10;
	alm_regs->alm_sec.sec_ten = time_buffer->tm_sec / 10;
	alm_regs->alm_min.min_one = time_buffer->tm_min % 10;
	alm_regs->alm_min.min_ten = time_buffer->tm_min / 10;
	alm_regs->alm_hours.hr_one = time_buffer->tm_hour % 10;
	alm_regs->alm_hours.hr_ten = time_buffer->tm_hour / 10;
	alm_regs->alm_weekday.weekday = time_buffer->tm_wday;
	alm_regs->alm_date.date_one = time_buffer->tm_mday % 10;
	alm_regs->alm_date.date_ten = time_buffer->tm_mday / 10;
	alm_regs->alm_month.month_one = month % 10;
	alm_regs->alm_month.month_ten = month / 10;

	return 0;
}

static int decode_alarm(const struct device *dev, struct rtc_time * const time_buffer, uint8_t alarm_id)
{
	struct mcp79412_data *data = dev->data;
	struct mcp79412_alarm_registers *alm_regs;

	if (alarm_id == ALARM0_ID) {
		alm_regs = &data->alm0_registers;
	} else if (alarm_id == ALARM1_ID) {
		alm_regs = &data->alm1_registers;
	} else {
		return -EINVAL;
	}

	/* In a tm struct, months and weekdays start at 0 */

	time_buffer->tm_sec 	= RTC_BCD_DECODE(alm_regs->alm_sec.sec);
	time_buffer->tm_min 	= RTC_BCD_DECODE(alm_regs->alm_min.min);
	time_buffer->tm_hour 	= RTC_BCD_DECODE(alm_regs->alm_hours.hr);
	time_buffer->tm_wday	= alm_regs->alm_weekday.weekday - 1;
	time_buffer->tm_mday 	= RTC_BCD_DECODE(alm_regs->alm_date.date);
	time_buffer->tm_mon		= RTC_BCD_DECODE(alm_regs->alm_month.month) - 1;

	return 0;
}
#endif /* CONFIG_RTC_ALARM */

/** @brief Reads single register from MCP79412
 *
 * @param dev the MCP79412 device pointer.
 * @param addr register address.
 * @param val pointer to uint8_t that will contain register value if
 * successful.
 *
 * @retval return 0 on success, or a negative error code from an I2C
 * transaction.
 */
static int read_register(const struct device *dev, uint8_t addr, uint8_t *val)
{
	const struct mcp79412_config *cfg = dev->config;

	int rc = i2c_write_read_dt(&cfg->i2c, &addr, sizeof(addr), val, 1);

	return rc;
}

/** @brief Read registers from device and populate mcp79412_registers struct
 *
 * @param dev the MCP79412 device pointer.
 *
 * @retval return 0 on success, or a negative error code from an I2C
 * transaction.
 */
static int read_time(const struct device *dev, struct rtc_time * const time)
{
	struct mcp79412_data *data = dev->data;
	const struct mcp79412_config *cfg = dev->config;
	uint8_t addr = REG_RTC_SEC;

	int rc = i2c_write_read_dt(&cfg->i2c, &addr, sizeof(addr), &data->registers,
				   RTC_TIME_REGISTERS_SIZE);

	if (rc >= 0) {
		*time = decode_rtc(dev);
	}

	return rc;
}

/** @brief Write a single register to MCP79412
 *
 * @param dev the MCP79412 device pointer.
 * @param addr register address.
 * @param value Value that will be written to the register.
 *
 * @retval return 0 on success, or a negative error code from an I2C
 * transaction or invalid parameter.
 */
static int write_register(const struct device *dev, enum mcp79412_register addr, uint8_t value)
{
	const struct mcp79412_config *cfg = dev->config;
	int rc = 0;

	uint8_t time_data[2] = {addr, value};

	rc = i2c_write_dt(&cfg->i2c, time_data, sizeof(time_data));

	return rc;
}

/** @brief Write a full time struct to MCP79412 registers.
 *
 * @param dev the MCP79412 device pointer.
 * @param addr first register address to write to, should be REG_RTC_SEC,
 * REG_ALM0_SEC or REG_ALM0_SEC.
 * @param size size of data struct that will be written.
 *
 * @retval return 0 on success, or a negative error code from an I2C
 * transaction or invalid parameter.
 */
static int write_data_block(const struct device *dev, enum mcp79412_register addr, uint8_t size)
{
	struct mcp79412_data *data = dev->data;
	const struct mcp79412_config *cfg = dev->config;
	int rc = 0;
	uint8_t time_data[MAX_WRITE_SIZE + 1];
	uint8_t *write_block_start;

	if (size > MAX_WRITE_SIZE) {
		return -EINVAL;
	}

	if (addr >= REG_INVAL) {
		return -EINVAL;
	}

	if (addr == REG_RTC_SEC) {
		write_block_start = (uint8_t *)&data->registers;
	} else if (addr == REG_ALM0_SEC) {
		write_block_start = (uint8_t *)&data->alm0_registers;
	} else if (addr == REG_ALM1_SEC) {
		write_block_start = (uint8_t *)&data->alm1_registers;
	} else {
		return -EINVAL;
	}

	/* Load register address into first byte then fill in data values */
	time_data[0] = addr;
	memcpy(&time_data[1], write_block_start, size);

	rc = i2c_write_dt(&cfg->i2c, time_data, size + 1);

	return rc;
}

/** @brief Sets the correct weekday.
 *
 * If the time is never set then the device defaults to 1st January 1970
 * but with the wrong weekday set. This function ensures the weekday is
 * correct in this case.
 *
 * @param dev the MCP79412 device pointer.
 * @param time pointer to tm struct that will be used to work out the weekday
 *
 * @retval return 0 on success, or a negative error code from an I2C
 * transaction or invalid parameter.
 */
static int set_day_of_week(const struct device *dev, struct tm *time)
{
	struct mcp79412_data *data = dev->data;
	int rc = 0;
	// tm struct uses 0-6 from sunday as opposed to 1-7 BCD in IC
	data->registers.rtc_weekday.weekday = time->tm_wday + 1; 
	rc = write_register(dev, REG_RTC_WDAY,
		*((uint8_t *)(&data->registers.rtc_weekday)));

	return rc;
}

/** @brief Checks the interrupt pending flag (IF) of a given alarm.
 *
 * A callback is fired if an IRQ is pending.
 *
 * @param dev the MCP79412 device pointer.
 * @param alarm_id ID of alarm, can be 0 or 1 for MCP79412.
 */
static int mcp79412_alarm_is_pending(const struct device *dev, uint16_t alarm_id) {
	struct mcp79412_data *data = dev->data;
	struct mcp79412_alarm_registers *alm_regs;
	uint8_t alarm_reg_address;
	int ispending = 0;

	if (alarm_id == ALARM0_ID) {
		alarm_reg_address = REG_ALM0_WDAY;
		alm_regs = &data->alm0_registers;
	} else if (alarm_id == ALARM1_ID) {
		alarm_reg_address = REG_ALM1_WDAY;
		alm_regs = &data->alm1_registers;
	} else {
		return -EINVAL;
	}
	
	k_sem_take(&data->lock, K_FOREVER);

	int rc = read_register(dev, alarm_reg_address, (uint8_t *)&alm_regs->alm_weekday);
	
	if (rc != 0) goto out;
	
	ispending = alm_regs->alm_weekday.alm_if;
	alm_regs->alm_weekday.alm_if = 0;

	rc = write_register(dev, alarm_reg_address, *(uint8_t *)&alm_regs->alm_weekday);

out:
	k_sem_give(&data->lock);

	if (rc != 0) return rc;
	return ispending;
}

static void mcp79412_handle_interrupt(const struct device *dev, uint16_t alarm_id)
{
	struct mcp79412_data *data = dev->data;
	rtc_alarm_callback cb;
	struct rtc_time time = {0};
	bool fire_callback = false;

	if (!(alarm_id == ALARM0_ID || alarm_id == ALARM1_ID)) return;

	bool pending = mcp79412_alarm_is_pending(dev, alarm_id);
	k_sem_take(&data->lock, K_FOREVER);
	
	/* Check if this alarm has a pending interrupt */
	if (pending) {
		/* Fire callback */
		if (data->rtc_handler[alarm_id]) {
			cb = data->rtc_handler[alarm_id];
			time = data->callback_time[alarm_id];
			fire_callback = true;
		}
	}

	k_sem_give(&data->lock);

	if (fire_callback) {
		cb(data->mcp79412, alarm_id, data->alarm_user_data[alarm_id]);
	}
}

static void mcp79412_work_handler(struct k_work *work)
{
	struct mcp79412_data *data =
		CONTAINER_OF(work, struct mcp79412_data, alarm_work);

	/* Check interrupt flags for both alarms */
	mcp79412_handle_interrupt(data->mcp79412, ALARM0_ID);
	mcp79412_handle_interrupt(data->mcp79412, ALARM1_ID);
}

static void mcp79412_init_cb(const struct device *dev,
				 struct gpio_callback *gpio_cb, uint32_t pins)
{
	struct mcp79412_data *data =
		CONTAINER_OF(gpio_cb, struct mcp79412_data, int_callback);

	ARG_UNUSED(pins);

	(void)k_work_submit(&data->alarm_work);
}

int mcp79412_rtc_set_time(const struct device *dev, const struct rtc_time *time)
{
	struct mcp79412_data *data = dev->data;
	int rc = 0;

	k_sem_take(&data->lock, K_FOREVER);

	LOG_DBG("Desired time is %d-%d-%d %d:%d:%d\n", (time->tm_year + 1900),
		(time->tm_mon + 1), time->tm_mday, time->tm_hour,
		time->tm_min, time->tm_sec);

	// stop clock before setting time
	write_register(dev, REG_RTC_SEC, 0x00);

	/* Encode time */
	rc = encode_rtc(dev, time);

	if (rc >= 0) {
		/* Write to device */
		rc = write_data_block(dev, REG_RTC_SEC, RTC_TIME_REGISTERS_SIZE);
	}

	k_sem_give(&data->lock);

	return rc;
}

#if defined(CONFIG_RTC_ALARM)
static int mcp79412_alarm_supported(const struct device *dev, uint16_t alarm_id,
				      uint16_t * mask) {
	if (!(alarm_id == 0 || alarm_id == 1)) {
		return -EINVAL;
	}
	*mask = RTC_ALARM_TIME_MASK_SECOND 		|
			RTC_ALARM_TIME_MASK_MINUTE 		|
			RTC_ALARM_TIME_MASK_HOUR		|
			RTC_ALARM_TIME_MASK_WEEKDAY 	|
			RTC_ALARM_TIME_MASK_MONTHDAY	|
			RTC_ALARM_TIME_MASK_MONTH		;

	return 0;
}

static int mcp79412_alarm_set_time(const struct device *dev, uint16_t alarm_id,
				      uint16_t mask, const struct rtc_time *alarm_time)
{
	struct mcp79412_data *data = dev->data;
	uint8_t alarm_base_address;
	struct mcp79412_alarm_registers *alm_regs;
	int rc = 0;

	if (alarm_time == NULL) return -EINVAL;
	
	k_sem_take(&data->lock, K_FOREVER);
	
	if (alarm_id == ALARM0_ID) {
		alarm_base_address = REG_ALM0_SEC;
		alm_regs = &data->alm0_registers;
		data->registers.rtc_control.alm0_en = 1;
	} else if (alarm_id == ALARM1_ID) {
		alarm_base_address = REG_ALM1_SEC;
		alm_regs = &data->alm1_registers;
		data->registers.rtc_control.alm1_en = 1;
	} else {
		rc = -EINVAL;
		goto out;
	}

	rc = mcp79412_alarm_supported(dev, alarm_id, &mask);

	if (rc != 0) goto out;

	/* Set alarm to match with second, minute, hour, day of week, day of
	 * month and month
	 */
	uint8_t alm_msk = 0;
	//TODO: handle case where multiple fields are selected by storing target time and mask properly
	//TODO: fix alarms always selecting MCP79412_ALARM_TRIGGER_ALL
	if (mask != 0) alm_msk = MCP79412_ALARM_TRIGGER_ALL;
	if ((mask & RTC_ALARM_TIME_MASK_SECOND) && ~(mask & ~RTC_ALARM_TIME_MASK_SECOND)) alm_msk = MCP79412_ALARM_TRIGGER_SECONDS;
	else if ((mask & RTC_ALARM_TIME_MASK_MINUTE) && ~(mask & ~RTC_ALARM_TIME_MASK_MINUTE)) alm_msk = MCP79412_ALARM_TRIGGER_MINUTES;
	else if ((mask & RTC_ALARM_TIME_MASK_HOUR) && ~(mask & ~RTC_ALARM_TIME_MASK_HOUR)) alm_msk = MCP79412_ALARM_TRIGGER_HOURS;
	else if ((mask & RTC_ALARM_TIME_MASK_WEEKDAY) && ~(mask & ~RTC_ALARM_TIME_MASK_WEEKDAY)) alm_msk = MCP79412_ALARM_TRIGGER_WDAY;
	else if ((mask & RTC_ALARM_TIME_MASK_MONTHDAY) && ~(mask & ~RTC_ALARM_TIME_MASK_MONTHDAY)) alm_msk = MCP79412_ALARM_TRIGGER_DATE;

	alm_regs->alm_weekday.alm_msk = alm_msk;

	/* Write time to alarm registers */
	encode_alarm(dev, alarm_time, alarm_id);
	rc = write_data_block(dev, alarm_base_address, RTC_ALARM_REGISTERS_SIZE);
	if (rc < 0) {
		goto out;
	}

	/* Enable alarm */
	rc = write_register(dev, REG_RTC_CONTROL,
		*((uint8_t *)(&data->registers.rtc_control)));
	if (rc < 0) {
		goto out;
	}

	/* Config user data and callback */
	data->callback_time[alarm_id] = *alarm_time;
	data->alarm_mask[alarm_id] = mask;


out:
	k_sem_give(&data->lock);

	return rc;
}

static int mcp79412_alarm_get_time(const struct device *dev, uint16_t alarm_id,
				      uint16_t *mask, struct rtc_time *alarm_time)
{
	struct mcp79412_data *data = dev->data;
	const struct mcp79412_config *cfg = dev->config;
	struct mcp79412_alarm_registers *alm_regs;
	uint8_t addr;
	if (mask == NULL || alarm_time == NULL) return -EINVAL;
	if (alarm_id == ALARM0_ID) {
		alm_regs = &data->alm0_registers;
		addr = REG_ALM0_SEC;
	}
	else if (alarm_id == ALARM1_ID) {
		alm_regs = &data->alm1_registers;
		addr = REG_ALM1_SEC;
	}
	else return -EINVAL;

	k_sem_take(&data->lock, K_FOREVER);

	int rc = i2c_write_read_dt(&cfg->i2c, &addr, sizeof(addr), alm_regs, RTC_ALARM_REGISTERS_SIZE);
	
	if (rc != 0) goto out;

	rc = decode_alarm(dev, alarm_time, alarm_id);
	*mask = data->alarm_mask[alarm_id];

out:
	k_sem_give(&data->lock);

	return rc;
}

static int mcp79412_alarm_set_callback(const struct device *dev, uint16_t alarm_id, 
	rtc_alarm_callback callback, void *user_data) {
	struct mcp79412_data *data = dev->data;
	k_sem_take(&data->lock, K_FOREVER);
	data->rtc_handler[alarm_id] = callback;
	data->alarm_user_data[alarm_id] = user_data;
	k_sem_give(&data->lock);
	return 0;
}
#endif /* CONFIG_RTC_ALARM */

#if defined(CONFIG_RTC_UPDATE)
static int mcp79412_update_set_callback(const struct device * dev,
		rtc_update_callback callback, void * user_data) {
	struct mcp79412_data *data = dev->data;
	k_sem_take(&data->lock, K_FOREVER);
	if (callback == NULL && user_data == NULL) {
		data->update_callback_mode = false;
		data->update_callback = NULL;
		data->update_user_data = NULL;


		//TODO: reconfigure IC
		k_sem_give(&data->lock);
		return 0;
	}
	//TODO
	k_sem_give(&data->lock);
	return -ENOSYS;
}
#endif /* CONFIG_RTC_UPDATE */

#if defined(CONFIG_RTC_CALIBRATION)	
static int mcp79412_set_calibration(const struct device * dev, int32_t calibration) {
	return -ENOSYS;
}

static int mcp79412_get_calibration(const struct device * dev, int32_t calibration) {
	return -ENOSYS;
}
#endif /* CONFIG_RTC_CALIBRATION */

static int mcp79412_init(const struct device *dev)
{
	struct mcp79412_data *data = dev->data;
	const struct mcp79412_config *cfg = dev->config;
	int rc = 0;
	struct rtc_time time = {0};

	/* Initialize and take the lock */
	k_sem_init(&data->lock, 0, 1);

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C device %s is not ready", cfg->i2c.bus->name);
		rc = -ENODEV;
		goto out;
	}

	rc = read_time(dev, &time);
	if (rc < 0) {
		goto out;
	}

	/* Configure VBat enable */
	data->registers.rtc_weekday.vbaten = cfg->vbat_enable;

	/* Set day of week and update VBat enable config */
	rc = set_day_of_week(dev, rtc_time_to_tm(&time));
	if (rc < 0) goto out;

	/* Set 24-hour time */
	data->registers.rtc_hours.twelve_hr = false;
	rc = write_register(dev, REG_RTC_HOUR,
		*((uint8_t *)(&data->registers.rtc_hours)));
	if (rc < 0) goto out;

	/* Configure alarm interrupt gpio */
	if (cfg->int_gpios.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->int_gpios)) {
			LOG_ERR("Port device %s is not ready",
				cfg->int_gpios.port->name);
			rc = -ENODEV;
			goto out;
		}
		
		data->mcp79412 = dev;
		k_work_init(&data->alarm_work, mcp79412_work_handler);

		gpio_pin_configure_dt(&cfg->int_gpios, GPIO_INPUT);

		gpio_pin_interrupt_configure_dt(&cfg->int_gpios,
						GPIO_INT_EDGE_TO_ACTIVE);
		
		gpio_init_callback(&data->int_callback, mcp79412_init_cb,
				   BIT(cfg->int_gpios.pin));

		(void)gpio_add_callback(cfg->int_gpios.port, &data->int_callback);

		// Configure interrupt polarity
		if ((cfg->int_gpios.dt_flags & GPIO_ACTIVE_LOW) == GPIO_ACTIVE_LOW) {
			data->int_active_high = false;
		} else {
			data->int_active_high = true;
		}
		data->alm0_registers.alm_weekday.alm_pol = data->int_active_high;
		data->alm1_registers.alm_weekday.alm_pol = data->int_active_high;
		rc = write_register(dev, REG_ALM0_WDAY,
				    *((uint8_t *)(&data->alm0_registers.alm_weekday)));
		rc = write_register(dev, REG_ALM1_WDAY,
				    *((uint8_t *)(&data->alm1_registers.alm_weekday)));
	}
	if (data->registers.rtc_sec.start_osc == 0) {
		data->registers.rtc_sec.start_osc = 1;
		i2c_reg_update_byte_dt(&cfg->i2c, REG_RTC_SEC, 0x80, *(uint8_t *)&data->registers.rtc_sec);
	}
out:

	k_sem_give(&data->lock);
	return rc;
}

static DEVICE_API(rtc, mcp79412_api) = {
	.set_time = mcp79412_rtc_set_time,
	.get_time = read_time,
#if defined(CONFIG_RTC_ALARM)
	.alarm_get_supported_fields = mcp79412_alarm_supported,
	.alarm_set_time = mcp79412_alarm_set_time,	
	.alarm_get_time = mcp79412_alarm_get_time,
	.alarm_is_pending = mcp79412_alarm_is_pending,		//TODO: Remake to poll for interrupts
	.alarm_set_callback = mcp79412_alarm_set_callback,
#endif /* CONFIG_RTC_ALARM */
#if defined(CONFIG_RTC_UPDATE)
	.update_set_callback = mcp79412_update_set_callback, //TODO
#endif /* CONFIG_RTC_UPDATE */
#if defined(CONFIG_RTC_CALIBRATION)					
	.set_calibration = mcp79412_set_calibration,	// not implemented
	.get_calibration = mcp79412_get_calibration,	// not implemented
#endif /* CONFIG_RTC_CALIBRATION */
};

#define INST_DT_MCP79412(index)                                                         \
											\
	static struct mcp79412_data mcp79412_data_##index;				\
											\
	static const struct mcp79412_config mcp79412_config_##index = {			\
		.i2c = I2C_DT_SPEC_INST_GET(index),					\
		.int_gpios = GPIO_DT_SPEC_INST_GET_OR(index, int_gpios, {0}),		\
		.vbat_enable = DT_INST_PROP(index, vbat_enable)				\
	};										\
											\
	DEVICE_DT_INST_DEFINE(index, mcp79412_init, NULL,				\
		    &mcp79412_data_##index,						\
		    &mcp79412_config_##index,						\
		    POST_KERNEL,							\
		    CONFIG_RTC_INIT_PRIORITY,					\
		    &mcp79412_api);

DT_INST_FOREACH_STATUS_OKAY(INST_DT_MCP79412);

