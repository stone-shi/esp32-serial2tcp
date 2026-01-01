/* Serial to TCP Bridge Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include <fcntl.h>
#include <errno.h>
#include "led_strip.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Built-in LED pin (override by defining BLINK_GPIO before including this file) */
#ifndef BLINK_GPIO
#define BLINK_GPIO 48
#endif

static const char *TAG = "serial_tcp";

static int s_retry_num = 0;
static led_strip_handle_t led_strip;
static uint8_t s_led_state = 0;
static volatile TickType_t s_last_activity_time = 0;
static volatile int s_is_client_connected = 0;

static portMUX_TYPE stats_spinlock = portMUX_INITIALIZER_UNLOCKED;

static volatile uint64_t s_total_bytes_bridged = 0;
static volatile uint32_t s_byte_accumulator = 0;
static uint32_t s_bytes_history[60] = {0};
static int s_history_idx = 0;


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void blink_led(void)
{
    /* If the addressable LED is enabled */
    if (s_led_state) {
        /* Set the LED pixel using RGB from 0 (0%) to 255 (100%) for each color */
        led_strip_set_pixel(led_strip, 0, 16, 16, 16);
        /* Refresh the strip to send data */
        led_strip_refresh(led_strip);
    } else {
        /* Set all LED off to clear all pixels */
        led_strip_clear(led_strip);
    }
}

static void configure_led(void)
{
    ESP_LOGI(TAG, "Example configured to blink addressable LED!");
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1, // at least one LED on board
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "led_task started on GPIO %d", BLINK_GPIO);

    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "s_wifi_event_group is NULL, LED task exiting");
        vTaskDelete(NULL);
        return;
    }

    configure_led();

    while (1) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        /* If connected, keep LED ON and park the task */
        if (bits & WIFI_CONNECTED_BIT) {
            portENTER_CRITICAL(&stats_spinlock);
            TickType_t last_activity = s_last_activity_time;
            portEXIT_CRITICAL(&stats_spinlock);
            if (xTaskGetTickCount() - last_activity < pdMS_TO_TICKS(200)) {
                s_led_state = !s_led_state;
                blink_led();
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                s_led_state = 1;
                blink_led();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else if (bits & WIFI_FAIL_BIT) {
            /* On fail, blink LED red */
            ESP_LOGI(TAG, "WiFi failed, blinking LED red");
            while (1) {
                led_strip_set_pixel(led_strip, 0, 16, 0, 0);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
                led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            s_led_state = !s_led_state;
            blink_led();
            ESP_LOGD(TAG, "LED toggle -> %d", s_led_state);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* Start LED task: blink while connecting, solid ON when connected */
    xTaskCreate(led_task, "led", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

/* --- UART to TCP forwarder --- */

#define UART_PORT_NUM      UART_NUM_1
#define UART_RX_PIN        18
#define UART_TX_PIN        17
#define UART_BAUD_RATE     9600
#define BUF_SIZE           1024
#define TCP_PORT           8989
#define LISTEN_BACKLOG     1
#define STATUS_TCP_PORT    9090
#define HTTP_STATUS_PORT   9091

static void uart_init_forward(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
}

static void tcp_client_handler(void *arg)
{
    int client_sock = (int)(intptr_t)arg;
    uint8_t* data = (uint8_t*)malloc(BUF_SIZE);
    if (!data) {
        ESP_LOGE(TAG, "malloc failed");
        portENTER_CRITICAL(&stats_spinlock);
        s_is_client_connected = 0;
        portEXIT_CRITICAL(&stats_spinlock);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, data, BUF_SIZE, pdMS_TO_TICKS(20));
        if (len > 0) {
            portENTER_CRITICAL(&stats_spinlock);
            s_total_bytes_bridged += len;
            s_byte_accumulator += len;
            s_last_activity_time = xTaskGetTickCount();
            portEXIT_CRITICAL(&stats_spinlock);
            int to_send = len;
            int sent = 0;
            while (to_send > 0) {
                int w = send(client_sock, (const char*)data + sent, to_send, 0);
                if (w < 0) {
                    ESP_LOGE(TAG, "socket send failed: errno %d", errno);
                    goto cleanup;
                }
                sent += w;
                to_send -= w;
            }
        }
        int r = recv(client_sock, (char*)data, BUF_SIZE, MSG_DONTWAIT);
        if (r > 0) {
            portENTER_CRITICAL(&stats_spinlock);
            s_total_bytes_bridged += r;
            s_byte_accumulator += r;
            s_last_activity_time = xTaskGetTickCount();
            portEXIT_CRITICAL(&stats_spinlock);
            int written = uart_write_bytes(UART_PORT_NUM, (const char*)data, r);
            if (written < 0) {
                ESP_LOGE(TAG, "uart write failed");
                goto cleanup;
            }
        } else if (r == 0) {
            ESP_LOGI(TAG, "Client disconnected");
            break;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "recv error: errno %d", errno);
                break;
            }
        }
    }
cleanup:
    portENTER_CRITICAL(&stats_spinlock);
    s_is_client_connected = 0;
    portEXIT_CRITICAL(&stats_spinlock);
    close(client_sock);
    free(data);
    vTaskDelete(NULL);
}

static void tcp_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(TCP_PORT);
    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Error during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listening on port %d", TCP_PORT);
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int client_sock = accept(listen_sock, (struct sockaddr*)&source_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "Accepted client");

        /* Enable TCP Keepalive to detect silent disconnects (e.g. power loss) */
        int keepAlive = 1;
        int keepIdle = 5;    // Send probe if idle for 5s
        int keepInterval = 5; // Send probe every 5s
        int keepCount = 3;    // Drop after 3 failed probes
        setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        portENTER_CRITICAL(&stats_spinlock);
        if (s_is_client_connected) {
            portEXIT_CRITICAL(&stats_spinlock);
            ESP_LOGW(TAG, "Client already connected, rejecting new connection");
            close(client_sock);
        } else {
            s_is_client_connected = 1;
            portEXIT_CRITICAL(&stats_spinlock);
            if (xTaskCreate(tcp_client_handler, "tcp_client", 4096, (void*)(intptr_t)client_sock, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to create tcp_client task");
                portENTER_CRITICAL(&stats_spinlock);
                s_is_client_connected = 0;
                portEXIT_CRITICAL(&stats_spinlock);
                close(client_sock);
            }
        }
    }
    close(listen_sock);
    vTaskDelete(NULL);
}

static void stats_task(void *arg)
{
    uint32_t last_acc = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        portENTER_CRITICAL(&stats_spinlock);
        uint32_t now = s_byte_accumulator;
        portEXIT_CRITICAL(&stats_spinlock);
        uint32_t delta = now - last_acc;
        last_acc = now;
        s_bytes_history[s_history_idx] = delta;
        s_history_idx = (s_history_idx + 1) % 60;
    }
}

