/**
 * Optional cloud bridge for xiaozhi-esp32: POST events to Magic Wand Railway backend.
 * Requires: CONFIG_MAGIC_WAND_CLOUD_BRIDGE=y, esp_http_client in PRIV_REQUIRES.
 */
#include "cloud_bridge.h"

#if CONFIG_MAGIC_WAND_CLOUD_BRIDGE

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstring>
#include <string>

#define TAG "CloudBridge"
#define QUEUE_LEN 8
#define PAYLOAD_MAX 512

struct QueueItem {
    char label[24];
    char text[256];
};

static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_task = nullptr;
static bool s_inited = false;

// CONFIG_MAGIC_WAND_CLOUD_URL and CONFIG_MAGIC_WAND_DEVICE_ID come from Kconfig (sdkconfig)

static void build_payload(char* out, size_t out_size, const char* label, const char* text) {
    long ts = (long)(esp_timer_get_time() / 1000000);
    int n = snprintf(out, out_size,
        "{\"device_id\":\"%s\",\"label\":\"%s\",\"score\":0,\"sensor\":\"xiaozhi\",\"timestamp\":%ld",
        CONFIG_MAGIC_WAND_DEVICE_ID, label, (long)ts);
    if (text && text[0] != '\0') {
        // Escape " and \ in text for JSON
        n += snprintf(out + n, out_size - (size_t)n, ",\"extra\":{\"text\":\"");
        for (const char* p = text; *p && n < (int)(out_size - 4); p++) {
            if (*p == '"' || *p == '\\') { out[n++] = '\\'; }
            out[n++] = *p;
        }
        n += snprintf(out + n, out_size - (size_t)n, "\"}");
    } else {
        n += snprintf(out + n, out_size - (size_t)n, "}");
    }
}

static void send_one(const char* label, const char* text) {
    char payload[PAYLOAD_MAX];
    build_payload(payload, sizeof(payload), label, text);

    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_MAGIC_WAND_CLOUD_URL;
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.skip_cert_common_name_check = true;
    cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST failed %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "POST ok label=%s", label);
        } else {
            ESP_LOGW(TAG, "POST status %d", status);
        }
    }
    esp_http_client_cleanup(client);
}

static void bridge_task(void* arg) {
    QueueItem item;
    while (true) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) == pdTRUE) {
            send_one(item.label, item.text);
        }
    }
}

void CloudBridge::Init() {
    if (s_inited) return;
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(QueueItem));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    BaseType_t ok = xTaskCreate(bridge_task, "cloud_br", 4096, nullptr, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return;
    }
    s_inited = true;
    ESP_LOGI(TAG, "init ok url=%s", CONFIG_MAGIC_WAND_CLOUD_URL);
}

void CloudBridge::SendEvent(const char* label, const char* text) {
    if (!s_inited || !label) return;
    QueueItem item;
    strncpy(item.label, label, sizeof(item.label) - 1);
    item.label[sizeof(item.label) - 1] = '\0';
    if (text) {
        strncpy(item.text, text, sizeof(item.text) - 1);
        item.text[sizeof(item.text) - 1] = '\0';
    } else {
        item.text[0] = '\0';
    }
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full drop label=%s", label);
    }
}

#else  // !CONFIG_MAGIC_WAND_CLOUD_BRIDGE

void CloudBridge::Init() {}
void CloudBridge::SendEvent(const char*, const char*) {}

#endif
