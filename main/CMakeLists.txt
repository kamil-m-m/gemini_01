idf_component_register(
    SRCS 
        "main.c"
        "network.c"
        "sd_config.c"
        "szum.c"
        "tuning.c"
        # Pliki źródłowe dekodera Helix MP3
        "helix_mp3/mp3dec.c"
        "helix_mp3/mp3tabs.c"
        # Pliki źródłowe dekodera Helix AAC
        "helix_aac/aacdec.c"
        "helix_aac/aactabs.c"
    INCLUDE_DIRS 
        "."
        "helix_mp3"
        "helix_aac"
    REQUIRES
        driver
        esp_ringbuf
        esp_system
        esp_psram
        esp_http_client
        nvs_flash
        esp_wifi
        fatfs
)