#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"

#define WIFI_SSID "ESP32_AP"
#define WIFI_PASS "34081948"
#define MAXIMUM_RETRY 5

#define UDP_PORT 3333
#define UDP_SERVER_IP "192.168.4.1"
#define UDP_BUFFER_SIZE 128

static const char *TAG = "WiFi_Client";

// Wi-Fi 연결 상태를 나타내는 이벤트 그룹
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// 재시도 횟수
static int retry_count = 0;

// Wi-Fi Event Handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (retry_count < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retrying to connect to the AP...");
        }
        else
        {
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "Failed to connect to the AP");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Wi-Fi 초기화 및 연결
void wifi_init_sta()
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA initialized.");
}

// UDP 클라이언트 태스크
void udp_client_task(void *pvParameters)
{
    char rx_buffer[UDP_BUFFER_SIZE];
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = inet_addr(UDP_SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);

    // UDP 소켓 생성
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    ESP_LOGI(TAG, "Socket created");

    // UDP Server와의 연결을 위한 최초 송신
    static const char *payload = "Connected to UDP Client";
    // Wi-Fi 연결 확인

    int err = -1;

    while (1)
    {
        if (xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY))
        {
            if (err < 0)
            {
                while (err < 0)
                {
                    err = sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                }
                ESP_LOGI(TAG, "Message sent");
            }
            else
            {
                // UDP 데이터 수신
                struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
                socklen_t socklen = sizeof(source_addr);

                int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
                if (len < 0)
                {
                    ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                    err = -1;
                }
                else
                {
                    rx_buffer[len] = 0; // NULL-terminate the string
                    ESP_LOGI(TAG, "Received data: %s", rx_buffer);
                    static const char *flag_received_complete = "Data received";
                    err = sendto(sock, flag_received_complete, strlen(flag_received_complete), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                
                }
            }
        }
    }
    if (sock != -1)
    {
        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        close(sock);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
}
