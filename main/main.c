#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "driver/i2s_std.h"

// Dekodery Helix MP3 i AAC
#include "mp3dec.h"
#include "aacdec.h"

// Własne moduły
#include "sd_config.h"
#include "tuning.h"
#include "network.h"
#include "szum.h"

static const char *TAG = "WEB_RADIO_V16";

// Pinout I2S (PCM5102A)
#define I2S_BCK_IO          GPIO_NUM_26
#define I2S_WS_IO           GPIO_NUM_25
#define I2S_DO_IO           GPIO_NUM_22

// Rozmiary buforów
#define PSRAM_RINGBUF_SIZE  (256 * 1024)
#define PCM_RINGBUF_SIZE    (32 * 1024)

// Definicja globalnego stanu aplikacji (dostępna w innych modułach przez extern)
app_state_t g_state = {
    .current_station_idx = -1,
    .http_active = false,
    .volume = 80,
    .waga_radio = 0,
    .waga_szum = 100,
    .current_pf = 0.0f
};

// Zmienne globalne i synchronizacja
_Atomic uint32_t g_stream_generation = 0;
static _Atomic uint32_t g_pending_samprate = 44100;
static uint32_t g_current_samprate = 44100;

RingbufHandle_t g_psram_ringbuf = NULL;
RingbufHandle_t g_pcm_ringbuf = NULL;
SemaphoreHandle_t g_state_mutex = NULL;
static SemaphoreHandle_t g_pcm_reader_mutex = NULL;
static i2s_chan_handle_t g_tx_handle = NULL;

typedef struct {
    uint8_t *curr_item;
    size_t offset;
    size_t size;
} pcm_reader_t;

static pcm_reader_t g_pcm_reader = { .curr_item = NULL, .offset = 0, .size = 0 };

// -----------------------------------------------------------------------------
// Resetowanie buforów audio i wskaźników odczytu
// -----------------------------------------------------------------------------
void reset_audio_buffers(void) {
    xSemaphoreTake(g_pcm_reader_mutex, portMAX_DELAY);
    if (g_pcm_reader.curr_item != NULL) {
        vRingbufferReturnItem(g_pcm_ringbuf, (void *)g_pcm_reader.curr_item);
        g_pcm_reader.curr_item = NULL;
    }
    g_pcm_reader.offset = 0;
    g_pcm_reader.size = 0;

    size_t sz = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(g_psram_ringbuf, &sz, 0)) != NULL) {
        vRingbufferReturnItem(g_psram_ringbuf, item);
    }
    while ((item = xRingbufferReceive(g_pcm_ringbuf, &sz, 0)) != NULL) {
        vRingbufferReturnItem(g_pcm_ringbuf, item);
    }
    xSemaphoreGive(g_pcm_reader_mutex);
}

// -----------------------------------------------------------------------------
// Obsługa Zegara I2S
// -----------------------------------------------------------------------------
static void apply_i2s_clock_change(uint32_t new_samprate) {
    if (new_samprate == 0 || new_samprate == g_current_samprate) return;
    ESP_LOGI(TAG, "Zmiana zegara I2S: %lu Hz -> %lu Hz", g_current_samprate, new_samprate);
    i2s_channel_disable(g_tx_handle);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(new_samprate);
    i2s_channel_reconfig_std_clock(g_tx_handle, &clk_cfg);
    i2s_channel_enable(g_tx_handle);
    g_current_samprate = new_samprate;
}

static void init_i2s(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &g_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(g_current_samprate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCK_IO, .ws = I2S_WS_IO,
            .dout = I2S_DO_IO, .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_tx(g_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_tx_handle));
}

