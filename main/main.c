/* HTTP GET Example using plain POSIX sockets

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
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

/* Constants that aren't configurable in menuconfig */
#define WEB_SERVER "wasser.borgsdorf.krasch.io"
#define WEB_PORT 80
#define WEB_URL "http://wasser.borgsdorf.krasch.io/actions/eightport/"
#define DEFAULT_SLEEP 1

static RTC_DATA_ATTR struct timeval sleep_enter_time;

static const char *TAG = "controllerboard";

static const char *REQUEST = "GET " WEB_URL " HTTP/1.0\r\n"
    "Host: "WEB_SERVER"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

static void enter_deep_sleep(uint32_t minutes) {
    const uint64_t wakeup_time_sec = minutes*60;
    ESP_LOGI(TAG, "Enabling timer wakeup, %ds\n", (int)wakeup_time_sec);
    esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000L);
    ESP_LOGI(TAG, "Entering deep sleep\n");
    gettimeofday(&sleep_enter_time, NULL);
    esp_deep_sleep_start();
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static char recv_buf[1024];
#define NUM_PORTS 8
static int ports[NUM_PORTS] = {22, 23, 19, 21, 5, 18, 16, 17};
static void http_get_task(void *pvParameters)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r, t = 0;
    EventBits_t connected;
    /* Wait for the callback to set the CONNECTED_BIT in the
       event group.
    */
    connected = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                                    false, true, (1*60*1000) / portTICK_PERIOD_MS);

    if((connected & CONNECTED_BIT) == 0) {
        ESP_LOGI(TAG, "Failed to Connect to AP");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        enter_deep_sleep(DEFAULT_SLEEP);
    }

    ESP_LOGI(TAG, "Connected to AP");

    int err = getaddrinfo(WEB_SERVER, "80", &hints, &res);

    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        enter_deep_sleep(DEFAULT_SLEEP);
    }

    /* Code to print the resolved IP.

       Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0);
    if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            enter_deep_sleep(DEFAULT_SLEEP);
    }
    ESP_LOGI(TAG, "... allocated socket\r\n");

    if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        enter_deep_sleep(DEFAULT_SLEEP);
    }

    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(res);

    if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            enter_deep_sleep(DEFAULT_SLEEP);
    }
    ESP_LOGI(TAG, "... socket send success");

    bzero(recv_buf, sizeof(recv_buf));

    /* Read HTTP response */
    do {
        r = read(s, recv_buf + t, sizeof(recv_buf) - t - 1);
        t+=r;
    } while(r > 0);

    ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
    close(s);

    recv_buf[t] = '\0';

    ESP_LOGI(TAG, "%s\n", recv_buf);

    char * csv = strstr(recv_buf, "\r\n\r\n") + 4;
    if(csv != NULL) {
        ESP_LOGI(TAG, "%s", csv);

        char * line = csv;

        uint32_t port, minutes;

        while(line != (NULL + 1) && sscanf(line, "%d,%d", &port, &minutes) == 2) {
            if(port == 0) {
                ESP_LOGI(TAG, "Received sleep command. Will sleep for %d minutes\n", minutes);
                enter_deep_sleep(minutes);
            } else {
                gpio_set_level(ports[port - 1], 1);
                ESP_LOGI(TAG, "Open port %d (GPIO %d) for %d minutes\n", port, ports[port - 1], minutes);
                vTaskDelay((minutes*60*1000) / portTICK_PERIOD_MS);
                gpio_set_level(ports[port - 1], 0);
                ESP_LOGI(TAG, "Close port %d (GPIO %d)\n", port, ports[port - 1]);
            }
            line = strstr(line, "\n") + 1;
        }
    }

    ESP_LOGI(TAG, "Invalid response or no sleep command. Will sleep for %d minutes\n", DEFAULT_SLEEP);
    enter_deep_sleep(DEFAULT_SLEEP);
}

void app_main() {

    struct timeval now;
    gettimeofday(&now, NULL);
    int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

    int level = 0;

    for(int i = 0; i < NUM_PORTS; i++) {
        gpio_pad_select_gpio(ports[i]);
        gpio_set_direction(ports[i], GPIO_MODE_OUTPUT);
        gpio_set_level(ports[i], level);
        ESP_LOGI(TAG, "Port %d ready for output %d\n", ports[i], level);
    }

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER: {
            ESP_LOGI(TAG, "Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
            break;
        }
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            ESP_LOGI(TAG, "Power on\n");
            break;
    }

    ESP_ERROR_CHECK( nvs_flash_init() );
    initialise_wifi();
    xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, NULL);
}
