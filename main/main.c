#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "secrets.h"

static QueueHandle_t msg_queue;

#define WIFI_CONNECTED_BIT BIT0
#define LED_GPIO GPIO_NUM_27 // Built-in LED on many ESP32 boards
static const char *TAG = "HTTPS_APP";
static EventGroupHandle_t wifi_event_group;
extern const uint8_t ca_pem_start[] asm("_binary_ca_pem_start");
extern const uint8_t ca_pem_end[] asm("_binary_ca_pem_end");
typedef struct struct_message
{
    int id;
    float temperature;
    float humidity;
} message_t;

/* ---- Wi-Fi Event Handler ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        //  esp_wifi_connect();
        ESP_LOGI(TAG, "📡 Connecting to Wi-Fi...");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "⚠️ Disconnected. Reconnecting...");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "✅ Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
/* ---- HTTPS Function ---- */
void send_to_server(int id, float temperature, float humidity)
{
    esp_http_client_config_t config = {
        .url = "https://pretelephonic-superconducting-don.ngrok-free.dev/api/data", // test endpoint
        .crt_bundle_attach = esp_crt_bundle_attach,
        .cert_pem = (const char *)ca_pem_start,
        .skip_cert_common_name_check = true,
        .timeout_ms = 8000};
    // esp_http_client_config_t config = {
    //     .url = "https://postman-echo.com/post",
    //     .crt_bundle_attach = esp_crt_bundle_attach,
    // };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    // JSON payload
    char post_data[128];
    snprintf(post_data, sizeof(post_data),
             "{\"class_name\":%d,\"temperature\":%.2f,\"humidity\":%.2f}",
             id, temperature, humidity);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    ESP_LOGI(TAG, "🚀 Sending POST: %s", post_data);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "✅ HTTPS POST OK, status = %d", status);

        // Optional: read the body
        char buffer[256];
        int len = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
        if (len > 0)
        {
            buffer[len] = '\0';
            ESP_LOGI(TAG, "📦 Response: %s", buffer);
        }
    }
    else
    {
        ESP_LOGE(TAG, "❌ HTTPS POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
void https_task(void *pv)
{
    message_t msg;
    while (1)
    {
        if (xQueueReceive(msg_queue, &msg, portMAX_DELAY))
        {
            send_to_server(msg.id, msg.temperature, msg.humidity);
        }
    }
}

void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    message_t msg;
    memcpy(&msg, data, sizeof(msg));

    ESP_LOGI(TAG, "📩 Received: id=%d temp=%.2f hum=%.2f", msg.id, msg.temperature, msg.humidity);

    // Optional LED feedback
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(LED_GPIO, 0);

    // --- 📴 Temporarily disable ESP-NOW before HTTPS ---
    // ESP_LOGI(TAG, "🛑 Pausing ESP-NOW to perform HTTPS...");
    // esp_now_deinit();
    vTaskDelay(pdMS_TO_TICKS(200)); // small delay to settle the radio

    // --- 🌐 Perform HTTPS POST ---
    xQueueSend(msg_queue, &msg, 0);

    // --- 🔁 Re-enable ESP-NOW after sending ---
    // ESP_LOGI(TAG, "🔄 Re-enabling ESP-NOW...");
    // if (esp_now_init() == ESP_OK) {
    //     esp_now_register_recv_cb(OnDataRecv);
    //     ESP_LOGI(TAG, "✅ ESP-NOW resumed successfully.");
    // } else {
    //     ESP_LOGE(TAG, "❌ Failed to re-init ESP-NOW after HTTPS!");
    // }
}

/* ---- Initialize Wi-Fi ---- */
void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register events BEFORE start
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // for esp now and wifi to coexist
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "🚀 Wi-Fi initialized");

    ESP_ERROR_CHECK(esp_now_init());
}

/* ---- Main ---- */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    // ⏳ Wait for Wi-Fi connection and IP
    ESP_LOGI(TAG, "Waiting for Wi-Fi...");
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        false, true,
        pdMS_TO_TICKS(15000) // 15 seconds max
    );

    if (bits & WIFI_CONNECTED_BIT)
    {
        // Initialize ESP-NOW
        ESP_LOGI(TAG, "✅ Wi-Fi connected, ready to receive ESP-NOW data.");
        uint8_t channel;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&channel, &second);
        ESP_LOGI("WiFi", "📡 Receiver channel: %d", channel);

        // vTaskDelay(pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG, "🚀 ESP-NOW initialized");
        msg_queue = xQueueCreate(5, sizeof(message_t));
        xTaskCreate(https_task, "https_task", 8192, NULL, 5, NULL);
        esp_now_register_recv_cb(OnDataRecv);
    }
    else
    {
        ESP_LOGE(TAG, "❌ Timeout waiting for Wi-Fi connection.");
    }
}
