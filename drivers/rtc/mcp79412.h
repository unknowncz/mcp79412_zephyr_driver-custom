#ifndef ZEPHYR_DRIVERS_RTC_MCP79412_
#define ZEPHYR_DRIVERS_RTC_MCP79412_

#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/drivers/rtc.h>
#include <time.h>

// Time control registers
#define MCP79412_REG_RTCSEC 0x00
#define MCP79412_REG_RTCMIN 0x01
#define MCP79412_REG_RTCHOUR 0x02
#define MCP79412_REG_RTCWKDAY 0x03
#define MCP79412_REG_RTCDATE 0x04
#define MCP79412_REG_RTCMTH 0x05
#define MCP79412_REG_RTCYEAR 0x06
#define MCP79412_REG_CONTROL 0x07
#define MCP79412_REG_OSCTRIM 0x08
#define MCP79412_REG_EEUNLOCK 0x09

// Alarm 0 registers
#define MCP79412_REG_ALM0SEC 0x0A
#define MCP79412_REG_ALM0MIN 0x0B
#define MCP79412_REG_ALM0HOUR 0x0C
#define MCP79412_REG_ALM0WKDAY 0x0D
#define MCP79412_REG_ALM0DATE 0x0E
#define MCP79412_REG_ALM0MTH 0x0F

// Alarm 1 registers
#define MCP79412_REG_ALM1SEC 0x11
#define MCP79412_REG_ALM1MIN 0x12
#define MCP79412_REG_ALM1HOUR 0x13
#define MCP79412_REG_ALM1WKDAY 0x14
#define MCP79412_REG_ALM1DATE 0x15
#define MCP79412_REG_ALM1MTH 0x16

// Power-down timestamp registers
#define MCP79412_REG_PWRDNMIN 0x18
#define MCP79412_REG_PWRDNHOUR 0x19
#define MCP79412_REG_PWRDNDATE 0x1A
#define MCP79412_REG_PWRDNMNTH 0x1B

// Power-up timestamp registers
#define MCP79412_REG_PWRUPMIN 0x1C
#define MCP79412_REG_PWRUPHOUR 0x1D
#define MCP79412_REG_PWRUPDATE 0x1E
#define MCP79412_REG_PWRUPMNTH 0x1F

enum mcp79412_alarm_trigger {
	MCP79412_ALARM_TRIGGER_SECONDS	= 0x0,
	MCP79412_ALARM_TRIGGER_MINUTES	= 0x1,
	MCP79412_ALARM_TRIGGER_HOURS	= 0x2,
	MCP79412_ALARM_TRIGGER_WDAY	= 0x3,
	MCP79412_ALARM_TRIGGER_DATE	= 0x4,
	/* TRIGGER_ALL matches seconds, minutes, hours, weekday, date and month */
	MCP79412_ALARM_TRIGGER_ALL	= 0x7,
};

enum mcp79412_register {
	REG_RTC_SEC		= 0x0,
	REG_RTC_MIN		= 0x1,
	REG_RTC_HOUR		= 0x2,
	REG_RTC_WDAY		= 0x3,
	REG_RTC_DATE		= 0x4,
	REG_RTC_MONTH		= 0x5,
	REG_RTC_YEAR		= 0x6,
	REG_RTC_CONTROL		= 0x7,
	REG_RTC_OSCTRIM		= 0x8,
	/* 0x9 not implemented */
	REG_ALM0_SEC		= 0xA,
	REG_ALM0_MIN		= 0xB,
	REG_ALM0_HOUR		= 0xC,
	REG_ALM0_WDAY		= 0xD,
	REG_ALM0_DATE		= 0xE,
	REG_ALM0_MONTH		= 0xF,
	/* 0x10 not implemented */
	REG_ALM1_SEC		= 0x11,
	REG_ALM1_MIN		= 0x12,
	REG_ALM1_HOUR		= 0x13,
	REG_ALM1_WDAY		= 0x14,
	REG_ALM1_DATE		= 0x15,
	REG_ALM1_MONTH		= 0x16,
	/* 0x17 not implemented */
	REG_PWR_DWN_MIN		= 0x18,
	REG_PWR_DWN_HOUR	= 0x19,
	REG_PWR_DWN_DATE	= 0x1A,
	REG_PWR_DWN_MONTH	= 0x1B,
	REG_PWR_UP_MIN		= 0x1C,
	REG_PWR_UP_HOUR		= 0x1D,
	REG_PWR_UP_DATE		= 0x1E,
	REG_PWR_UP_MONTH	= 0x1F,
	SRAM_MIN		= 0x20,
	SRAM_MAX		= 0x5F,
	REG_INVAL		= 0x60,
};

#define RTC_BCD_DECODE(reg_prefix) (reg_prefix##_one + reg_prefix##_ten * 10)

struct mcp79412_rtc_sec {
	uint8_t sec_one : 4;
	uint8_t sec_ten : 3;
	uint8_t start_osc : 1;
} __packed;

