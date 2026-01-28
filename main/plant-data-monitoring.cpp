#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_adc/adc_oneshot.h"
#include "sdkconfig.h"

extern "C" {
    #include "dht.h"
}

static const char *TAG = "PLANT_DATA";

// --- PROJECT CONFIGURATION ---
/*Define in Kconfig.projbuild
#define WIFI_SSID           
#define WIFI_PASS           
// Hivemq connection
#define BROKER_URL         
#define BROKER_USER         
#define BROKER_PASS         
*/

#define DHT_GPIO            (gpio_num_t)4
#define SOIL_ADC_CHAN       ADC_CHANNEL_6 

extern const uint8_t isrg_root_x1_pem_start[] asm("_binary_isrg_root_x1_pem_start");
extern const uint8_t isrg_root_x1_pem_end[]   asm("_binary_isrg_root_x1_pem_end");

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;

/** Wi-Fi Event Handler */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying Wi-Fi connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP address.");
    }
}

/** MQTT Event Handler */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data) {
    auto event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Cloud Broker Connection Success");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Broker Disconnected");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error Occurred");
            break;
        default:
            break;
    }
}

void sensor_task(void *pvParameters) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)pvParameters;
    
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {};
    init_config1.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t adc_cfg = {};
    adc_cfg.atten = ADC_ATTEN_DB_11;
    adc_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SOIL_ADC_CHAN, &adc_cfg));

    while (1) {
        float temp, hum;
        int soil_raw;
        
        esp_err_t dht_res = dht_read_float_data(DHT_TYPE_AM2301, DHT_GPIO, &hum, &temp);
        
        esp_err_t adc_res = adc_oneshot_read(adc1_handle, SOIL_ADC_CHAN, &soil_raw);

        if (dht_res == ESP_OK && adc_res == ESP_OK) {
            char payload[128];
            snprintf(payload, sizeof(payload), 
                     "{\"temp\": %.1f, \"humidity\": %.1f, \"soil_raw\": %d}", 
                     temp, hum, soil_raw);
            
            ESP_LOGI(TAG, "Publishing: %s", payload);
            esp_mqtt_client_publish(client, "plants/travel-node/telemetry", payload, 0, 1, 0);
        } else {
            ESP_LOGE(TAG, "Sensor Read Error (DHT: %d, ADC: %d)", dht_res, adc_res);
        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds
    }
}

extern "C" void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = BROKER_URL;
    mqtt_cfg.credentials.username = BROKER_USER;
    mqtt_cfg.credentials.authentication.password = BROKER_PASS;
    
    mqtt_cfg.broker.verification.certificate = (const char *)isrg_root_x1_pem_start;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    xTaskCreate(sensor_task, "sensor_task", 4096, client, 5, NULL);
}