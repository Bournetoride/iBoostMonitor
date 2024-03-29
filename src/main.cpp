#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "main.h"
#include "my_ringbuf.h"
#include "config.h"
#include "CC1101_RFx.h"

// Defines
#define PING_IBOOST_UNIT 10000      // PING_IBOOST_UNIT iBoost main unit for data every 10 seconds

// ESP32 Wroom 32: SCK_PIN = 18; MISO_PIN = 19; MOSI_PIN = 23; SS_PIN = 5; GDO0 = 2;
#define SS_PIN 5
#define MISO_PIN 19

#define MAGIC_NUMBER 380 // value used to conert iBoost value to watts

CC1101 radio(SS_PIN,  MISO_PIN);

// freeRTOS specific variables
TaskHandle_t blink_led_task_handle = NULL;
TaskHandle_t ws2812b_task_handle = NULL;

TaskHandle_t mqqt_keep_alive_task_handle = NULL;
TaskHandle_t receive_packet_task_handle = NULL;
TaskHandle_t transmit_packet_task_handle = NULL;

TaskHandle_t display_task_handle = NULL;

QueueHandle_t inbuilt_led_queue;
QueueHandle_t ws2812b_queue;
QueueHandle_t transmit_queue;
QueueHandle_t g_main_queue;

int queue_size = 10;

SemaphoreHandle_t keep_alive_mqtt_semaphore;
SemaphoreHandle_t radio_semaphore;

RingbufHandle_t buf_handle;

#define MSG_BUFFER_SIZE	(100)
#define PIN_WS2812B 3 // 13 on wroom-32d           // Output pin on ESP32 that controls the addressable LEDs
#define NUM_PIXELS 4            // Number of LEDs (pixels) we can control
#define GLOW 25

// Set up library to control LEDs
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_GRB + NEO_KHZ800);

// codes for the various requests and responses
enum { 
    SAVED_TODAY = 0xCA,
    SAVED_YESTERDAY = 0xCB,
    SAVED_LAST_7 = 0xCC,
    SAVED_LAST_28 = 0xCD,
    SAVED_TOTAL = 0xCE
};

// LEDs that get lit depending on message we want to convey
enum led_measage_t{ 
    TX_FAKE_BUDDY_REQUEST,
    RECEIVE,
    ERROR,
    CLEAR,
    CLEAR_ERROR,
    BLANK
};

typedef struct  {
    long today;
    long yesterday;
    long last7;
    long last28;
    long total;
    uint8_t address[2];
    uint8_t lqi;
    bool b_is_address_valid;
    bool b_sender_battery_ok;
} iboost_information_t;

// LED colours
typedef struct {
    uint32_t red = ws2812b.Color(GLOW*255/255, GLOW*0/255, GLOW*0/255);
    uint32_t green = ws2812b.Color(GLOW*0/255, GLOW*255/255, GLOW*0/255);
    uint32_t blue = ws2812b.Color(GLOW*0/255, GLOW*0/255, GLOW*255/255);
    uint32_t violet = ws2812b.Color(GLOW*246/255, GLOW*0/255, GLOW*255/255);
    uint32_t yellow = ws2812b.Color(GLOW*255/255, GLOW*255/255, GLOW*0/255);
    uint32_t clear = ws2812b.Color(0, 0, 0);
} colours_t;

// Logging tag
static const char* TAG = "MAIN";

WiFiClient wifi_client;
byte mac_address[6];
PubSubClient mqtt_client(MQTT_SERVER, MQTT_PORT, wifi_client);
colours_t pixel_colours;
char weather_description[35];    // buffer for the current weather description from OpenWeatherMap
static portMUX_TYPE myMux = portMUX_INITIALIZER_UNLOCKED;

iboost_information_t volatile iboost_information = {.today = 0, .yesterday = 0, .last7 = 0, .last28 = 0, .total = 0,
                                            .lqi = 255, .b_is_address_valid = false, .b_sender_battery_ok = false};

/* Function prototypes */
void blink_led_task(void *parameter);
void mqtt_keep_alive_task(void *parameter);
void receive_packet_task(void *parameter);
void transmit_packet_task(void *parameter);
void ws2812b_task(void *parameter);
///////
void radio_setup();
void connect_to_wifi(void);
void connect_to_mqtt(void);
String wifi_connection_status_message(wl_status_t wifi_status);
static void mqtt_callback(char* topic, byte* message, unsigned int length);

/**
 * @brief Set up everything. SPI, WiFi, MQTT, and CC1101
 * 
 */
