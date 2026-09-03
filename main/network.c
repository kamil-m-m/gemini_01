#include "network.h"
#include "sd_config.h"
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#define TAG "NETWORK"

// Zmienne zadeklarowane w main.c
extern app_state_t g_state;
extern SemaphoreHandle_t g_state_mutex;
extern _Atomic uint32_t g_stream_generation;
extern RingbufHandle_t g_psram_ringbuf;

void connect_wifi(const char *ssid, const char *pass) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Łączenie z WiFi: %s...", ssid);
}

void http_task(void *arg) {
    uint8_t http_buf[1024];

    while (1) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);

        int st_idx = g_state.current_station_idx;
        bool active = g_state.http_active;
        uint32_t my_gen = atomic_load(&g_stream_generation);

        xSemaphoreGive(g_state_mutex);

        if (!active || st_idx < 0 || st_idx >= g_stations_count) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        esp_http_client_config_t cfg = {
            .url = g_stations[st_idx].url,
            .timeout_ms = 4000,
            .buffer_size = 2048,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);

        if (client) {
            if (esp_http_client_open(client, 0) == ESP_OK) {
                esp_http_client_fetch_headers(client);

                while (1) {
                    // Sprawdzenie przed odczytem
                    if (atomic_load(&g_stream_generation) != my_gen) {
                        break;
                    }

                    int read_bytes = esp_http_client_read(
                        client,
                        (char *)http_buf,
                        sizeof(http_buf)
                    );

                    if (read_bytes <= 0) {
                        break;
                    }

                    // Sprawdzenie po odczycie, przed wrzuceniem do bufora PSRAM
                    if (atomic_load(&g_stream_generation) != my_gen) {
                        break;
                    }

                    xRingbufferSend(
                        g_psram_ringbuf,
                        http_buf,
                        read_bytes,
                        pdMS_TO_TICKS(500)
                    );
                }
                // Zamknięcie połączenia tylko jeśli zostało poprawnie otwarte
                esp_http_client_close(client);
            }
            // Zwolnienie zasobów klienta HTTP
            esp_http_client_cleanup(client);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}