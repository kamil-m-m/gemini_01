#include "tuning.h"
#include "sd_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"

#define TAG "TUNING"
#define PCNT_INPUT_SIG_IO GPIO_NUM_34
#define THRESH_CONNECT 5.5f
#define THRESH_DISCONNECT 6.5f

typedef struct {
    int current_station_idx;
    bool http_active;
    uint8_t volume;
    uint8_t waga_radio;
    uint8_t waga_szum;
    float current_pf;
} app_state_t;

extern app_state_t g_state;
extern SemaphoreHandle_t g_state_mutex;
extern _Atomic uint32_t g_stream_generation;
extern void reset_audio_buffers(void);

static pcnt_unit_handle_t g_pcnt_unit = NULL;

static void init_pcnt(void) {
    pcnt_unit_config_t unit_config = {
        .high_limit = 32767,
        .low_limit = -32768,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &g_pcnt_unit));

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = PCNT_INPUT_SIG_IO,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(g_pcnt_unit, &chan_config, &pcnt_chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));

    ESP_ERROR_CHECK(pcnt_unit_enable(g_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(g_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(g_pcnt_unit));
}

static float frequency_to_pf(uint32_t freq_hz) {
    if (freq_hz == 0) return 0.0f;
    const float R_sum = 21000.0f; 
    float c_farads = 1.44f / (R_sum * (float)freq_hz);
    return c_farads * 1e12f;
}

void tuning_task(void *arg) {
    int count = 0;
    init_pcnt();

    while (1) {
        pcnt_unit_clear_count(g_pcnt_unit);
        vTaskDelay(pdMS_TO_TICKS(100));
        pcnt_unit_get_count(g_pcnt_unit, &count);

        uint32_t freq_hz = (uint32_t)abs(count) * 10;
        float current_pf = frequency_to_pf(freq_hz);

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_state.current_pf = current_pf;

        int active_idx = g_state.current_station_idx;
        bool is_connected = g_state.http_active;

        if (is_connected && active_idx >= 0 && active_idx < g_stations_count) {
            float diff = fabsf(current_pf - g_stations[active_idx].target_pf);

            if (diff > THRESH_DISCONNECT) {
                g_state.http_active = false;
                g_state.current_station_idx = -1;
                g_state.waga_radio = 0;
                g_state.waga_szum = 100;
                atomic_fetch_add(&g_stream_generation, 1);
                reset_audio_buffers();
                ESP_LOGI(TAG, "DISCONNECT: Odłączono strumień (ΔpF = %.2f)", diff);
            } else {
                float signal = 100.0f * (1.0f - (diff / THRESH_DISCONNECT));
                if (signal < 0) signal = 0;
                g_state.waga_radio = (uint8_t)signal;
                g_state.waga_szum = 100 - g_state.waga_radio;
            }
        } else {
            int closest_idx = -1;
            float min_diff = 9999.0f;

            for (int i = 0; i < g_stations_count; i++) {
                float diff = fabsf(current_pf - g_stations[i].target_pf);
                if (diff < min_diff) {
                    min_diff = diff;
                    closest_idx = i;
                }
            }

            if (closest_idx != -1 && min_diff <= THRESH_CONNECT) {
                g_state.current_station_idx = closest_idx;
                g_state.http_active = true;
                atomic_fetch_add(&g_stream_generation, 1);
                reset_audio_buffers();

                float signal = 100.0f * (1.0f - (min_diff / THRESH_DISCONNECT));
                g_state.waga_radio = (uint8_t)signal;
                g_state.waga_szum = 100 - g_state.waga_radio;

                ESP_LOGI(TAG, "CONNECT: Połączono ze stacją %s (ΔpF = %.2f)", 
                         g_stations[closest_idx].name, min_diff);
            } else {
                g_state.waga_radio = 0;
                g_state.waga_szum = 100;
            }
        }

        xSemaphoreGive(g_state_mutex);
    }
}