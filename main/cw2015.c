#include "cw2015.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "freertos/task.h"

static const char *TAG = "CW2015";

/* External battery variable */
cw_battery_t cw_bat;

/* Internal device handle */
static cw2015_dev_t *dev = NULL;

/* Internal variables */
static uint8_t no_charger_full_jump = 0;
static uint32_t allow_no_charger_full = 0;
static uint32_t allow_charger_always_zero = 0;
static uint8_t if_quickstart = 0;
static uint8_t reset_loop = 0;

/* Internal functions */
static esp_err_t cw2015_write(uint8_t reg, uint8_t data);
static esp_err_t cw2015_read(uint8_t reg, uint8_t *data);
static esp_err_t cw2015_por(void);
static esp_err_t cw2015_config(void);
static esp_err_t cw2015_update_config_info(void);
static int cw2015_get_capacity(void);
static int cw2015_get_vcell(void);
static void cw2015_task(void *arg);

static esp_err_t cw2015_write(uint8_t reg, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CW2015_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(0, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t cw2015_read(uint8_t reg, uint8_t *data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CW2015_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CW2015_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(0, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t cw2015_por(void)
{
    esp_err_t ret = cw2015_write(REG_MODE, MODE_SLEEP);
    if (ret != ESP_OK)
        return ret;
    vTaskDelay(pdMS_TO_TICKS(1));

    ret = cw2015_write(REG_MODE, MODE_NORMAL);
    if (ret != ESP_OK)
        return ret;
    vTaskDelay(pdMS_TO_TICKS(1));

    return cw2015_config();
}

static esp_err_t cw2015_config(void)
{
    esp_err_t ret;
    uint8_t i;
    uint8_t reg_val;

    /* Wake up from sleep mode */
    ret = cw2015_write(REG_MODE, MODE_NORMAL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wakeup fail");
        return ret;
    }

    /* Check ATHD */
    ret = cw2015_read(REG_CONFIG, &reg_val);
    if (ret != ESP_OK)
        return ret;

    if ((reg_val & 0xf8) != ATHD)
    {
        reg_val &= 0x07; /* Clear ATHD */
        reg_val |= ATHD; /* Set ATHD */
        ret = cw2015_write(REG_CONFIG, reg_val);
        if (ret != ESP_OK)
            return ret;
    }

    /* Check config update flag */
    ret = cw2015_read(REG_CONFIG, &reg_val);
    if (ret != ESP_OK)
        return ret;

    if (!(reg_val & CONFIG_UPDATE_FLG))
    {
        ESP_LOGI(TAG, "Update flag for new battery info needed");
        ret = cw2015_update_config_info();
        if (ret != ESP_OK)
            return ret;
    }
    else
    {
        for (i = 0; i < SIZE_BATINFO; i++)
        {
            ret = cw2015_read(REG_BATINFO + i, &reg_val);
            if (ret != ESP_OK)
                return ret;
            if (cw_bat_config_info[i] != reg_val)
                break;
        }

        if (i != SIZE_BATINFO)
        {
            ESP_LOGI(TAG, "Update flag for new battery info needed");
            ret = cw2015_update_config_info();
            if (ret != ESP_OK)
                return ret;
        }
    }

    /* Check SOC */
    for (i = 0; i < 30; i++)
    {
        ret = cw2015_read(REG_SOC, &reg_val);
        if (ret != ESP_OK)
            return ret;
        else if (reg_val <= 100)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (i >= 30)
    {
        ret = cw2015_write(REG_MODE, MODE_SLEEP);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cw2015_update_config_info(void)
{
    esp_err_t ret;
    uint8_t i;
    uint8_t reg_val;

    /* Make sure not in sleep mode */
    ret = cw2015_read(REG_MODE, &reg_val);
    if (ret != ESP_OK)
        return ret;

    if ((reg_val & MODE_SLEEP_MASK) == MODE_SLEEP)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Update new battery info */
    for (i = 0; i < SIZE_BATINFO; i++)
    {
        ret = cw2015_write(REG_BATINFO + i, cw_bat_config_info[i]);
        if (ret != ESP_OK)
            return ret;
    }

    /* Readback and check */
    for (i = 0; i < SIZE_BATINFO; i++)
    {
        ret = cw2015_read(REG_BATINFO + i, &reg_val);
        if (ret != ESP_OK)
            return ret;
        if (reg_val != cw_bat_config_info[i])
            return ESP_ERR_INVALID_RESPONSE;
    }

    /* Set to use new battery info */
    ret = cw2015_read(REG_CONFIG, &reg_val);
    if (ret != ESP_OK)
        return ret;

    reg_val |= CONFIG_UPDATE_FLG; /* Set UPDATE_FLAG */
    reg_val &= 0x07;              /* Clear ATHD */
    reg_val |= ATHD;              /* Set ATHD */

    ret = cw2015_write(REG_CONFIG, reg_val);
    if (ret != ESP_OK)
        return ret;

    /* Reset */
    ret = cw2015_write(REG_MODE, MODE_RESTART);
    if (ret != ESP_OK)
        return ret;

    vTaskDelay(pdMS_TO_TICKS(1));
    return cw2015_write(REG_MODE, MODE_NORMAL);
}

esp_err_t cw2015_set_athd(uint8_t new_athd)
{
    uint8_t reg_val;
    esp_err_t ret = cw2015_read(REG_CONFIG, &reg_val);
    if (ret != ESP_OK)
        return ret;

    reg_val &= 0x07;            /* Clear ATHD */
    reg_val |= (new_athd << 3); /* Set new ATHD */

    return cw2015_write(REG_CONFIG, reg_val);
}

static int cw2015_get_capacity(void)
{
    uint8_t allow_capacity;
    uint8_t reg_val;
    uint8_t cw_capacity;

    esp_err_t ret = cw2015_read(REG_SOC, &reg_val);
    if (ret != ESP_OK)
        return -1;

    cw_capacity = reg_val;

    if ( cw_capacity > 100)
    {
        reset_loop++;
        if (reset_loop > 5)
        {
            ret = cw2015_por();
            if (ret != ESP_OK)
                return -1;
            reset_loop = 0;
        }
        return cw_bat.capacity;
    }
    else
    {
        reset_loop = 0;
    }

    if ((cw_bat.usb_online == 1) && (cw_capacity == (cw_bat.capacity - 1))) {
        if (!((cw_capacity == 0 && cw_bat.capacity <= 2) || (cw_capacity == 100 && cw_bat.capacity == 99)))
        {
            cw_capacity = cw_bat.capacity;
        }
    }
    
    if ((cw_bat.usb_online == 1) && (cw_capacity >= 95) && (cw_capacity <= cw_bat.capacity)) {
        allow_no_charger_full++;
        if (allow_no_charger_full >= BATTERY_UP_MAX_CHANGE)
        {
            allow_capacity = cw_bat.capacity + 1;
            cw_capacity = (allow_capacity <= 100) ? allow_capacity : 100;
            no_charger_full_jump = 1;
            allow_no_charger_full = 0;
        }
        else if (cw_capacity <= cw_bat.capacity)
        {
            cw_capacity = cw_bat.capacity;
        }
    } else if ((cw_bat.usb_online == 0) && (cw_capacity <= cw_bat.capacity) && (cw_capacity >= 90) && (no_charger_full_jump == 1)) {
        if (cw_bat.usb_online == 0)
            allow_no_charger_full++;
        if (allow_no_charger_full >= BATTERY_DOWN_MIN_CHANGE)
        {
            allow_capacity = cw_bat.capacity - 1;
            allow_no_charger_full = 0;
            if (cw_capacity >= allow_capacity)
            {
                no_charger_full_jump = 0;
            }
            else
            {
                cw_capacity = (allow_capacity > 0) ? allow_capacity : 0;
            }
        }
        else if (cw_capacity <= cw_bat.capacity)
        {
            cw_capacity = cw_bat.capacity;
        }
    } else {
        allow_no_charger_full = 0;
    }
    
    if ((cw_bat.usb_online > 0) && (cw_capacity == 0)) {
        allow_charger_always_zero++;
        if ((allow_charger_always_zero >= BATTERY_DOWN_MIN_CHANGE_SLEEP) && (if_quickstart == 0))
        {
            ret = cw2015_por();
            if (ret != ESP_OK)
                return -1;
            if_quickstart = 1;
            allow_charger_always_zero = 0;
        }
    } else if ((if_quickstart == 1) && (cw_bat.usb_online == 0)) {
        if_quickstart = 0;
    }
    
    return cw_capacity;
}

static int cw2015_get_vcell(void)
{
    uint8_t reg_val[2];
    uint32_t ad_value = 0;
    uint32_t ad_buff = 0;
    uint32_t ad_value_min = 0;
    uint32_t ad_value_max = 0;

    for (uint8_t get_ad_times = 0; get_ad_times < 3; get_ad_times++)
    {
        if (cw2015_read(REG_VCELL, &reg_val[0]) != ESP_OK)
            return 1;
        if (cw2015_read(REG_VCELL + 1, &reg_val[1]) != ESP_OK)
            return 1;

        ad_buff = (reg_val[0] << 8) + reg_val[1];

        if (get_ad_times == 0)
        {
            ad_value_min = ad_buff;
            ad_value_max = ad_buff;
        }

        if (ad_buff < ad_value_min)
            ad_value_min = ad_buff;
        if (ad_buff > ad_value_max)
            ad_value_max = ad_buff;

        ad_value += ad_buff;
    }

    ad_value -= ad_value_min;
    ad_value -= ad_value_max;
    ad_value = ad_value * 305 / 1000;
    return ad_value;
}

esp_err_t cw2015_release_alert_pin(void)
{
    uint8_t reg_val;
    uint8_t alrt;

    esp_err_t ret = cw2015_read(REG_RRT_ALERT, &reg_val);
    if (ret != ESP_OK)
        return ret;

    alrt = reg_val & 0x80;
    reg_val = reg_val & 0x7f;

    ret = cw2015_write(REG_RRT_ALERT, reg_val);
    if (ret != ESP_OK)
        return ret;

    return alrt ? ESP_OK : ESP_FAIL;
}

esp_err_t cw2015_update_capacity(void)
{
    if (xSemaphoreTake(dev->lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    int cw_capacity = cw2015_get_capacity();
    xSemaphoreGive(dev->lock);

    if ((cw_capacity >= 0) && (cw_capacity <= 100) && (cw_bat.capacity != cw_capacity))
    {
        cw_bat.capacity = cw_capacity;
    }

    return ESP_OK;
}

esp_err_t cw2015_update_voltage(void)
{
    if (xSemaphoreTake(dev->lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    int cw_voltage = cw2015_get_vcell();
    xSemaphoreGive(dev->lock);

    if (cw_voltage == 1)
    {
        cw_bat.voltage = cw_bat.voltage;
    }
    else if (cw_bat.voltage != cw_voltage)
    {
        cw_bat.voltage = cw_voltage;
    }

    return ESP_OK;
}

esp_err_t cw2015_init()
{
    esp_err_t ret;
    uint8_t error_num = 0;

    /* Allocate memory for device */
    dev = calloc(1, sizeof(cw2015_dev_t));
    if (dev == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for CW2015 device");
        return ESP_ERR_NO_MEM;
    }

    dev->update_interval_ms = 1000;

    /* Create mutex */
    dev->lock = xSemaphoreCreateMutex();
    if (dev->lock == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(dev);
        return ESP_ERR_NO_MEM;
    }



    /* Initialize device */
    if (xSemaphoreTake(dev->lock, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        vSemaphoreDelete(dev->lock);
        free(dev);
        return ESP_ERR_TIMEOUT;
    }

    while ((ret = cw2015_config()) != ESP_OK)
    {
        ESP_LOGE(TAG, "Config failed, error code: %d", ret);
        error_num++;
        if (error_num > 20)
        {
            xSemaphoreGive(dev->lock);
            vSemaphoreDelete(dev->lock);
            free(dev);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    xSemaphoreGive(dev->lock);

    /* Initialize battery info */
    cw_bat.usb_online = 0;
    cw_bat.capacity = 2;
    cw_bat.voltage = 0;
    cw_bat.alt = 0;

    /* Create update task */
    if (xTaskCreate(cw2015_task, "cw2015_task", 2048, NULL, 10, &dev->task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create CW2015 task");
        vSemaphoreDelete(dev->lock);
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t cw2015_deinit(void)
{
    if (dev == NULL)
        return ESP_ERR_INVALID_STATE;

    /* Delete task */
    if (dev->task_handle != NULL)
    {
        vTaskDelete(dev->task_handle);
    }

    /* Delete mutex */
    if (dev->lock != NULL)
    {
        vSemaphoreDelete(dev->lock);
    }

    /* Uninstall I2C driver */
    i2c_driver_delete(0);

    /* Free memory */
    free(dev);
    dev = NULL;

    return ESP_OK;
}

static void cw2015_task(void *arg)
{
    while (1)
    {
        cw2015_update_capacity();
        cw2015_update_voltage();
        vTaskDelay(pdMS_TO_TICKS(dev->update_interval_ms));
    }
}