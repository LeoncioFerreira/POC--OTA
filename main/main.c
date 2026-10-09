#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRY 10
static const char *TAG = "poc_ota";
static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count++ < WIFI_MAXIMUM_RETRY) esp_wifi_connect();
        else xEventGroupSetBits(wifi_event_group, WIFI_FAILED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi conectado: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (strlen(CONFIG_POC_WIFI_SSID)) {
        wifi_config_t config = {0};
        strlcpy((char *)config.sta.ssid, CONFIG_POC_WIFI_SSID, sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password, CONFIG_POC_WIFI_PASSWORD, sizeof(config.sta.password));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
        ESP_LOGI(TAG, "Configuração Wi-Fi atualizada pelo firmware");
    } else {
        wifi_config_t stored = {0};
        ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &stored));
        if (!strlen((char *)stored.sta.ssid)) {
            ESP_LOGE(TAG, "Nenhuma rede Wi-Fi armazenada; configure por USB uma vez");
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Usando rede Wi-Fi armazenada no NVS: %s", stored.sta.ssid);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

static void confirm_running_firmware(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Confirmando firmware novo como válido");
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
}

static esp_err_t perform_ota(void)
{
    esp_http_client_config_t http = {
        .url = CONFIG_POC_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t config = {.http_config = &http};
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&config, &handle);
    if (err != ESP_OK) return err;
    esp_app_desc_t remote;
    err = esp_https_ota_get_img_desc(handle, &remote);
    if (err != ESP_OK) { esp_https_ota_abort(handle); return err; }
    const esp_app_desc_t *current = esp_app_get_description();
    ESP_LOGI(TAG, "Atual: %s | disponível: %s", current->version, remote.version);
    if (!strcmp(current->version, remote.version)) {
        ESP_LOGI(TAG, "Firmware já está atualizado");
        esp_https_ota_abort(handle);
        return ESP_OK;
    }
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        ESP_LOGI(TAG, "OTA: %d bytes", esp_https_ota_get_image_len_read(handle));
    if (err != ESP_OK) { esp_https_ota_abort(handle); return err; }
    err = esp_https_ota_finish(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA concluído; reiniciando");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return err;
}

static void ota_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10000));
    while (true) {
        esp_err_t err = perform_ota();
        if (err != ESP_OK) ESP_LOGE(TAG, "OTA falhou: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_POC_OTA_CHECK_INTERVAL_SECONDS * 1000));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "POC OTA iniciada; versão %s", esp_app_get_description()->version);
    if (wifi_connect() == ESP_OK) {
        confirm_running_firmware();
        xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
    }
}
