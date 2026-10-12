#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "geiger_counter.h"

#define GEIGER_TAG "GEIGER"

#define GEIGER_COUNT_GPIO (GPIO_NUM_4)
#define GEIGER_BUZZER_GPIO (GPIO_NUM_3)
#define GEIGER_COUNT_INTR_TYPE (GPIO_INTR_POSEDGE)

#define GEIGER_CPM_WINDOW_SECONDS (60)
#define GEIGER_STATS_PERIOD_MS (1000)
#define GEIGER_STATS_TASK_STACK (4096)
#define GEIGER_STATS_TASK_PRIORITY (4)

#define GEIGER_BUZZER_LEDC_MODE (LEDC_LOW_SPEED_MODE)
#define GEIGER_BUZZER_LEDC_TIMER (LEDC_TIMER_0)
#define GEIGER_BUZZER_LEDC_CHANNEL (LEDC_CHANNEL_0)
#define GEIGER_BUZZER_FREQ_HZ (3000)
#define GEIGER_BUZZER_DUTY_RES (LEDC_TIMER_10_BIT)
#define GEIGER_BUZZER_DUTY_ON (512)
#define GEIGER_BUZZER_CLICK_ON_MS (8)
#define GEIGER_BUZZER_CLICK_OFF_MS (12)
#define GEIGER_BUZZER_TASK_STACK (3072)
#define GEIGER_BUZZER_TASK_PRIORITY (5)

static portMUX_TYPE s_geiger_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_buzzer_task_handle;
static bool s_geiger_started;
static uint32_t s_total_pulses;
static uint32_t s_pulses_in_current_second;

static void geiger_buzzer_set(bool enabled)
{
    uint32_t duty = enabled ? GEIGER_BUZZER_DUTY_ON : 0;
    ledc_set_duty(GEIGER_BUZZER_LEDC_MODE, GEIGER_BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(GEIGER_BUZZER_LEDC_MODE, GEIGER_BUZZER_LEDC_CHANNEL);
}

static void IRAM_ATTR geiger_gpio_isr_handler(void *arg)
{
    (void)arg;

    portENTER_CRITICAL_ISR(&s_geiger_lock);
    s_total_pulses++;
    s_pulses_in_current_second++;
    portEXIT_CRITICAL_ISR(&s_geiger_lock);

    BaseType_t high_task_wakeup = pdFALSE;
    if (s_buzzer_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_buzzer_task_handle, &high_task_wakeup);
    }
    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void geiger_buzzer_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t pulses = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (pulses-- > 0) {
            geiger_buzzer_set(true);
            vTaskDelay(pdMS_TO_TICKS(GEIGER_BUZZER_CLICK_ON_MS));
            geiger_buzzer_set(false);
            vTaskDelay(pdMS_TO_TICKS(GEIGER_BUZZER_CLICK_OFF_MS));
        }
    }
}

static void geiger_stats_task(void *arg)
{
    (void)arg;

    uint32_t history[GEIGER_CPM_WINDOW_SECONDS] = {0};
    uint32_t history_sum = 0;
    size_t history_index = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(GEIGER_STATS_PERIOD_MS));

        uint32_t pulses_this_second = 0;
        uint32_t total_pulses = 0;

        portENTER_CRITICAL(&s_geiger_lock);
        pulses_this_second = s_pulses_in_current_second;
        s_pulses_in_current_second = 0;
        total_pulses = s_total_pulses;
        portEXIT_CRITICAL(&s_geiger_lock);

        history_sum -= history[history_index];
        history[history_index] = pulses_this_second;
        history_sum += pulses_this_second;
        history_index = (history_index + 1) % GEIGER_CPM_WINDOW_SECONDS;

        ESP_LOGI(GEIGER_TAG,
                 "GPIO%d pulses=%lu in last 1s, CPM=%lu, total=%lu",
                 GEIGER_COUNT_GPIO,
                 (unsigned long)pulses_this_second,
                 (unsigned long)history_sum,
                 (unsigned long)total_pulses);
    }
}

static esp_err_t geiger_buzzer_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = GEIGER_BUZZER_LEDC_MODE,
        .timer_num = GEIGER_BUZZER_LEDC_TIMER,
        .duty_resolution = GEIGER_BUZZER_DUTY_RES,
        .freq_hz = GEIGER_BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), GEIGER_TAG, "Failed to configure buzzer timer");

    const ledc_channel_config_t channel_config = {
        .gpio_num = GEIGER_BUZZER_GPIO,
        .speed_mode = GEIGER_BUZZER_LEDC_MODE,
        .channel = GEIGER_BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = GEIGER_BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), GEIGER_TAG, "Failed to configure buzzer channel");

    BaseType_t task_ok = xTaskCreate(geiger_buzzer_task,
                                     "geiger_buzzer",
                                     GEIGER_BUZZER_TASK_STACK,
                                     NULL,
                                     GEIGER_BUZZER_TASK_PRIORITY,
                                     &s_buzzer_task_handle);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, GEIGER_TAG, "Failed to create buzzer task");

    ESP_LOGI(GEIGER_TAG, "Buzzer PWM ready on GPIO%d at %d Hz", GEIGER_BUZZER_GPIO, GEIGER_BUZZER_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t geiger_counter_gpio_init(void)
{
    const gpio_config_t counter_gpio_config = {
        .pin_bit_mask = 1ULL << GEIGER_COUNT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GEIGER_COUNT_INTR_TYPE,
    };
    ESP_ERROR_CHECK(gpio_config(&counter_gpio_config));

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(GEIGER_COUNT_GPIO, geiger_gpio_isr_handler, NULL), GEIGER_TAG, "Failed to add GPIO ISR");
    ESP_LOGI(GEIGER_TAG, "Count input ready on GPIO%d", GEIGER_COUNT_GPIO);
    return ESP_OK;
}

esp_err_t geiger_counter_init(void)
{
    if (s_geiger_started) {
        ESP_LOGW(GEIGER_TAG, "Geiger counter already started");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(geiger_buzzer_init(), GEIGER_TAG, "Buzzer init failed");
    ESP_RETURN_ON_ERROR(geiger_counter_gpio_init(), GEIGER_TAG, "Counter GPIO init failed");

    BaseType_t task_ok = xTaskCreate(geiger_stats_task,
                                     "geiger_stats",
                                     GEIGER_STATS_TASK_STACK,
                                     NULL,
                                     GEIGER_STATS_TASK_PRIORITY,
                                     NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, GEIGER_TAG, "Failed to create stats task");

    s_geiger_started = true;
    return ESP_OK;
}
