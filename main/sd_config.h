#ifndef SD_CONFIG_H
#define SD_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MAX_STATIONS 20

// -----------------------------------------------------------------------------
// Struktury danych
// -----------------------------------------------------------------------------

// Definicja pojedynczej stacji radiowej
typedef struct {
    char name[32];
    char url[128];
    char codec[8];       // "mp3" lub "aac"
    float target_pf;     // Pojemność docelowa w pF (np. dla kondensatora obrotowego)
} station_t;

// Globalny stan aplikacji
typedef struct {
    int current_station_idx;
    bool http_active;
    uint8_t volume;
    uint8_t waga_radio;
    uint8_t waga_szum;
    float current_pf;
} app_state_t;

// -----------------------------------------------------------------------------
// Zmienne globalne (zadeklarowane w sd_config.c lub main.c)
// -----------------------------------------------------------------------------

extern station_t g_stations[MAX_STATIONS];
extern int g_stations_count;

// -----------------------------------------------------------------------------
// Prototypy funkcji obsługi karty SD
// -----------------------------------------------------------------------------

/**
 * @brief Inicjalizuje magistralę SPI oraz montuje system plików FatFS z karty SD.
 * @return ESP_OK w przypadku sukcesu, odpowiedni kod błędu ESP-IDF w przeciwnym razie.
 */
esp_err_t init_sd_card(void);

/**
 * @brief Odczytuje plik konfiguracyjny z karty SD, ładuje ustawienia WiFi oraz listę stacji.
 * 
 * @param out_ssid Bufor na nazwę sieci WiFi
 * @param out_pass Bufor na hasło do sieci WiFi
 */
void load_config_from_sd(char *out_ssid, char *out_pass);

#endif // SD_CONFIG_H