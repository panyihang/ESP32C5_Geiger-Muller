#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "hv_monitor.h"

#define HV_MONITOR_TAG "HV_MON"

#define HV_ENABLE_GPIO (GPIO_NUM_10)
#define HV_FB_GPIO (GPIO_NUM_2)
#define HV_FB_ADC_UNIT (ADC_UNIT_1)
#define HV_FB_ADC_CHANNEL (ADC_CHANNEL_1)
#define HV_FB_ADC_ATTEN (ADC_ATTEN_DB_12)
#define HV_FB_ADC_BITWIDTH (ADC_BITWIDTH_DEFAULT)
#define HV_DIVIDER_TOP_RESISTOR_OHM (4000000LL)
#define HV_DIVIDER_BOTTOM_RESISTOR_OHM (24900LL)
#define HV_MONITOR_PERIOD_MS (1000)
#define HV_MONITOR_TASK_STACK (4096)
#define HV_MONITOR_TASK_PRIORITY (4)

static adc_oneshot_unit_handle_t s_hv_adc_handle;
static adc_cali_handle_t s_hv_adc_cali_handle;
static bool s_hv_adc_calibrated;
static bool s_hv_monitor_started;

static bool hv_monitor_adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = HV_FB_ADC_BITWIDTH,
    };
    ret = adc_cali_create_scheme_curve_fitting(&curve_config, &handle);
    if (ret == ESP_OK) {
        calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t line_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = HV_FB_ADC_BITWIDTH,
        };
        ret = adc_cali_create_scheme_line_fitting(&line_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    return calibrated;
}

static void hv_monitor_log_voltage(void)
{
    int raw = 0;
    int fb_mv = 0;

    if (adc_oneshot_read(s_hv_adc_handle, HV_FB_ADC_CHANNEL, &raw) != ESP_OK) {
        ESP_LOGE(HV_MONITOR_TAG, "Failed to read ADC on GPIO%d", HV_FB_GPIO);
        return;
    }

    if (s_hv_adc_calibrated) {
        if (adc_cali_raw_to_voltage(s_hv_adc_cali_handle, raw, &fb_mv) != ESP_OK) {
            ESP_LOGE(HV_MONITOR_TAG, "Failed to convert ADC raw=%d to voltage", raw);
            return;
        }
    } else {
        fb_mv = raw * 3300 / 4095;
    }

    int64_t hv_mv = (int64_t)fb_mv * (HV_DIVIDER_TOP_RESISTOR_OHM + HV_DIVIDER_BOTTOM_RESISTOR_OHM) / HV_DIVIDER_BOTTOM_RESISTOR_OHM;
    ESP_LOGI(HV_MONITOR_TAG,
             "EN GPIO%d=1, FB GPIO%d raw=%d, FB=%d mV, HV=%lld.%03lld V%s",
             HV_ENABLE_GPIO,
             HV_FB_GPIO,
             raw,
             fb_mv,
             (long long)(hv_mv / 1000),
             (long long)(hv_mv % 1000),
             s_hv_adc_calibrated ? "" : " (estimated)");
}

static void hv_monitor_task(void *arg)
{
    (void)arg;

    while (true) {
        hv_monitor_log_voltage();
        vTaskDelay(pdMS_TO_TICKS(HV_MONITOR_PERIOD_MS));
    }
}

esp_err_t hv_monitor_init(void)
{
    if (s_hv_monitor_started) {
        ESP_LOGW(HV_MONITOR_TAG, "HV monitor already started");
        return ESP_OK;
    }

    gpio_config_t hv_en_gpio_config = {
        .pin_bit_mask = 1ULL << HV_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&hv_en_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(HV_ENABLE_GPIO, 1));
    ESP_LOGI(HV_MONITOR_TAG, "HV output enabled on GPIO%d", HV_ENABLE_GPIO);

    adc_oneshot_unit_init_cfg_t adc_init_config = {
        .unit_id = HV_FB_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_init_config, &s_hv_adc_handle), HV_MONITOR_TAG, "HV ADC unit init failed");

    adc_oneshot_chan_cfg_t adc_channel_config = {
        .atten = HV_FB_ADC_ATTEN,
        .bitwidth = HV_FB_ADC_BITWIDTH,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_hv_adc_handle, HV_FB_ADC_CHANNEL, &adc_channel_config), HV_MONITOR_TAG, "HV ADC channel config failed");

    s_hv_adc_calibrated = hv_monitor_adc_calibration_init(HV_FB_ADC_UNIT, HV_FB_ADC_CHANNEL, HV_FB_ADC_ATTEN, &s_hv_adc_cali_handle);
    if (s_hv_adc_calibrated) {
        ESP_LOGI(HV_MONITOR_TAG, "ADC calibration enabled for GPIO%d", HV_FB_GPIO);
    } else {
        ESP_LOGW(HV_MONITOR_TAG, "ADC calibration unavailable for GPIO%d, using estimated voltage", HV_FB_GPIO);
    }

    hv_monitor_log_voltage();

    BaseType_t task_ok = xTaskCreate(hv_monitor_task,
                                     "hv_monitor",
                                     HV_MONITOR_TASK_STACK,
                                     NULL,
                                     HV_MONITOR_TASK_PRIORITY,
                                     NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, HV_MONITOR_TAG, "Failed to create HV monitor task");

    s_hv_monitor_started = true;
    return ESP_OK;
}
