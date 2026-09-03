#include "sd_config.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc.h"

static const char *TAG = "SD_CONFIG";

station_t g_stations[MAX_STATIONS];
int g_stations_count = 0;

esp_err_t init_sd_card(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "Montowanie karty SD (SDMMC)...");
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, NULL);
}

void load_config_from_sd(char *wifi_ssid, char *wifi_pass) {
    FILE *f_wifi = fopen("/sdcard/wifi.txt", "r");
    if (f_wifi) {
        fgets(wifi_ssid, 64, f_wifi);
        fgets(wifi_pass, 64, f_wifi);
        wifi_ssid[strcspn(wifi_ssid, "\r\n")] = 0;
        wifi_pass[strcspn(wifi_pass, "\r\n")] = 0;
        fclose(f_wifi);
        ESP_LOGI(TAG, "Wczytano WiFi z SD: SSID='%s'", wifi_ssid);
    } else {
        ESP_LOGE(TAG, "Brak pliku /sdcard/wifi.txt!");
    }

    FILE *f_st = fopen("/sdcard/listastacji.txt", "r");
    if (f_st) {
        char line[384];
        g_stations_count = 0;
        while (fgets(line, sizeof(line), f_st) && g_stations_count < MAX_STATIONS) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0 || line[0] == '#') continue;

            station_t *st = &g_stations[g_stations_count];
            if (sscanf(line, "%f;%63[^;];%255[^;];%7s", 
                       &st->target_pf, st->name, st->url, st->codec) == 4) {
                ESP_LOGI(TAG, "Stacja [%d]: %.1fpF | %s | %s | %s", 
                         g_stations_count, st->target_pf, st->name, st->codec, st->url);
                g_stations_count++;
            }
        }
        fclose(f_st);
    } else {
        ESP_LOGE(TAG, "Brak pliku /sdcard/listastacji.txt!");
    }
}