// -----------------------------------------------------------------------------
// Wątek Dekodera (Helix MP3 / AAC)
// -----------------------------------------------------------------------------
static void audio_decoder_task(void *arg) {
    HMP3Decoder hMP3Decoder = NULL;
    HAACDecoder hAACDecoder = NULL;
    int16_t pcm_out[2048 * 2];
    MP3FrameInfo mp3_info;
    AACFrameInfo aac_info;
    uint32_t local_gen = 0;

    while (1) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        bool active = g_state.http_active;
        int st_idx = g_state.current_station_idx;
        uint32_t cur_gen = atomic_load(&g_stream_generation);
        xSemaphoreGive(g_state_mutex);

        if (cur_gen != local_gen) {
            local_gen = cur_gen;
            if (hMP3Decoder) { MP3FreeDecoder(hMP3Decoder); hMP3Decoder = NULL; }
            if (hAACDecoder) { AACFreeDecoder(hAACDecoder); hAACDecoder = NULL; }
            hMP3Decoder = MP3InitDecoder();
            hAACDecoder = AACInitDecoder();
        }

        if (!active || st_idx < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool is_aac = (strcasecmp(g_stations[st_idx].codec, "aac") == 0);
        size_t bytes_read = 0;
        uint8_t *stream_ptr = (uint8_t *)xRingbufferReceive(g_psram_ringbuf, &bytes_read, pdMS_TO_TICKS(50));

        if (stream_ptr && bytes_read > 0) {
            int bytes_left = (int)bytes_read;
            uint8_t *in_buf = stream_ptr;

            while (bytes_left > 0) {
                    if (atomic_load(&g_stream_generation) != local_gen) {
						break;
					}

                if (is_aac) {
                    if (AACDecode(hAACDecoder, &in_buf, &bytes_left, pcm_out) == 0) {
                        AACGetLastFrameInfo(hAACDecoder, &aac_info);
                        if (aac_info.samprate > 0) {
                            atomic_store(&g_pending_samprate, aac_info.samprate);
                        }
                        size_t pcm_bytes = (aac_info.outputSamps * sizeof(int16_t)) & ~3;
                        if (pcm_bytes > 0) xRingbufferSend(g_pcm_ringbuf, pcm_out, pcm_bytes, pdMS_TO_TICKS(50));
                    } else { in_buf++; bytes_left--; }
                } else {
                    int offset = MP3FindSyncWord(in_buf, bytes_left);
                    if (offset < 0) break;
                    in_buf += offset; bytes_left -= offset;

                    if (MP3Decode(hMP3Decoder, &in_buf, &bytes_left, pcm_out, 0) == ERR_MP3_NONE) {
                        MP3GetLastFrameInfo(hMP3Decoder, &mp3_info);
                        if (mp3_info.samprate > 0) {
                            atomic_store(&g_pending_samprate, mp3_info.samprate);
                        }
                        size_t pcm_bytes = (mp3_info.outputSamps * sizeof(int16_t)) & ~3;
                        if (pcm_bytes > 0) xRingbufferSend(g_pcm_ringbuf, pcm_out, pcm_bytes, pdMS_TO_TICKS(50));
                    }
                }
            }
            vRingbufferReturnItem(g_psram_ringbuf, (void *)stream_ptr);
        }
    }
}

// -----------------------------------------------------------------------------
// Pobieranie ramki stereo
// -----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    int16_t left;
    int16_t right;
} stereo_sample_t;

