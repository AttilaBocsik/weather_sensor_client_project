/*
Description: Barometric HTTP Web API client. Sending sensor data to a web server every 20 minutes.
Create: 16/01/2023
User: Attila Bocsik
email: a.bocsik@gmail.com
version: v1.01

Hardware:
 - ESP32s MCU
 - BMP180
LED 01 pin:
 - ESP32 pin GPIO5   <--> LED 01 (+)
 - ESP32 pin 13(GND) <--> LED 01 (-)

BMP180:
 - ESP32 pin 3V3    <--> BMP180 VIN
 - ESP32 pin GDD    <--> BMP180 GND
 - ESP32 pin GPIO22 <--> BMP180 SCL
 - ESP32 pin GPIO21 <--> BMP180 SDA
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_types.h"
#include "esp_err.h"
#include "esp_tls.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "bmp180.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "sdkconfig.h"

/* Compensate altitude measurement
   using current reference pressure, preferably at the sea level,
   obtained from weather station on internet
   Assume normal air pressure at sea level of 101325 Pa
   in case weather station is not available.
 */
#define REFERENCE_PRESSURE 101325.0
#define LED_01 CONFIG_LED_01_OUTPUT_PIN

/* I2C Interface Pins*/
#define I2C_PIN_SDA CONFIG_I2C_PIN_SDA
#define I2C_PIN_SCL CONFIG_I2C_PIN_SCL
/* Stacksize used for Tasks*/
#define STACK_SIZE 2000

/*MIN,MAX values for sensors*/
#define MIN_TEMP 4
#define MAX_TEMP 45
#define MIN_HUM 20
#define MAX_HUM 90
#define MIN_PRESSURE 40000
#define MAX_PRESSURE 110000
#define MIN_ALTITUDE 0
#define MAX_ALTITUDE 6000

unsigned int cycleCounter = 0;

/* WIFI */
static const char *WIFI = "[WIFI]";
unsigned int _counter = 0;
bool isConnectedWifi = false;
/* Set the SSID and Password via project configuration, or can set directly here */
#define DEFAULT_SSID CONFIG_EXAMPLE_WIFI_SSID
#define DEFAULT_PWD CONFIG_EXAMPLE_WIFI_PASSWORD

#if CONFIG_EXAMPLE_WIFI_ALL_CHANNEL_SCAN
#define DEFAULT_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#elif CONFIG_EXAMPLE_WIFI_FAST_SCAN
#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#else
#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#endif /*CONFIG_EXAMPLE_SCAN_METHOD*/

#if CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SIGNAL
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SECURITY
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#else
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#endif /*CONFIG_EXAMPLE_SORT_METHOD*/

#if CONFIG_EXAMPLE_FAST_SCAN_THRESHOLD
#define DEFAULT_RSSI CONFIG_EXAMPLE_FAST_SCAN_MINIMUM_SIGNAL
#if CONFIG_EXAMPLE_FAST_SCAN_WEAKEST_AUTHMODE_OPEN
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#elif CONFIG_EXAMPLE_FAST_SCAN_WEAKEST_AUTHMODE_WEP
#define DEFAULT_AUTHMODE WIFI_AUTH_WEP
#elif CONFIG_EXAMPLE_FAST_SCAN_WEAKEST_AUTHMODE_WPA
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA_PSK
#elif CONFIG_EXAMPLE_FAST_SCAN_WEAKEST_AUTHMODE_WPA2
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA2_PSK
#else
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#endif
#else
#define DEFAULT_RSSI -127
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#endif /*CONFIG_EXAMPLE_FAST_SCAN_THRESHOLD*/

/* int to string method */
char *intToString(const unsigned int input)
{
    if (input < 100)
    {
        char data[4] = {0};
        char *mystring = malloc(4);
        sprintf(data, "0%d", input);
        strcpy(mystring, data);
        // printf("%s\n", mystring);
        return mystring;
    }
    else
    {
        char data[4] = {0};
        char *mystring = malloc(4);
        sprintf(data, "%d", input);
        strcpy(mystring, data);
        // printf("%s\n", mystring);
        return mystring;
    }
}

/* float to string method */
char *floatToString(const float input)
{
    if (input < 100)
    {
        char data[7] = {0};
        char *mystring = malloc(7);
        sprintf(data, "00%0.2f", input);
        strcpy(mystring, data);
        // printf("%s\n", mystring);
        return mystring;
    }
    else
    {
        char data[7] = {0};
        char *mystring = malloc(7);
        sprintf(data, "%0.2f", input);
        strcpy(mystring, data);
        // printf("%s\n", mystring);
        return mystring;
    }
}

