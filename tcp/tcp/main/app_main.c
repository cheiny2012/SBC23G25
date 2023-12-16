/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "cJSON.h"

#include <driver/i2c.h>
#include <driver/adc.h>
#include <driver/uart.h>
#include "driver/gpio.h"
#include <math.h>

#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#define I2C_MASTER_NUM I2C_NUM_0    // I2C port number
#define I2C_MASTER_FREQ_HZ 100000   // Frecuencia de operación I2C

#define SHT85_ADDR 0x44             // Dirección del sensor SHT85

static const char *TAG = "MQTT_EXAMPLE";

/*-----------------------------------------------------------------------------------------*/
/*-------------------------------------SHT85-----------------------------------------------*/
/*-----------------------------------------------------------------------------------------*/
static esp_err_t set_i2c(void) {
    i2c_config_t i2c_config = {
    };
    i2c_config.mode = I2C_MODE_MASTER;
    i2c_config.sda_io_num = 21;
    i2c_config.scl_io_num = 22;
    i2c_config.sda_pullup_en = true;
    i2c_config.scl_pullup_en = true;
    i2c_config.master.clk_speed = 100000;
    i2c_config.clk_flags = 0;

    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_config));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, ESP_INTR_FLAG_LEVEL1));

    return ESP_OK;
}

void read_sht85_data(float *temperature,float *humidity) {
    uint8_t command[] = {0x21, 0x26}; // Comando para iniciar la medición de temperatura y humedad
    uint8_t data[6] = {0};            // Datos leídos desde el sensor (temperatura: 2 bytes, humedad: 2 bytes)

    esp_err_t ret;

    ret = i2c_master_write_to_device(I2C_MASTER_NUM, SHT85_ADDR, command, sizeof(command), true);
    if (ret != ESP_OK) {
        printf("Error al enviar comando al sensor SHT85\n");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50)); // Tiempo para que el sensor realice la medición

    ret = i2c_master_read_from_device(I2C_MASTER_NUM, SHT85_ADDR, data, sizeof(data), true);
    if (ret != ESP_OK) {
        printf("Error al leer datos del sensor SHT85\n");
        return;
    }

    // Cálculo de la temperatura y la humedad desde los datos leídos
    uint16_t raw_temperature = (data[0] << 8) | data[1];
    uint16_t raw_humidity = (data[3] << 8) | data[4];

    *temperature = ((float)raw_temperature / 65535) * 175 - 45;
    *humidity = ((float)raw_humidity / 65535) * 100;
}


/*-----------------------------------------------------------------------------------------*/
/*-------------------------------------C02-------------------------------------------------*/
/*-----------------------------------------------------------------------------------------*/
#define SENSOR_PIN 34 // Pin al que está conectado el sensor (cambia según tu configuración)

void configuracion_adc() {
    // Configuración del ADC
    adc1_config_width(ADC_WIDTH_BIT_10); // 10 bits de resolución
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11); // Canal ADC y atenuación de señal

    // Configuración del puerto serial para imprimir los resultados
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
}

int read_c02_data() {
    // Leer el valor analógico del sensor
    int val = adc1_get_raw(ADC1_CHANNEL_6);

    return val;
    // Esperar un tiempo antes de tomar otra lectura (ajusta según sea necesario)
    vTaskDelay(pdMS_TO_TICKS(1000));
}

/*-----------------------------------------------------------------------------------------*/
/*-------------------------------------BOTON-----------------------------------------------*/
/*-----------------------------------------------------------------------------------------*/
#define INPUT_GPIO GPIO_NUM_4

int monitorizar_caida() {
    esp_rom_gpio_pad_select_gpio(INPUT_GPIO);
    gpio_set_direction(INPUT_GPIO, GPIO_MODE_INPUT);
    int caida = 0;
    
    int signal = gpio_get_level(INPUT_GPIO);
    
    if (signal){
        caida = 1;
    }
    return caida;
    vTaskDelay(pdMS_TO_TICKS(100));
}

/*-----------------------------------------------------------------------------------------*/
/*-------------------------------------SLEEP-----------------------------------------------*/
/*-----------------------------------------------------------------------------------------*/
bool should_enter_deep_sleep = false;
void check_gpio_signal() {
    int signal = gpio_get_level(12);
    if (signal == 1) {
        should_enter_deep_sleep = true;
    }
}
void enter_deep_sleep() {
    check_gpio_signal();
    if (should_enter_deep_sleep) {
        const int ext_wakeup_pin_0 = 12;

        printf("Enabling EXT0 wakeup on pin GPIO%d\n", ext_wakeup_pin_0);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)ext_wakeup_pin_0, 1);

        rtc_gpio_pullup_dis(ext_wakeup_pin_0);
        rtc_gpio_pulldown_en(ext_wakeup_pin_0);

        printf("Entering deep sleep\n");
        esp_deep_sleep_start();

        // Poner la variable a false después de entrar en el sueño profundo
        should_enter_deep_sleep = false;
    }
    
}

