idf_component_register(
    SRCS "plant-data-monitoring.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        mqtt
        esp_adc
        dht
    EMBED_TXTFILES "isrg_root_x1.pem"
)