/* HTTP */
static const char *HTTP = "[HTTP_CLIENT]";
const int RX_BUF_SIZE = 1024;
#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048
typedef struct
{
    char *uuid;
    float latitude;
    float longitude;
    char *value;
    char *denomination;
    char *message;
    char *error;
} telegraph_t;

#define TIME_ZONE (+2)   // Budapest Time
#define YEAR_BASE (2000) // date in GPS starts from 2000
#define systime_ms() (xTaskGetTickCount() * portTICK_PERIOD_MS)
#define delay_ms(t) vTaskDelay((t) / portTICK_PERIOD_MS)
#define TIME_MARKER "[%8u] "
#define CREATE_SENSOR_URL "<https://>"

/* HTTP */
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer; // Buffer to store response of http request from event handler
    static int output_len;      // Stores number of bytes read
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(HTTP, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(HTTP, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(HTTP, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(HTTP, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(HTTP, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        /*
         *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
         *  However, event handler can also be used in case chunked encoding is used.
         */
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            // If user_data buffer is configured, copy the response into the buffer
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            else
            {
                if (output_buffer == NULL)
                {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(HTTP, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(HTTP, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
            // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(HTTP, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            if (output_buffer != NULL)
            {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            ESP_LOGI(HTTP, "Last esp error code: 0x%x", err);
            ESP_LOGI(HTTP, "Last mbedtls failure: 0x%x", mbedtls_err);
        }

        break;
    }
    return ESP_OK;
}

static void http_async(char *input)
{
    char data[21] = {0};
    char *dataStr = malloc(21);
    sprintf(data, "%s", input);
    strcpy(dataStr, data);
    printf("input %s\n", dataStr);

    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    if (buffer == NULL)
    {
        ESP_LOGE(HTTP, "Cannot malloc http receive buffer");
        return;
    }

    esp_http_client_config_t config = {
        .url = CREATE_SENSOR_URL,
        .event_handler = _http_event_handler,
        .username = "attis71",
        .password = "Za1957",
        .auth_type = HTTP_AUTH_TYPE_BASIC};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;

    char telegraph[200] = {0};
    stpcpy(telegraph, "uuid=abcd");
    strcat(telegraph, "&latitude=46.0");
    strcat(telegraph, "&longitude=20.0");
    strcat(telegraph, "&value=");
    strcat(telegraph, dataStr);
    strcat(telegraph, "&denomination=Barometric sensor");
    strcat(telegraph, "&createDate=not");
    strcat(telegraph, "&message=Home data");
    strcat(telegraph, "&error=No");
    // const char *post_data = telegraph;
    char *post_data = malloc(200);
    stpcpy(post_data, telegraph);
    printf("post_data: %s\n", post_data);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    // esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    while (1)
    {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN)
        {
            break;
        }
    }
    int content_length = esp_http_client_fetch_headers(client);
    int total_read_len = 0, read_len;
    if (total_read_len < content_length && content_length <= MAX_HTTP_RECV_BUFFER)
    {
        read_len = esp_http_client_read(client, buffer, content_length);
        if (read_len <= 0)
        {
            ESP_LOGE(HTTP, "Error read data");
        }
        buffer[read_len] = 0;
        ESP_LOGD(HTTP, "read_len = %d", read_len);
    }
    ESP_LOGI(HTTP, "HTTP Stream reader Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));

    printf("%s", buffer);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    free(buffer);
    free(post_data);
    free(dataStr);
    /*
    if (err == ESP_OK) {
        ESP_LOGI(HTTP, "HTTP Status = %d, content length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(HTTP, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    */
}

/* WIFI */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        ESP_LOGI(WIFI, "WIFI_EVENT_STA_START");
        isConnectedWifi = false;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        ESP_LOGI(WIFI, "WIFI_EVENT_STA_DISCONNECTED");
        isConnectedWifi = false;
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(WIFI, "Connection WI-FI");
        isConnectedWifi = true;
    }
}

/* Initialize Wi-Fi as sta and set scan method */
static void wifi_scan(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    // Initialize default station as network interface instance (esp-netif)
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // Initialize and start WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_SSID,
            .password = DEFAULT_PWD,
            .scan_method = DEFAULT_SCAN_METHOD,
            .sort_method = DEFAULT_SORT_METHOD,
            .threshold.rssi = DEFAULT_RSSI,
            .threshold.authmode = DEFAULT_AUTHMODE,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static struct
{
    uint32_t humidity;
    float temperature;
    uint32_t altitude;
    uint32_t pressure;
} sensor;

static const char *TAG = "weather";
static SemaphoreHandle_t ctrl_sem1;

/*Install I2C Bus driver*/
esp_err_t i2c_master_init(int pin_sda, int pin_scl)
{
    esp_err_t err;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = pin_sda,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = pin_scl,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000};

    err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C driver configuration failed with error = %d", err);
        return ESP_FAIL;
    }
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C driver installation failed with error = %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "I2C master driver has been installed.");
    return ESP_OK;
}

/* WIFI */

/* Read sensor data*/
static void task_read_sensors(void)
{
    gpio_set_level(LED_01, 0);
    delay_ms(1000);
    gpio_set_level(LED_01, 1);
    delay_ms(500);
    gpio_set_level(LED_01, 0);
    delay_ms(500);
    gpio_set_level(LED_01, 1);
    delay_ms(500);
    gpio_set_level(LED_01, 0);
    delay_ms(1000);
    while (1)
    {
        // delay_ms(60000 * 20);
        cycleCounter++;
        delay_ms(700);
#if (CONFIG_USE_BMP180)
        gpio_set_level(LED_01, 0);
        delay_ms(100);
        gpio_set_level(LED_01, 1);
        delay_ms(200);
        gpio_set_level(LED_01, 0);
        if (cycleCounter == (60 * 15))
        {
            unsigned long bmp_pressure;
            u_int32_t pressure_calculate = 0;
            float bmp_altitude;
            float bmp_temperature;
            esp_err_t err = bmp180_read_pressure(&bmp_pressure);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error reading BMP180, err = %d", err);
            }
            else
            {
                if ((bmp_pressure > MIN_PRESSURE) && (bmp_pressure < MAX_PRESSURE))
                {
                    pressure_calculate = bmp_pressure / 100;
                    ESP_LOGI(TAG, "BMP180 legnyomas: %d hPa", pressure_calculate);
                }
                else
                {
                    ESP_LOGE(TAG, "Error reading BMP180, pressure out of range");
                }
            }
            err = bmp180_read_altitude(REFERENCE_PRESSURE, &bmp_altitude);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error reading BMP180, err = %d", err);
            }
            else
            {
                if ((bmp_altitude > MIN_ALTITUDE) && (bmp_altitude < MAX_ALTITUDE))
                {
                    ESP_LOGI(TAG, "BMP180 magassag: %.2f meter", bmp_altitude);
                }
                else
                {
                    ESP_LOGE(TAG, "Error reading BMP180, altitude out of range");
                }
            }
            err = bmp180_read_temperature(&bmp_temperature);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error reading BMP180, err = %d", err);
            }
            else
            {
                if ((bmp_temperature > MIN_TEMP) && bmp_temperature < MAX_TEMP)
                {
                    ESP_LOGI(TAG, "BMP180 homerseklet: %.2f °C", bmp_temperature);
                }
                else
                {
                    ESP_LOGE(TAG, "Error reading BMP180, temperature out of range");
                }
            }

            char telegraph[21] = {0};
            strcat(telegraph, floatToString(bmp_temperature));
            strcat(telegraph, "_");
            strcat(telegraph, intToString(pressure_calculate));
            strcat(telegraph, "_");
            strcat(telegraph, floatToString(bmp_altitude));
            ESP_LOGI("[TELEGRAPH]", "%s\n", telegraph);
            char *post_data = malloc(21);
            stpcpy(post_data, telegraph);
            if (isConnectedWifi)
            {
                gpio_set_level(LED_01, 1);
                http_async(post_data);
                delay_ms(500);
                gpio_set_level(LED_01, 0);
            }
            cycleCounter = 0;
        }
#endif
        printf("Cycle counter value: %d\n", cycleCounter);
    }
}

void app_main(void)
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    /* Initialize Wi-Fi as sta and set scan method */
    wifi_scan();
    /* Initialize GPIO */
    gpio_reset_pin(LED_01);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(LED_01, GPIO_MODE_OUTPUT);
    ESP_ERROR_CHECK(i2c_master_init(I2C_PIN_SDA, I2C_PIN_SCL));
#if (CONFIG_USE_BMP180)
    // Init BMP180 Sensor
    ESP_ERROR_CHECK(bmp180_init());
#endif
    task_read_sensors();
    // xTaskCreatePinnedToCore(task_read_sensors, "readSensors", STACK_SIZE, (void *)1, uxTaskPriorityGet(NULL), NULL, 1);
}