struct mcp79412_rtc_min {
	uint8_t min_one : 4;
	uint8_t min_ten : 3;
	uint8_t nimp : 1;
} __packed;

struct mcp79412_rtc_hours {
	uint8_t hr_one : 4;
	uint8_t hr_ten : 2;
	uint8_t twelve_hr : 1;
	uint8_t nimp : 1;
} __packed;

struct mcp79412_rtc_weekday {
	uint8_t weekday : 3;
	uint8_t vbaten : 1;
	uint8_t pwrfail : 1;
	uint8_t oscrun : 1;
	uint8_t nimp : 2;
} __packed;

struct mcp79412_rtc_date {
	uint8_t date_one : 4;
	uint8_t date_ten : 2;
	uint8_t nimp : 2;
} __packed;

struct mcp79412_rtc_month {
	uint8_t month_one : 4;
	uint8_t month_ten : 1;
	uint8_t lpyr : 1;
	uint8_t nimp : 2;
} __packed;

struct mcp79412_rtc_year {
	uint8_t year_one : 4;
	uint8_t year_ten : 4;
} __packed;

struct mcp79412_rtc_control {
	uint8_t sqwfs : 2;
	uint8_t crs_trim : 1;
	uint8_t ext_osc : 1;
	uint8_t alm0_en : 1;
	uint8_t alm1_en : 1;
	uint8_t sqw_en : 1;
	uint8_t out : 1;
} __packed;

struct mcp79412_rtc_osctrim {
	uint8_t trim_val : 7;
	uint8_t sign : 1;
} __packed;

struct mcp79412_alm_sec {
	uint8_t sec_one : 4;
	uint8_t sec_ten : 3;
	uint8_t nimp : 1;
} __packed;

struct mcp79412_alm_min {
	uint8_t min_one : 4;
	uint8_t min_ten : 3;
	uint8_t nimp : 1;
} __packed;

struct mcp79412_alm_hours {
	uint8_t hr_one : 4;
	uint8_t hr_ten : 2;
	uint8_t twelve_hr : 1;
	uint8_t nimp : 1;
} __packed;

struct mcp79412_alm_weekday {
	uint8_t weekday : 3;
	uint8_t alm_if : 1;
	uint8_t alm_msk : 3;
	uint8_t alm_pol : 1;
} __packed;

struct mcp79412_alm_date {
	uint8_t date_one : 4;
	uint8_t date_ten : 2;
	uint8_t nimp : 2;
} __packed;

struct mcp79412_alm_month {
	uint8_t month_one : 4;
	uint8_t month_ten : 1;
	uint8_t nimp : 3;
} __packed;

union mcp79412_bus {
    struct i2c_dt_spec i2c;
};

typedef int (*mcp79412_bus_check_fn)(const union mcp79412_bus *bus);
typedef int (*mcp79412_bus_init_fn)(const union mcp79412_bus *bus);
typedef int (*mcp79412_reg_read_fn)(const union mcp79412_bus *bus,
				  uint8_t start,
				  uint8_t *data,
				  uint16_t len);
typedef int (*mcp79412_reg_write_fn)(const union mcp79412_bus *bus,
				   uint8_t start,
				   const uint8_t *data,
				   uint16_t len);

struct mcp79412_bus_io {
	mcp79412_bus_check_fn check;
	mcp79412_reg_read_fn read;
	mcp79412_reg_write_fn write;
	mcp79412_bus_init_fn init;
};

struct mcp79412_feature_config {
	const char *name;
	const uint8_t *config_file;
	size_t config_file_len;
};

struct mcp79412_time_registers {
	struct mcp79412_rtc_sec rtc_sec;
	struct mcp79412_rtc_min rtc_min;
	struct mcp79412_rtc_hours rtc_hours;
	struct mcp79412_rtc_weekday rtc_weekday;
	struct mcp79412_rtc_date rtc_date;
	struct mcp79412_rtc_month rtc_month;
	struct mcp79412_rtc_year rtc_year;
	struct mcp79412_rtc_control rtc_control;
	struct mcp79412_rtc_osctrim rtc_osctrim;
} __packed;

struct mcp79412_alarm_registers {
	struct mcp79412_alm_sec alm_sec;
	struct mcp79412_alm_min alm_min;
	struct mcp79412_alm_hours alm_hours;
	struct mcp79412_alm_weekday alm_weekday;
	struct mcp79412_alm_date alm_date;
	struct mcp79412_alm_month alm_month;
} __packed;

struct mcp79412_data {
	const struct device *mcp79412;
	struct k_sem lock;
	struct mcp79412_time_registers registers;
	struct mcp79412_alarm_registers alm0_registers;
	struct mcp79412_alarm_registers alm1_registers;

	struct k_work alarm_work;
	struct gpio_callback int_callback;

	rtc_alarm_callback rtc_handler[2];
	struct rtc_time callback_time[2]; // TODO: Change name
	void *alarm_user_data[2];

	bool int_active_high;
};

int mcp79412_rtc_set_time(const struct device *dev, const struct rtc_time *time);

#endif