static bool read_pcm_stereo_frame(stereo_sample_t *out_frame) {
    bool success = false;
    xSemaphoreTake(g_pcm_reader_mutex, portMAX_DELAY);

    if (g_pcm_reader.curr_item == NULL) {
        size_t item_size = 0;
        g_pcm_reader.curr_item = (uint8_t *)xRingbufferReceive(g_pcm_ringbuf, &item_size, 0);
        if (g_pcm_reader.curr_item != NULL) {
            g_pcm_reader.size = item_size & ~3;
            g_pcm_reader.offset = 0;

            if (g_pcm_reader.size == 0) {
                vRingbufferReturnItem(g_pcm_ringbuf, (void *)g_pcm_reader.curr_item);
                g_pcm_reader.curr_item = NULL;
            }
        }
    }

    if (g_pcm_reader.curr_item != NULL) {
        stereo_sample_t *src_sample = (stereo_sample_t *)(g_pcm_reader.curr_item + g_pcm_reader.offset);
        *out_frame = *src_sample;
        g_pcm_reader.offset += sizeof(stereo_sample_t);
        success = true;

        if (g_pcm_reader.offset >= g_pcm_reader.size) {
            vRingbufferReturnItem(g_pcm_ringbuf, (void *)g_pcm_reader.curr_item);
            g_pcm_reader.curr_item = NULL;
            g_pcm_reader.offset = 0;
            g_pcm_reader.size = 0;
        }
    }

    xSemaphoreGive(g_pcm_reader_mutex);
    return success;
}

// -----------------------------------------------------------------------------
// Wątek Odczytu i Zapisu I2S
// -----------------------------------------------------------------------------
static void i2s_audio_task(void *arg) {
    stereo_sample_t frame_buf[256];
    size_t bytes_written = 0;

    while (1) {
        uint32_t req_samprate = atomic_load(&g_pending_samprate);
        if (req_samprate != g_current_samprate) {
            apply_i2s_clock_change(req_samprate);
        }

        int samples_count = 0;

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        uint8_t main_vol = g_state.volume;
        uint8_t w_radio = g_state.waga_radio;
        uint8_t w_szum = g_state.waga_szum;
        xSemaphoreGive(g_state_mutex);

        while (samples_count < 256) {
            stereo_sample_t radio_frame = {0, 0};
            bool has_audio = read_pcm_stereo_frame(&radio_frame);

            int16_t noise = generate_brown_noise();

            int32_t mix_l = 0;
            int32_t mix_r = 0;

            if (has_audio && w_radio > 0) {
                mix_l += ((int32_t)radio_frame.left * w_radio) / 100;
                mix_r += ((int32_t)radio_frame.right * w_radio) / 100;
            }

            if (w_szum > 0) {
                mix_l += ((int32_t)noise * w_szum) / 100;
                mix_r += ((int32_t)noise * w_szum) / 100;
            }

            mix_l = (mix_l * main_vol) / 100;
            mix_r = (mix_r * main_vol) / 100;

            if (mix_l > 32767) mix_l = 32767; else if (mix_l < -32768) mix_l = -32768;
            if (mix_r > 32767) mix_r = 32767; else if (mix_r < -32768) mix_r = -32768;

            frame_buf[samples_count].left = (int16_t)mix_l;
            frame_buf[samples_count].right = (int16_t)mix_r;
            samples_count++;
        }

        i2s_channel_write(g_tx_handle, frame_buf, samples_count * sizeof(stereo_sample_t), 
                          &bytes_written, portMAX_DELAY);
    }
}

// -----------------------------------------------------------------------------
// Główna funkcja programu
// -----------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "Inicjalizacja Radia Internetowego V16...");

    g_state_mutex = xSemaphoreCreateMutex();
    g_pcm_reader_mutex = xSemaphoreCreateMutex();
    
    g_psram_ringbuf = xRingbufferCreate(PSRAM_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    g_pcm_ringbuf = xRingbufferCreate(PCM_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);

    init_i2s();

    char wifi_ssid[64] = {0};
    char wifi_pass[64] = {0};

    if (init_sd_card() == ESP_OK) {
        load_config_from_sd(wifi_ssid, wifi_pass);
    } else {
        ESP_LOGE(TAG, "Nie udało się zamontować karty SD!");
    }

    if (strlen(wifi_ssid) > 0) {
        connect_wifi(wifi_ssid, wifi_pass);
    }

    xTaskCreatePinnedToCore(http_task, "http_task", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(audio_decoder_task, "decoder_task", 16384, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(i2s_audio_task, "i2s_task", 4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(tuning_task, "tuning_task", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "Gotowe (ESP-IDF 5.2.6).");
}