void setup() {
    bool b_setup_successful = true;      // Flag for confirming setup was sucessfull or not
    BaseType_t x_returned;
    UBaseType_t res = pdFALSE;
    char tx_item[50];

    // Initialise WS2812B strip object (REQUIRED)    
    ws2812b.begin();
    ws2812b.show();
    ws2812b.clear();
    for (int pixel = 0; pixel < NUM_PIXELS; pixel++) {          // for each pixel
        ws2812b.setPixelColor(pixel, pixel_colours.violet);      // it only takes effect when pixels.show() is called
    }
    ws2812b.show();  // update to the WS2812B Led Strip

    //Create ring buffer
    buf_handle = xRingbufferCreate(1028, RINGBUF_TYPE_NOSPLIT);
    if (buf_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        b_setup_successful = false;
    }

    // Create queue for sending messages to the display task
    g_main_queue = xQueueCreate(queue_size, sizeof(electricity_event_t));
    if (g_main_queue == NULL) {
        ESP_LOGE(TAG, "Error creating g_main_queue");
        strcpy(tx_item, "Error creating g_main_queue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }

        b_setup_successful = false;
    }

    // Create queue for sending messages to the LED task and transmit packet task
    inbuilt_led_queue = xQueueCreate(queue_size, sizeof(bool));  // internal led
    if (inbuilt_led_queue == NULL) {
        ESP_LOGE(TAG, "Error creating inbuilt_led_queue");
        strcpy(tx_item, "Error creating inbuilt_led_queue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }

        b_setup_successful = false;
    }

    ws2812b_queue = xQueueCreate(queue_size, sizeof(led_measage_t));  // led strip
    if (ws2812b_queue == NULL) {
        ESP_LOGE(TAG, "Error creating ws2812b_queue");
        strcpy(tx_item, "Error creating ws2812b_queue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }

        b_setup_successful = false;
    }

    transmit_queue = xQueueCreate(queue_size, sizeof(bool));
    if (transmit_queue == NULL) {
        ESP_LOGE(TAG, "Error creating transmit_queue");
        strcpy(tx_item, "Error creating transmit_queue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        b_setup_successful = false;
    }

    // Create required semaphores
    keep_alive_mqtt_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(keep_alive_mqtt_semaphore);            // Stop keep alive when publishing

    radio_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(radio_semaphore);

    SPI.begin();
    ESP_LOGI(TAG, "SPI OK");
    strcpy(tx_item, "SPI Ok");
    res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    }

    // Set up the radio
    radio_setup();
    
    /* LED setup - so we can use the module without serial terminal,
       set low to start so it's off and flashes when it receives a packet */
    pinMode(LED_BUILTIN, OUTPUT);   

    // Internal blue LED on ESP32 board
    x_returned = xTaskCreate(blink_led_task, "blink_led_task", 1024, NULL, tskIDLE_PRIORITY, &blink_led_task_handle);
    if (x_returned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create blink_led_task");
        strcpy(tx_item, "Error creating blink_led_task");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        b_setup_successful = false;
    }

    x_returned = xTaskCreate(ws2812b_task, "ws2812b_task", 2048, NULL, tskIDLE_PRIORITY, &ws2812b_task_handle);
    if (x_returned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ws2812b_task");
        strcpy(tx_item, "Error creating ws2812b_task");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        b_setup_successful = false;
    }

    /* This task also creates/checks the WiFi connection */
    x_returned = xTaskCreate(mqtt_keep_alive_task, "mqtt_keep_alive_task", 4096, NULL, 1, &mqqt_keep_alive_task_handle);
    if (x_returned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mqtt_keep_alive_task");
        strcpy(tx_item, "Error creating mqttKeepAliveTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        b_setup_successful = false;
    }

    x_returned = xTaskCreate(receive_packet_task, "receive_packet_task", 4096, NULL, 3, &receive_packet_task_handle);
    if (x_returned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive_packet_task");
        strcpy(tx_item, "Error creating receivePackeTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        b_setup_successful = false;
    }

    x_returned = xTaskCreate(transmit_packet_task, "transmit_packet_task", 4096, NULL, 3, &transmit_packet_task_handle);
    if (x_returned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create transmit_packet_task");
        strcpy(tx_item, "Error creating transmit_packet_task");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        b_setup_successful = false;
    }

    // Actioned in screen.cpp
    x_returned = xTaskCreate(display_task, "display_task", 3072, NULL, tskIDLE_PRIORITY, &display_task_handle);
    if (x_returned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display_task, setup failed");
        // No point send a log item as there isn't any display task to action it!!
        b_setup_successful = false;
    } 

    // Everything set up okay so turn (blue) LED off
    if (b_setup_successful) {
        digitalWrite(LED_BUILTIN, LOW);

        ws2812b.clear();
        ws2812b.show();

        radio.setRXstate();             // Set the current state to RX : listening for RF packets
    } else {
        ESP_LOGE(TAG, "Setup Failed!!!");
        strcpy(tx_item, "Setup Failed!!!");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
    }
}

/**
 * @brief Main loop
 * 
 */
void loop(void) {

    // All happens in tasks

}

/**
 * @brief Blink the ESP32 LED for 200ms when a flag is set.
 * 
 * @param parameter Parameters passed to task on creation.
 */
void blink_led_task(void *parameter) {
    bool b_flag = false;

    for( ;; ) {
        xQueueReceive(inbuilt_led_queue, &b_flag, (TickType_t)portMAX_DELAY);
        if (b_flag) {
            digitalWrite(LED_BUILTIN, HIGH);        // Turn LED on
            vTaskDelay(200 / portTICK_PERIOD_MS);   // Wait 200ms
            digitalWrite(LED_BUILTIN, LOW);         // Turn LED off
            b_flag = false;                           // Reset flag
        }
    }
    vTaskDelete (NULL);
}


/**
 * @brief Blink an LED on the LED strip dependiing on request.
 * 
 * @param parameter Parameters passed to task on creation.
 */
void ws2812b_task(void *parameter) {
    led_measage_t led = BLANK;

    for( ;; ) {
        xQueueReceive(ws2812b_queue, &led, (TickType_t)portMAX_DELAY);
        switch (led) {
            case TX_FAKE_BUDDY_REQUEST:
                ws2812b.setPixelColor(0, pixel_colours.green);
                ws2812b.show();
                vTaskDelay(200 / portTICK_PERIOD_MS);
                ws2812b.setPixelColor(0, pixel_colours.clear);
                ws2812b.show();
                break;
            case RECEIVE:
                ws2812b.setPixelColor(1, pixel_colours.blue);
                ws2812b.show();
                vTaskDelay(200 / portTICK_PERIOD_MS);
                ws2812b.setPixelColor(1, pixel_colours.clear);
                ws2812b.show();
                break;
            case ERROR:
                ws2812b.setPixelColor(2, pixel_colours.red);
                ws2812b.setPixelColor(3, pixel_colours.red);
                ws2812b.show();
                break;
            case CLEAR_ERROR:
                ws2812b.setPixelColor(2, pixel_colours.clear);
                ws2812b.setPixelColor(3, pixel_colours.clear);
                ws2812b.show();
                break;
            case CLEAR:
                ws2812b.clear();
                ws2812b.show();
                break;
        }
        led = BLANK;
    }
    vTaskDelete(NULL);
}


/**
 * @brief Task to keep connection to MQTT broker alive.  Makes the initial
 * connection to the MQTT broker and keeps the connection open along with 
 * the WiFi connection.
 * 
 * Data is sent to the MQTT broker on a Raspberry Pi so that it can be saved to a 
 * database (InfluxDb) for viewing in Grafana and the state of the water (HOT or not) 
 * will be sent to an external web page so can it can be viewed to see if we need to 
 * put the hot water on or not.
 * 
 * @param parameter Parameters passed to task on creation.
 */
void mqtt_keep_alive_task(void *parameter) {
    led_measage_t led = BLANK;

    // setting must be set before a mqtt connection is made
    mqtt_client.setKeepAlive( 90 ); // setting keep alive to 90 seconds makes for a very reliable connection.
    mqtt_client.setBufferSize( 256 );
    mqtt_client.setSocketTimeout( 15 );
    for( ;; ) {
        //check for a is-connected and if the WiFi 'thinks' its connected, found checking on both is more realible than just a single check
        if ((mqtt_client.connected()) && (WiFi.status() == WL_CONNECTED))
        {   // while MQTTlient.loop() is running no other mqtt operations should be in process
            xSemaphoreTake(keep_alive_mqtt_semaphore, portMAX_DELAY); 
            mqtt_client.loop();
            xSemaphoreGive(keep_alive_mqtt_semaphore);
        } else {       
            led = ERROR;
            xQueueSend(ws2812b_queue, &led, 0);
            wl_status_t status = WiFi.status();
            ESP_LOGI(TAG, "MQTT keep alive: MQTT %s,  WiFi %s", 
                    mqtt_client.connected() ? "Connected" : "Not Connected", 
                    wifi_connection_status_message(status)
            );
            if (!(status == WL_CONNECTED)) {
                connect_to_wifi();
            }

            connect_to_mqtt();

            configTime(0, 0, SNTP_TIME_SERVER);  // connect to ntp time server
            update_local_time();                      // update the local time
    
            led = CLEAR_ERROR;
            char tx_item[] = "Set up complete";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAG, "Failed to send Ringbuffer item");
            } else {
                ESP_LOGI(TAG, "Item sent to Ringbuffer");
            }

            xQueueSend(ws2812b_queue, &led, 0);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS); //task runs approx every 500 mS

        // ESP_LOGI(TAG, "## MQTT Task Stack Left: %d", uxTaskGetStackHighWaterMark(NULL));
    }
    vTaskDelete (NULL);
}


/**
 * @brief Handle the receipt of a packet from the CC1101
 * 
 */
void receive_packet_task(void *parameter) { 
    byte packet[65];                // The CC1101 library sets the biggest packet at 61
    uint8_t address_lqi = 255;      // set received LQI to lowest value
    uint8_t receive_lqi = 0;        // signal strength test 
    JsonDocument doc;               // Create JSON message for sending via MQTT
    char msg[MSG_BUFFER_SIZE];      // MQTT message
    led_measage_t led = RECEIVE;
    bool b_flag = true;
    electricity_event_t electricity_event;

    for( ;; ) {
        xSemaphoreTake(radio_semaphore, portMAX_DELAY);
        byte pkt_size = radio.getPacket(packet);
        if (pkt_size > 0 && radio.crcok()) {        // We have a valid packet with some data
            short heating;
            long p1, p2;
            byte boostTime;
            bool b_is_water_heating_by_solar, b_is_cylinder_hot, b_is_battery_ok;
            int16_t rssi = radio.getRSSIdbm();

            //   buddy request                            sender packet
            if ((packet[2] == 0x21 && pkt_size == 29) || (packet[2] == 0x01 && pkt_size == 44)) {
                receive_lqi = radio.getLQI();
                ESP_LOGI(TAG, "Buddy/Sender frame received: length=%d, RSSI=%d, LQI=%d", pkt_size, rssi, receive_lqi);

                if(receive_lqi < address_lqi) { // is the signal stronger than the previous/none
                    address_lqi = receive_lqi;
                    iboost_information.address[0] = packet[0]; // save the address of the packet	0x1c7b; //
                    iboost_information.address[1] = packet[1];
                    iboost_information.b_is_address_valid = true;

                    ESP_LOGI(TAG, "Updated iBoost address to: %02x,%02x", iboost_information.address[0], iboost_information.address[1]);

                }

                if (receive_lqi != iboost_information.lqi) {
                    iboost_information.lqi = receive_lqi;
                    electricity_event.event = SL_LQI;
                    electricity_event.value = receive_lqi;
                    electricity_event.info = IB_NONE;
                    xQueueSend(g_main_queue, &electricity_event, 0);
                }
            }

            // main unit (sending info to iBoost Buddy)
            if (packet[2] == 0x22) {    
                #ifdef HEXDUMP  // declared in platformio.ini
                    // log level needs to be ESP_LOG_ERROR to get something to print!!
                    ESP_LOG_BUFFER_HEXDUMP(TAG, packet, pkt_size, ESP_LOG_ERROR);
                #endif
                ESP_LOGI(TAG, "iBoost frame received: length=%d, RSSI=%d, LQI=%d", pkt_size, rssi, receive_lqi);
                heating = (* ( short *) &packet[16]);
                p1 = (* ( long*) &packet[18]);
                p2 = (* ( long*) &packet[25]); // this depends on the request

                ESP_LOGI(TAG, "packet[6]: %d, packet[7]: %d", packet[6], packet[7]);
                if (packet[6]) {
                    b_is_water_heating_by_solar = false;
                } else {
                    b_is_water_heating_by_solar = true;
                }

                if (packet[7] == 1) {
                    b_is_cylinder_hot = true;
                } else {
                    b_is_cylinder_hot = false;
                }

                if (packet[12]) {
                    b_is_battery_ok = false;
                } else {
                    b_is_battery_ok = true;
                }

                boostTime=packet[5]; // boost time remaining (minutes)

                ESP_LOGI(TAG, "Heating: %d Watts  P1: %ld  %s: %ld Watts  P2: %ld", 
                    heating, p1, (p1/MAGIC_NUMBER < 0 ? "Exporting": "Importing"), 
                    (p1/MAGIC_NUMBER < 0 ? abs(p1/MAGIC_NUMBER): p1/MAGIC_NUMBER), p2); 

                // Importing or exporting electricity
                if (p1/MAGIC_NUMBER < 0) {   // exporting
                    electricity_event.event = SL_EXPORT;
                    electricity_event.value = abs(p1/MAGIC_NUMBER);
                    electricity_event.info = IB_NONE;
                } else if (p1/MAGIC_NUMBER > 0){            // importing
                    electricity_event.event = SL_IMPORT;
                    electricity_event.value = p1/MAGIC_NUMBER;
                    electricity_event.info = IB_NONE;
                }
                xQueueSend(g_main_queue, &electricity_event, 0);

                switch (packet[24]) {
                    case   SAVED_TODAY:
                        if (iboost_information.today != p2) {   // only update if value changed
                            iboost_information.today = p2;
                            electricity_event.event = SL_WT_TODAY;
                            electricity_event.value = p2;
                            electricity_event.info = IB_NONE;
                            xQueueSend(g_main_queue, &electricity_event, 0);
                        } 
                    break;

                    case   SAVED_YESTERDAY:
                        iboost_information.yesterday = p2;
                    break;

                    case   SAVED_LAST_7:
                        iboost_information.last7 = p2;
                    break;

                    case   SAVED_LAST_28:
                        iboost_information.last28 = p2;
                    break;

                    case   SAVED_TOTAL:
                        iboost_information.total = p2;
                    break;
                }

                if (b_is_cylinder_hot)
                    ESP_LOGI(TAG, "Water Tank HOT");
                else if (boostTime > 0)
                    ESP_LOGI(TAG, "Manual Boost ON"); 
                else if (b_is_water_heating_by_solar) {
                    ESP_LOGI(TAG, "Heating by Solar = %d Watts", heating);
                }
                else {
                    ESP_LOGI(TAG, "Water Heating OFF");
                }

                ESP_LOGI(TAG, "Today: %ld Wh   Yesterday: %ld Wh   Last 7 Days: %ld Wh   Last 28 Days: %ld Wh   Total: %ld Wh   Boost Time: %d", 
                    iboost_information.today, iboost_information.yesterday, iboost_information.last7, iboost_information.last28, iboost_information.total, boostTime);

                // Create JSON for sending via MQTT to MQTT server
                // How much solar we have used today to heat the hot water
                doc["savedToday"] = iboost_information.today;
                
                // Water tank status
                if (b_is_cylinder_hot) {
                    doc["hotWater"] =  "HOT";                        
                    electricity_event.event = SL_WT_STATUS;
                    electricity_event.value = 0;
                    electricity_event.info = IB_WT_HOT;
                } else if (b_is_water_heating_by_solar) {
                    ESP_LOGI(TAG, "Heating by solar detected");
                    doc["hotWater"] =  "Heating by Solar";                        
                    electricity_event.event = SL_WT_NOW;
                    electricity_event.value = heating;          // equates to PV being used now
                    electricity_event.info = IB_WT_HEATING;
                } else {
                    doc["hotWater"] =  "Off";                        
                    electricity_event.event = SL_WT_STATUS;
                    electricity_event.value = 0;
                    electricity_event.info = IB_WT_OFF;
                }
                xQueueSend(g_main_queue, &electricity_event, 0);
                

                // Status of the sender battery
                if (b_is_battery_ok) {
                    iboost_information.b_sender_battery_ok = true;
                    ESP_LOGI(TAG, "Sender Battery OK");
                    doc["battery"] =  "OK"; 
                    electricity_event.info = IB_BATTERY_OK;
                } else {
                    iboost_information.b_sender_battery_ok = false;
                    ESP_LOGI(TAG, "Warning - Sender Battery LOW");
                    doc["battery"] = "LOW"; 
                    electricity_event.info = IB_BATTERY_LOW;
                }
                electricity_event.event = SL_BATTERY;
                electricity_event.value = 0;
                xQueueSend(g_main_queue, &electricity_event, 0);

                xSemaphoreTake(keep_alive_mqtt_semaphore, portMAX_DELAY);
                if (mqtt_client.connected()) {
                    serializeJson(doc, msg);
                    mqtt_client.publish("iboost/iboost", msg);
                    ESP_LOGI(TAG, "Published MQTT message: %s", msg);           
                } else {
                    ESP_LOGW(TAG, "Unable to publish message: %s to MQTT - not connected!", msg);    
                }
                xSemaphoreGive(keep_alive_mqtt_semaphore);

                //client.publish("iboost/savedYesterday", msg);
                //client.publish("iboost/savedLast7", msg);
                //client.publish("iboost/savedLast28", msg);
                //client.publish("iboost/savedTotal", msg);
            }


            // Send message to LED task to blink the LED to show we've received a packet
            xQueueSend(inbuilt_led_queue, &b_flag, 0); // internal led
            xQueueSend(ws2812b_queue, &led, 0); // led strip

            // ESP_LOGI(TAG, "## Receive Task Stack Left: %d", uxTaskGetStackHighWaterMark(NULL));
        }   

        xSemaphoreGive(radio_semaphore);
        vTaskDelay(250 / portTICK_PERIOD_MS);       // Give some time so other tasks can run/complete
        // TODO - can we increase this delay or just use one task for tx and rx - no reason why not
    }
    vTaskDelete (NULL);
}


/**
 * @brief Transmit a packet to the iBoost main unit to request information
 * 
 */
void transmit_packet_task(void *parameter) {
    uint8_t tx_buffer[32];
    uint8_t request = 0xca;
    led_measage_t led = TX_FAKE_BUDDY_REQUEST;

    for( ;; ) {
        if(iboost_information.b_is_address_valid) {
            // whilst radio is transmitting no other radio operation should be in progress
            xSemaphoreTake(radio_semaphore, portMAX_DELAY);

            memset(tx_buffer, 0, sizeof(tx_buffer));

            if ((request < 0xca) || (request > 0xce)) 
                request = 0xca;

            // Payload
            tx_buffer[0] = iboost_information.address[0];
            tx_buffer[1] = iboost_information.address[1];		  
            tx_buffer[2] = 0x21;
            tx_buffer[3] = 0x8;
            tx_buffer[4] = 0x92;
            tx_buffer[5] = 0x7;
            tx_buffer[8] = 0x24;
            tx_buffer[10] = 0xa0;
            tx_buffer[11] = 0xa0;
            tx_buffer[12] = request; // request information (on this topic) from the main unit
            tx_buffer[14] = 0xa0;
            tx_buffer[15] = 0xa0;
            tx_buffer[16] = 0xc8;

            radio.strobe(CC1101_SIDLE);
            radio.writeRegister(CC1101_TXFIFO, 0x1d);             // packet length
            radio.writeBurstRegister(CC1101_TXFIFO, tx_buffer, 29);   // write the data to the TX FIFO
            radio.strobe(CC1101_STX);
            delay(5);
            radio.strobe(CC1101_SWOR);
            delay(5);
            radio.strobe(CC1101_SFRX);
            radio.strobe(CC1101_SIDLE);
            radio.strobe(CC1101_SRX);       // Re-enable receive

            switch (request) {
                case SAVED_TODAY:
                    ESP_LOGI(TAG, "Sent request: Saved Today");
                break;
                case SAVED_YESTERDAY:
                    ESP_LOGI(TAG, "Sent request: Saved Yesterday");
                break;
                case SAVED_LAST_7:
                    ESP_LOGI(TAG, "Sent request: Saved Last 7 Days");
                break;
                case SAVED_LAST_28:
                    ESP_LOGI(TAG, "Sent request: Saved Last 28 Days");
                break;
                case SAVED_TOTAL:
                    ESP_LOGI(TAG, "Sent request: Saved In Total");
                break;
            }

            request++;
                        
            xSemaphoreGive(radio_semaphore);

            xQueueSend(ws2812b_queue, &led, 0);
                        
            // ESP_LOGI(TAG, "## Transmit Task Stack Left: %d", uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(PING_IBOOST_UNIT / portTICK_PERIOD_MS);          // PING_IBOOST_UNIT iBoost unit every n seconds
    }
    vTaskDelete (NULL);
}


/**
 * @brief Set up the CC1101 for receiving iBoost packets
 * 
 */
void radio_setup() {
    radio.reset();
    radio.begin(868.350e6); // Freq, do not forget the "e6"
    radio.setMaxPktSize(61);
    radio.writeRegister(CC1101_FREQ2, 0x21); 
    radio.writeRegister(CC1101_FREQ1, 0x65);
    radio.writeRegister(CC1101_FREQ0, 0xe8);
    radio.writeRegister(CC1101_FSCTRL1, 0x08); // fif=203.125kHz
    radio.writeRegister(CC1101_FSCTRL0, 0x00); // No offset
    radio.writeRegister(CC1101_MDMCFG4, 0x5B); // CHANBW_E = 1 CHANBW_M=1 BWchannel =325kHz   DRATE_E=11
    radio.writeRegister(CC1101_MDMCFG3, 0xF8); // DRATE_M=248 RDATA=99.975kBaud
    radio.writeRegister(CC1101_MDMCFG2, 0x03); // Disable digital DC blocking filter before demodulator enabled. MOD_FORMAT=000 (2-FSK) Manchester Coding disabled Combined sync-word qualifier mode = 30/32 sync word bits detected
    radio.writeRegister(CC1101_MDMCFG1, 0x22); // Forward error correction disabled 4 preamble bytes transmitted CHANSPC_E=2
    radio.writeRegister(CC1101_MDMCFG0, 0xF8); // CHANSPC_M=248 200kHz channel spacing
    radio.writeRegister(CC1101_CHANNR, 0x00); // The 8-bit unsigned channel number, which is multiplied by the channel spacing setting and added to the base frequency.
    radio.writeRegister(CC1101_DEVIATN, 0x47); // DEVIATION_E=4 DEVIATION_M=7 ±47.607 kHz deviation
    radio.writeRegister(CC1101_FREND1, 0xB6); // Adjusts RX RF device
    radio.writeRegister(CC1101_FREND0, 0x10); // Adjusts TX RF device
    radio.writeRegister(CC1101_MCSM0, 0x18); // Calibrates whngoing from IDLE to RX or TX (or FSTXON) PO_TIMEOUT 149-155uS Pin control disabled XOSC off in sleep mode
    //radio.writeRegister(CC1101_MCSM1, 0x00); // Channel clear = always Return to idle after packet reception Return to idle after transmission
    radio.writeRegister(CC1101_FOCCFG, 0x1D); // The frequency compensation loop gain to be used before a sync word is detected = 4K The frequency compensation loop gain to be used after a sync word is Detected = K/2 The saturation point for the frequency offset compensation algorithm = ±BWchannel /8
    radio.writeRegister(CC1101_BSCFG, 0x1C); // The clock recovery feedback loop integral gain to be used before a sync word is detected = KI The clock recovery feedback loop proportional gain to be used before a sync word is detected = 2KP The clock recovery feedback loop integral gain to be used after a sync word is Detected = KI/2 The clock recovery feedback loop proportional gain to be used after a sync word is detected = KP The saturation point for the data rate offset compensation algorithm = ±0 (No data rate offset compensation performed)
    radio.writeRegister(CC1101_AGCCTRL2, 0xC7); // The 3 highest DVGA gain settings can not be used. Maximum allowable LNA + LNA 2 gain relative to the maximum possible gain. Target value for the averaged amplitude from the digital channel filter = 42dB
    radio.writeRegister(CC1101_AGCCTRL1, 0x00); // LNA 2 gain is decreased to minimum before decreasing LNA gain Relative carrier sense threshold disabled Sets the absolute RSSI threshold for asserting carrier sense to MAGN_TARGET
    radio.writeRegister(CC1101_AGCCTRL0, 0xB2); // Sets the level of hysteresis on the magnitude deviation (internal AGC signal that determine gain changes) to Medium hysteresis, medium asymmetric dead zone, medium gain Sets the number of channel filter samples from a gain adjustment has been made until the AGC algorithm starts accumulating new samples to 32 samples AGC gain never frozen
    radio.writeRegister(CC1101_FSCAL3, 0xEA); // Detailed calibration
    radio.writeRegister(CC1101_FSCAL2, 0x2A); //
    radio.writeRegister(CC1101_FSCAL1, 0x00); //
    radio.writeRegister(CC1101_FSCAL0, 0x1F); //
    radio.writeRegister(CC1101_FSTEST, 0x59); // Test register
    radio.writeRegister(CC1101_TEST2, 0x81); // Values to be used from SmartRF software
    radio.writeRegister(CC1101_TEST1, 0x35); //
    radio.writeRegister(CC1101_TEST0, 0x09); //
    radio.writeRegister(CC1101_IOCFG2, 0x0B); // Active High Serial Clock
    radio.writeRegister(CC1101_IOCFG0, 0x46); // Analog temperature sensor disabled Active High Asserts when sync word has been sent / received, and de-asserts at the end of the packet
    radio.writeRegister(CC1101_PKTCTRL1, 0x04); // Sync word is always accepted Automatic flush of RX FIFO when CRC is not OK disabled Two status bytes will be appended to the payload of the packet. The status bytes contain RSSI and LQI values, as well as CRC OK. No address checkof received packages.
    radio.writeRegister(CC1101_PKTCTRL0, 0x05); // Data whitening off Normal mode, use FIFOs for RX and TX CRC calculation in TX and CRC check in RX enabled Variable packet length mode. Packet length configured by the first byte after sync word
    radio.writeRegister(CC1101_ADDR, 0x00); // Address used for packet filtration. Optional broadcast addresses are 0 (0x00) and 255 (0xFF).

    static uint8_t paTable[] = {0xC6, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
    radio.writeBurstRegister(CC1101_PATABLE, paTable, sizeof(paTable));

    radio.strobe(CC1101_SIDLE); 
    radio.strobe(CC1101_SPWD); 

    radio.setIDLEstate();                 // Was set to receive, moved so set when all setup of program is finished

    ESP_LOGI(TAG, "CC1101 set up complete, radio set to idle state");
    char tx_item[] = "C1101 set up complete";
    UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    }
}


/**
 * @brief Attempt to connect with the MQTT broker
 * 
 */
void connect_to_mqtt(void) {
    xSemaphoreTake(keep_alive_mqtt_semaphore, portMAX_DELAY); 

    // Create client ID from mac address
    String client_id = String(mac_address[0]) + String(mac_address[5]);
    ESP_LOGI(TAG, "Connecting to MQTT as client ID: %s", client_id);

    while (!mqtt_client.connected()) {
        // Attempt to connect
        if (mqtt_client.connect(client_id.c_str(), MQTT_USER, MQTT_USER_PASSWORD)) {
            ESP_LOGI(TAG, "   connecting to MQTT...");
            vTaskDelay(250);
        }      
    }

    // subscribe to topics we're interested in
    if (mqtt_client.subscribe("solar/pvnow", 0)) {
        ESP_LOGI(TAG, "Subscribed to MQTT topic solar/pvnow");
    };
    if (mqtt_client.subscribe("solar/pvtotal", 0)) {
        ESP_LOGI(TAG, "Subscribed to MQTT topic solar/pvtotal");
    }
    // if (MQTTclient.subscribe("weather/description", 0)) {
    //     ESP_LOGI(TAG, "Subscribed to MQTT topic weather/description");
    // }
    // if (MQTTclient.subscribe("weather/outsidetemp", 0)) {
    //     ESP_LOGI(TAG, "Subscribed to MQTT topic weather/outsidetemp");
    // }
     
    xSemaphoreGive(keep_alive_mqtt_semaphore); 

    mqtt_client.setCallback(mqtt_callback);

    ESP_LOGI(TAG, "Connected to MQTT");

    char tx_item[] = "Connected to MQTT";
    UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    }
}

/**
 * @brief Connect to WiFi
 * 
 */
void connect_to_wifi(void) {
    String temp_string = "IP Address: ";
    char tx_item[50];
    UBaseType_t res = pdFALSE;

    memset(tx_item, '\0', sizeof(tx_item));

    ESP_LOGI(TAG, "Connecting to WiFi");
    strcpy(tx_item, "Attempting to connect to WiFi");
    res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    }

    while (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        WiFi.begin(SSID, WIFI_PASSWORD);
        ESP_LOGI(TAG, "   waiting for WiFi connection");
        //updateLog("Waiting for WiFi connection");
        strcpy(tx_item, "Waiting for WiFi connection");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }

        vTaskDelay(4000);
    }
    ESP_LOGI(TAG, "Connected to Wi-Fi");

    strcpy(tx_item, "Connected to WiFi, IP address ");
    strcat(tx_item, WiFi.localIP().toString().c_str());
    res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    } else {
        ESP_LOGI(TAG, "Item sent to Ringbuffer");
    }

    WiFi.macAddress(mac_address);
    ESP_LOGI(TAG, "IP Address: %s  - MAC Address: %d.%d.%d.%d.%d.%d", 
        WiFi.localIP().toString(), mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
}


/**
 * @brief Convert wifi_connection_status_message
 * 
 * @param String Status of WiFi connection
 */
String wifi_connection_status_message(wl_status_t wifi_status) {
    String status = "Replace with status";

    switch (wifi_status) {
        case WL_IDLE_STATUS:
            status = "Idle";        
        break;
        case WL_NO_SSID_AVAIL:
            status = "No SSID Available";        
        break;
        case WL_SCAN_COMPLETED:
            status = "Scan Completed";        
        break;
        case WL_CONNECTED:
            status = "Connected";        
        break;
        case WL_CONNECT_FAILED:
            status = "Connection Failed";        
        break;
        case WL_CONNECTION_LOST:
            status = "Connection Lost";        
        break;
        case WL_DISCONNECTED:
            status = "Disconnected";        
        break;               
        default:
            status = "Unknown";
        break;
    }

    return status;
}

/**
 * @brief MQTT Callback function
 * 
 * @param topic Topic of message arriving
 * @param message Message
 * @param length Length of message
 */
static void mqtt_callback(char* topic, byte* message, unsigned int length) {
    String message_temp;
    electricity_event_t electricity_event;
   
    for (int i = 0; i < length; i++) {
        Serial.print((char)message[i]);
        message_temp += (char)message[i];
    }

    ESP_LOGI(TAG, "MQTT topic: %s, message: %s", topic, message_temp);

    if (String(topic) == "solar/pvnow") {
        // Seems to generate up to 30w even at night time, no need to 
        // show it, not sure how true it is
        if (message_temp.toFloat() > 30) {
            electricity_event.event = SL_NOW;   
            electricity_event.value = message_temp.toFloat();
            electricity_event.info = IB_NONE;
            xQueueSend(g_main_queue, &electricity_event, 0);
        }
    }   
    
    if (String(topic) == "solar/pvtotal") {
        electricity_event.event = SL_TODAY;
        electricity_event.value = message_temp.toFloat();
        electricity_event.info = IB_NONE;
        xQueueSend(g_main_queue, &electricity_event, 0);
    }

    // if (String(topic) == "weather/description") {
    //     message_temp.toCharArray(weatherDescription, 35);
    // }   
    
    // if (String(topic) == "weather/outsidetemp") {
    //     weatherTemperature = message_temp.toFloat();
    // }
}