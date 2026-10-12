#ifndef __CW2015_H__
#define __CW2015_H__

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define CW2015_ADDR             0x62

/* CW2015 Register Definitions */
#define REG_VERSION             0x0
#define REG_VCELL               0x2
#define REG_SOC                 0x4
#define REG_RRT_ALERT           0x6
#define REG_CONFIG              0x8
#define REG_MODE                0xA
#define REG_BATINFO             0x10

#define MODE_SLEEP_MASK         (0x3<<6)
#define MODE_SLEEP              (0x3<<6)
#define MODE_NORMAL             (0x0<<6)
#define MODE_QUICK_START        (0x3<<4)
#define MODE_RESTART            (0xf<<0)
#define CONFIG_UPDATE_FLG       (0x1<<1)
#define ATHD                    (0x0<<3)        // ATHD = 0%

/* CW2015 Parameters */
#define BATTERY_UP_MAX_CHANGE         720
#define BATTERY_DOWN_MIN_CHANGE       60
#define BATTERY_DOWN_MIN_CHANGE_SLEEP 1800
#define SIZE_BATINFO                  64

/* Battery profile data - replace with your battery's profile */
static const unsigned char cw_bat_config_info[SIZE_BATINFO] = {
    0X15,0X7E,0X7C,0X5C,0X64,0X6A,0X65,0X5C,0X55,0X53,0X56,0X61,0X6F,0X66,0X50,0X48,
    0X43,0X42,0X40,0X43,0X4B,0X5F,0X75,0X7D,0X52,0X44,0X07,0XAE,0X11,0X22,0X40,0X56,
    0X6C,0X7C,0X85,0X86,0X3D,0X19,0X8D,0X1B,0X06,0X34,0X46,0X79,0X8D,0X90,0X90,0X46,
    0X67,0X80,0X97,0XAF,0X80,0X9F,0XAE,0XCB,0X2F,0X00,0X64,0XA5,0XB5,0X11,0XD0,0X11
};

/* Device structure */
typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task_handle;
    uint32_t update_interval_ms;
} cw2015_dev_t;

/* Battery information structure */
typedef struct {
    uint8_t usb_online;
    uint32_t capacity;
    uint32_t voltage;
    uint8_t alt;
} cw_battery_t;

/* External battery variable */
extern cw_battery_t cw_bat;

/* Function declarations */
esp_err_t cw2015_init();
esp_err_t cw2015_deinit(void);
esp_err_t cw2015_update_voltage(void);
esp_err_t cw2015_update_capacity(void);
esp_err_t cw2015_release_alert_pin(void);
esp_err_t cw2015_set_athd(uint8_t new_athd);

#endif /* __CW2015_H__ */