static void status_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Status socket create failed");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(STATUS_TCP_PORT);

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Status socket bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Status socket listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Status server listening on port %d", STATUS_TCP_PORT);

    while(1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int client_sock = accept(listen_sock, (struct sockaddr*)&source_addr, &addr_len);
        if (client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        wifi_ap_record_t ap_info;
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_default_netif();
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
        } else {
            memset(&ip_info, 0, sizeof(ip_info));
        }

        uint32_t bytes_last_min = 0;
        for (int i=0; i<60; i++) {
            bytes_last_min += s_bytes_history[i];
        }

        portENTER_CRITICAL(&stats_spinlock);
        uint64_t total_bytes_bridged = s_total_bytes_bridged;
        int is_client_connected = s_is_client_connected;
        portEXIT_CRITICAL(&stats_spinlock);

        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "\r\n--- Status Monitor ---\r\n"
            "WiFi Signal: %d dBm\r\n"
            "IP Address: " IPSTR "\r\n"
            "Client Connected: %s\r\n"
            "Total Bytes Bridged: %llu\r\n"
            "Bytes (Last 1 min): %lu\r\n"
            "----------------------\r\n",
            rssi,
            IP2STR(&ip_info.ip),
            is_client_connected ? "Yes" : "No",
            total_bytes_bridged,
            (unsigned long)bytes_last_min
        );

        send(client_sock, buf, len, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        close(client_sock);
    }
}

static void http_status_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "HTTP Status socket create failed");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(HTTP_STATUS_PORT);

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "HTTP Status socket bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "HTTP Status socket listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HTTP Status server listening on port %d", HTTP_STATUS_PORT);

    while(1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int client_sock = accept(listen_sock, (struct sockaddr*)&source_addr, &addr_len);
        if (client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Read and discard request headers */
        char rx_buf[256];
        recv(client_sock, rx_buf, sizeof(rx_buf), 0);

        wifi_ap_record_t ap_info;
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_default_netif();
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
        } else {
            memset(&ip_info, 0, sizeof(ip_info));
        }

        uint32_t bytes_last_min = 0;
        for (int i=0; i<60; i++) {
            bytes_last_min += s_bytes_history[i];
        }

        portENTER_CRITICAL(&stats_spinlock);
        uint64_t total_bytes_bridged = s_total_bytes_bridged;
        int is_client_connected = s_is_client_connected;
        portEXIT_CRITICAL(&stats_spinlock);

        char json_buf[256];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"rssi\":%d,\"ip\":\"" IPSTR "\",\"client_connected\":%s,\"total_bytes\":%llu,\"bytes_last_min\":%lu}",
            rssi,
            IP2STR(&ip_info.ip),
            is_client_connected ? "true" : "false",
            total_bytes_bridged,
            (unsigned long)bytes_last_min
        );

        char http_buf[512];
        int http_len = snprintf(http_buf, sizeof(http_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
            "%s",
            json_len,
            json_buf
        );

        send(client_sock, http_buf, http_len, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        close(client_sock);
    }
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
         * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "Starting Serial to TCP Bridge");
    wifi_init_sta();

    /* Initialize UART and start TCP server to forward serial data */
    uart_init_forward();
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    xTaskCreate(stats_task, "stats_task", 2048, NULL, 5, NULL);
    xTaskCreate(status_server_task, "status_server", 4096, NULL, 5, NULL);
    xTaskCreate(http_status_server_task, "http_status", 4096, NULL, 5, NULL);
}