/*-----------------------------------------------------------------------------------------*/
/*-------------------------------------THINGSBOARD-----------------------------------------*/
/*-----------------------------------------------------------------------------------------*/

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_send_data(void) {
    // Configuración para el botón de caída
    esp_mqtt_client_config_t mqtt_cfg_boton = {
        .broker.address.uri = "mqtt://demo.thingsboard.io", 
        .broker.address.port = 1883,
        .credentials.username = "Epy0KLB7j9w6CwGeK0I9", // token del dispositivo ej. dispositivos->LDR->Copiar access token
    };
    esp_mqtt_client_handle_t client_boton = esp_mqtt_client_init(&mqtt_cfg_boton);
    /* ... configuración del cliente MQTT ... */

    // Configuración para el sensor de CO2
    esp_mqtt_client_config_t mqtt_cfg_c02 = {
        .broker.address.uri = "mqtt://demo.thingsboard.io", 
        .broker.address.port = 1883,
        .credentials.username = "6EclzE0PIMolnhG7iLB0", // token del dispositivo ej. dispositivos->LDR->Copiar access token
    };
    esp_mqtt_client_handle_t client_c02 = esp_mqtt_client_init(&mqtt_cfg_c02);
    /* ... configuración del cliente MQTT ... */

    // Configuración para el sensor de temperatura SHT85
   esp_mqtt_client_config_t mqtt_cfg_sht85_temp = {
        .broker.address.uri = "mqtt://demo.thingsboard.io", 
        .broker.address.port = 1883,
        .credentials.username = "yDzUOZDWIkrKgogdoDQZ", // token del dispositivo ej. dispositivos->LDR->Copiar access token
    };
    esp_mqtt_client_handle_t client_sht85_temp = esp_mqtt_client_init(&mqtt_cfg_sht85_temp);
    /* ... configuración del cliente MQTT ... */

    // Configuración para el sensor de humedad SHT85
     esp_mqtt_client_config_t mqtt_cfg_sht85_hum = {
        .broker.address.uri = "mqtt://demo.thingsboard.io", 
        .broker.address.port = 1883,
        .credentials.username = "c0J4oSc211eBii3mukr5", // token del dispositivo ej. dispositivos->LDR->Copiar access token
    };
    esp_mqtt_client_handle_t client_sht85_hum = esp_mqtt_client_init(&mqtt_cfg_sht85_hum);
    /* ... configuración del cliente MQTT ... */

    esp_mqtt_client_register_event(client_boton, ESP_EVENT_ANY_ID, mqtt_event_handler, client_boton);
    esp_mqtt_client_start(client_boton);

    esp_mqtt_client_register_event(client_c02, ESP_EVENT_ANY_ID, mqtt_event_handler, client_c02);
    esp_mqtt_client_start(client_c02);

    esp_mqtt_client_register_event(client_sht85_temp, ESP_EVENT_ANY_ID, mqtt_event_handler, client_sht85_temp);
    esp_mqtt_client_start(client_sht85_temp);

    esp_mqtt_client_register_event(client_sht85_hum, ESP_EVENT_ANY_ID, mqtt_event_handler, client_sht85_hum);
    esp_mqtt_client_start(client_sht85_hum);
    ESP_ERROR_CHECK(set_i2c());

    while (1) {
    enter_deep_sleep();
    cJSON *json_data_boton = cJSON_CreateObject();
    cJSON *json_data_c02 = cJSON_CreateObject();
    cJSON *json_data_sht85_temp = cJSON_CreateObject();
    cJSON *json_data_sht85_hum = cJSON_CreateObject();

    // Leer los valores de los sensores
    int button_value = monitorizar_caida();
    int c02_value = read_c02_data();
    float temperature, humidity;
    read_sht85_data(&temperature, &humidity);

    // Mostrar los valores por pantalla
    printf("BOTON: %d\nCO2: %d\nTEMPERATURA: %.2f\nHUMEDAD: %.2f\n\n", button_value, c02_value, temperature, humidity);

    // Agregar los valores al JSON
    cJSON_AddNumberToObject(json_data_boton, "BotonCaida", button_value);
    cJSON_AddNumberToObject(json_data_c02, "CalidadAire", c02_value);
    cJSON_AddNumberToObject(json_data_sht85_temp, "Temperatura", temperature);
    cJSON_AddNumberToObject(json_data_sht85_hum, "Humedad", humidity);

    char *post_data_boton = cJSON_PrintUnformatted(json_data_boton);
    esp_mqtt_client_publish(client_boton, "v1/devices/me/telemetry", post_data_boton, 0, 1, 0);

    char *post_data_c02 = cJSON_PrintUnformatted(json_data_c02);
    esp_mqtt_client_publish(client_c02, "v1/devices/me/telemetry", post_data_c02, 0, 1, 0);

    char *post_data_sht85_temp = cJSON_PrintUnformatted(json_data_sht85_temp);
    esp_mqtt_client_publish(client_sht85_temp, "v1/devices/me/telemetry", post_data_sht85_temp, 0, 1, 0);

    char *post_data_sht85_hum = cJSON_PrintUnformatted(json_data_sht85_hum);
    esp_mqtt_client_publish(client_sht85_hum, "v1/devices/me/telemetry", post_data_sht85_hum, 0, 1, 0);

    // Liberar memoria
    cJSON_Delete(json_data_boton);
    cJSON_Delete(json_data_c02);
    cJSON_Delete(json_data_sht85_temp);
    cJSON_Delete(json_data_sht85_hum);

    free(post_data_boton);
    free(post_data_c02);
    free(post_data_sht85_temp);
    free(post_data_sht85_hum);

    // Retraso antes de la próxima lectura y envío
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
}

/*-----------------------------------------------------------------------------------------*/
/*-------------------------------------MAIN------------------------------------------------*/
/*-----------------------------------------------------------------------------------------*/
void app_main(void)
{
    

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    
    mqtt_send_data();
}
