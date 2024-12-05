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
#include "sdkconfig.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"

// Wifi Credentials
const char *SSID = "ESP32_AP";
const char *PWD = "34081948";
uint8_t channel = 11;
uint8_t bw = 20; // 20 or 40
int8_t power = 8;

wifi_config_t g_ap_config = {
    .ap.max_connection = 4,
    .ap.authmode = WIFI_AUTH_WPA2_PSK};

static const char *TAG_AP = "WIFI-AP";

#define UDP_PORT 3333
#define UDP_BUFFER_SIZE 128

#define DATA_SEND_INTERVAL_MS 500 // 전송 주기 (밀리초 단위)

static const char *TAG = "WiFi_AP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// UDP socket
static int udp_socket = -1;

// 클라이언트 주소 정보
struct sockaddr_storage client_addr; // Large enough for both IPv4 or IPv6
socklen_t client_addr_len = sizeof(client_addr);
char addr_str[128];

// 연결 상태
static bool client_connected = false;
esp_netif_ip_info_t ip_info;

void print_ap_ip_info(void)
{
    // AP 모드 네트워크 인터페이스 가져오기
    esp_netif_t *esp_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (esp_netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to get AP interface");
        return;
    }

    // IP 정보 가져오기
    esp_netif_get_ip_info(esp_netif, &ip_info);

    // IP 정보 출력
    ESP_LOGI(TAG, "AP IP Address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info.netmask));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
}

// Wi-Fi Event Handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
    {
        ESP_LOGI(TAG, "AP started");
        print_ap_ip_info();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected: " MACSTR, MAC2STR(event->mac));

        // 클라이언트 주소 정보 저장
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        client_connected = true;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station disconnected: " MACSTR, MAC2STR(event->mac));
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        client_connected = false;
    }
}

// Initialize Wi-Fi AP
void wifi_init_softap()
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_create_default_wifi_ap();
    esp_event_handler_instance_t instance_any_id;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    strlcpy((char *)g_ap_config.ap.ssid, SSID, strlen(SSID) + 1);
    strlcpy((char *)g_ap_config.ap.password, PWD, strlen(PWD) + 1);

    g_ap_config.ap.channel = channel;
    wifi_second_chan_t second_chan;

    if (channel <= 7 && bw == 40)
    {
        second_chan = WIFI_SECOND_CHAN_ABOVE;
    }
    else if ((channel > 7 && bw == 40))
    {
        second_chan = WIFI_SECOND_CHAN_BELOW;
    }
    else
    {
        second_chan = WIFI_SECOND_CHAN_NONE;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &g_ap_config));

    if (bw == 40)
    {
        esp_wifi_set_bandwidth(ESP_IF_WIFI_AP, WIFI_BW_HT40); // SoftAP Bandwidth: 40 MHz
    }
    else
    {
        esp_wifi_set_bandwidth(ESP_IF_WIFI_AP, WIFI_BW_HT20); // SoftAP Bandwidth: 20 MHz
    }

    // Start the Wifi modules
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(power));
    esp_wifi_get_max_tx_power(&power);
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, second_chan));
    esp_wifi_get_channel(&channel, &second_chan);

    ESP_LOGI(TAG_AP, "Starting SoftAP, SSID - %s, Password - %s, Primary Channel - %d, Bandwidth - %dMHz",
             SSID, PWD, channel, bw);
}

// UDP Server Task
void udp_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    char tx_buffer[UDP_BUFFER_SIZE];
    char rx_buffer[UDP_BUFFER_SIZE];

    // UDP 소켓 생성
    udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_socket < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    // UDP 포트 바인딩
    if (bind(udp_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(udp_socket);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket bound, port, %d", UDP_PORT);

    // 클라이언트로부터 연결을 위한 데이터 수신

    int len = -1;
    while (1)
    {
        if (xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY))
        {
            if (len < 0)
            {
                ESP_LOGI(TAG, "Waiting for data");
                while (len < 0)
                {
                    len = recvfrom(udp_socket, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
                    // Error occurred during receiving
                    ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                }
                // Data received
                // Get the sender's ip address as string
                inet_ntoa_r(((struct sockaddr_in *)&client_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);

                // 클라이언트로부터 수신한 데이터 출력
                rx_buffer[len] = 0; // NULL-terminate the received data
                ESP_LOGI(TAG, "Received %d bytes from %s: %s", len, addr_str, rx_buffer);
                ESP_LOGI(TAG, "%s", rx_buffer);
            }
            else
            {
                
                snprintf(tx_buffer, sizeof(tx_buffer), 
                         "IP: %d.%d.%d.%d, Port: %d, Channel: %d, Bandwidth: %dMHz, Power: %d", 
                         IP2STR(&ip_info.ip), UDP_PORT, channel, bw, power);

                // 데이터 전송
                int err = sendto(udp_socket, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&client_addr, client_addr_len);
                if (err < 0)
                {
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    len = -1;
                }
                else
                {
                    ESP_LOGI(TAG, "Sent data to client: %s", tx_buffer);

                    rx_buffer[len] = 0;
                    len = recvfrom(udp_socket, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
                }
            }
        }
        // 전송 주기 대기
        vTaskDelay(DATA_SEND_INTERVAL_MS / portTICK_PERIOD_MS);
    }
    if (udp_socket != -1)
    {
        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        close(udp_socket);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_softap();

    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
}
