#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "main.h"
#include "my_ringbuf.h"
#include "config.h"
#include "CC1101_RFx.h"

// Defines
#define PING 10000      // Ping iBoost main unit for data every 10 seconds

// ESP32 Wroom 32: SCK_PIN = 18; MISO_PIN = 19; MOSI_PIN = 23; SS_PIN = 5; GDO0 = 2;
#define SS_PIN 5
#define MISO_PIN 19

CC1101 radio(SS_PIN,  MISO_PIN);

// freeRTOS specific variables
TaskHandle_t blinkLEDTaskHandle = NULL;
TaskHandle_t ws2812bTaskHandle = NULL;

TaskHandle_t mqqtKeepAliveTaskHandle = NULL;
TaskHandle_t receivePacketTaskHandle = NULL;
TaskHandle_t transmitPacketTaskHandle = NULL;

TaskHandle_t displayTaskHandle = NULL;

QueueHandle_t ledTaskQueue;
QueueHandle_t ws2812Queue;
QueueHandle_t transmitTaskQueue;
int queueSize = 10;

SemaphoreHandle_t keepAliveMQTTSemaphore;
SemaphoreHandle_t radioSemaphore;

RingbufHandle_t buf_handle;

//#define GDO0_PIN 2      // Not used

#define MSG_BUFFER_SIZE	(100)
#define PIN_WS2812B 3 // 13 on wroom-32d           // Output pin on ESP32 that controls the addressable LEDs
#define NUM_PIXELS 4            // Number of LEDs (pixels) we can control
#define GLOW 75

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
enum ledMessage{ 
    TX_FAKE_BUDDY_REQUEST,
    RECEIVE,
    ERROR,
    CLEAR,
    CLEAR_ERROR,
    BLANK
};

struct iboost {
    long today;
    long yesterday;
    long last7;
    long last28;
    long total;
    uint8_t address[2];
    bool addressValid;
} iboostInfo;

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
static const char* TAG = "iBoost";

WiFiClient wifiClient;
byte macAddress[6];
PubSubClient MQTTclient(MQTT_SERVER, MQTT_PORT, wifiClient);
colours_t pixelColours;
char weatherDescription[35];    // buffer for the current weather description from OpenWeatherMap
static portMUX_TYPE myMux = portMUX_INITIALIZER_UNLOCKED;


/* Function prototypes */
void blinkLEDTask(void *parameter);
void mqqtKeepAliveTask(void *parameter);
void receivePacketTask(void *parameter);
void transmitPacketTask(void *parameter);
void ws2812bTask(void *parameter);
///////
void radioSetup();
void connectToWiFi(void);
void connectToMQTT(void);
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
String connectionStatusMessage(wl_status_t wifiStatus);
static void mqttCallback(char* topic, byte* message, unsigned int length);

/**
 * @brief Set up everything. SPI, WiFi, MQTT, and CC1101
 * 
 */
void setup() {
    bool setupFlag = true;      // Flag for confirming setup was sucessfull or not
    BaseType_t xReturned;
    UBaseType_t res = pdFALSE;
    char tx_item[50];

    // Initialise WS2812B strip object (REQUIRED)    
    ws2812b.begin();
    ws2812b.show();
    ws2812b.clear();
    for (int pixel = 0; pixel < NUM_PIXELS; pixel++) {          // for each pixel
        ws2812b.setPixelColor(pixel, pixelColours.violet);      // it only takes effect when pixels.show() is called
    }
    ws2812b.show();  // update to the WS2812B Led Strip

    //Create ring buffer
    buf_handle = xRingbufferCreate(1028, RINGBUF_TYPE_NOSPLIT);
    if (buf_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        setupFlag = false;
    }

    // Create queue for sending messages to the LED task and transmit packet task
    ledTaskQueue = xQueueCreate(queueSize, sizeof(bool));  // internal led
    if (ledTaskQueue == NULL) {
        ESP_LOGE(TAG, "Error creating ledTaskQueue");
        // updateLog("Error creating ledTaskQueue");
        strcpy(tx_item, "Error creating ledTaskQueue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }

        setupFlag = false;
    }

    ws2812Queue = xQueueCreate(queueSize, sizeof(ledMessage));  // led strip
    if (ws2812Queue == NULL) {
        ESP_LOGE(TAG, "Error creating ws2812Queue");
        // updateLog("Error creating ws2812Queue");
        strcpy(tx_item, "Error creating ws2812Queue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }

        setupFlag = false;
    }

    transmitTaskQueue = xQueueCreate(queueSize, sizeof(bool));
    if (transmitTaskQueue == NULL) {
        ESP_LOGE(TAG, "Error creating transmitTaskQueue");
        // updateLog("Error creating transmitTaskQueue");
        strcpy(tx_item, "Error creating transmitTaskQueue");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        setupFlag = false;
    }

    // Create required semaphores
    keepAliveMQTTSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(keepAliveMQTTSemaphore);            // Stop keep alive when publishing

    radioSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(radioSemaphore);

    iboostInfo.today = 0;
    iboostInfo.yesterday = 0;
    iboostInfo.last7 = 0;
    iboostInfo.last28 = 0;
    iboostInfo.total = 0;
    iboostInfo.addressValid = false;

    SPI.begin();
    ESP_LOGI(TAG, "SPI OK");
    strcpy(tx_item, "SPI Ok");
    res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    }

    // updateLog("SPI Ok");
    // Set up the radio
    radioSetup();
    
    /* LED setup - so we can use the module without serial terminal,
       set low to start so it's off and flashes when it receives a packet */
    pinMode(LED_BUILTIN, OUTPUT);   

    // Internal blue LED on ESP32 board
    xReturned = xTaskCreate(blinkLEDTask, "blinkLEDTask", 1024, NULL, tskIDLE_PRIORITY, &blinkLEDTaskHandle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create blinkLEDTask");
        // updateLog("Failed to create blinkLEDTask");
        strcpy(tx_item, "Error creating blinkLEDTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        setupFlag = false;
    }

    xReturned = xTaskCreate(ws2812bTask, "ws2812bTask", 2048, NULL, tskIDLE_PRIORITY, &ws2812bTaskHandle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ws2812bTask");
        // updateLog("Failed to create ws2812bTask");
        strcpy(tx_item, "Error creating ws2812bTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        setupFlag = false;
    }

    /* This task also creates/checks the WiFi connection */
    xReturned = xTaskCreate( mqqtKeepAliveTask, "mqqtKeepAliveTask", 4096, NULL, 1, &mqqtKeepAliveTaskHandle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mqqtKeepAliveTask");
        // updateLog("Failed to create mqqtKeepAliveTask");
        strcpy(tx_item, "Error creating mqttKeepAliveTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        setupFlag = false;
    }

    xReturned = xTaskCreate(receivePacketTask, "receivePacketTask", 4096, NULL, 3, &receivePacketTaskHandle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receivePacketTask");
        // updateLog("Failed to create receivePacketTask");
        strcpy(tx_item, "Error creating receivePackeTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        setupFlag = false;
    }

    xReturned = xTaskCreate(transmitPacketTask, "transmitPacketTask", 4096, NULL, 3, &transmitPacketTaskHandle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create transmitPacketTask");
        // updateLog("Failed to create transmitPacketTask");
        strcpy(tx_item, "Error creating transmitPacketTask");
        res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send Ringbuffer item");
        }
        setupFlag = false;
    }

    // Actioned in screen.cpp
    xReturned = xTaskCreate(displayTask, "displayTask", 3072, NULL, tskIDLE_PRIORITY, &displayTaskHandle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create displayTask, setup failed");
        // No point send a log item as there isn't any display task to action it!!
        setupFlag = false;
    } 

    // Everything set up okay so turn (blue) LED off
    if (setupFlag) {
        digitalWrite(LED_BUILTIN, LOW);

        ws2812b.clear();
        ws2812b.show();

        //xRingbufferPrintInfo(buf_handle);

        radio.setRXstate();             // Set the current state to RX : listening for RF packets
    } else {
        ESP_LOGE(TAG, "Setup Failed!!!");
        // updateLog("Setup Failed!!!");    
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
void blinkLEDTask(void *parameter) {
    bool flag = false;

    for( ;; ) {
        xQueueReceive(ledTaskQueue, &flag, portMAX_DELAY);
        if (flag) {
            digitalWrite(LED_BUILTIN, HIGH);        // Turn LED on
            vTaskDelay(200 / portTICK_PERIOD_MS);   // Wait 200ms
            digitalWrite(LED_BUILTIN, LOW);         // Turn LED off
            flag = false;                           // Reset flag
        }
    }
    vTaskDelete (NULL);
}


/**
 * @brief Blink an LED on the LED strip dependiing on request.
 * 
 * @param parameter Parameters passed to task on creation.
 */
void ws2812bTask(void *parameter) {
    ledMessage led = BLANK;

    for( ;; ) {
        xQueueReceive(ws2812Queue, &led, portMAX_DELAY);
        switch (led) {
            case TX_FAKE_BUDDY_REQUEST:
                ws2812b.setPixelColor(0, pixelColours.green);
                ws2812b.show();
                vTaskDelay(150 / portTICK_PERIOD_MS);
                ws2812b.setPixelColor(0, pixelColours.clear);
                ws2812b.show();
                break;
            case RECEIVE:
                ws2812b.setPixelColor(1, pixelColours.blue);
                ws2812b.show();
                vTaskDelay(150 / portTICK_PERIOD_MS);
                ws2812b.setPixelColor(1, pixelColours.clear);
                ws2812b.show();
                break;
            case ERROR:
                ws2812b.setPixelColor(2, pixelColours.red);
                ws2812b.setPixelColor(3, pixelColours.red);
                ws2812b.show();
                break;
            case CLEAR_ERROR:
                ws2812b.setPixelColor(2, pixelColours.clear);
                ws2812b.setPixelColor(3, pixelColours.clear);
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
void mqqtKeepAliveTask(void *parameter) {
    ledMessage led = BLANK;

    // setting must be set before a mqtt connection is made
    MQTTclient.setKeepAlive( 90 ); // setting keep alive to 90 seconds makes for a very reliable connection.
    MQTTclient.setBufferSize( 256 );
    MQTTclient.setSocketTimeout( 15 );
    for( ;; ) {
        //check for a is-connected and if the WiFi 'thinks' its connected, found checking on both is more realible than just a single check
        if ((MQTTclient.connected()) && (WiFi.status() == WL_CONNECTED))
        {   // whiles MQTTlient.loop() is running no other mqtt operations should be in process
            xSemaphoreTake(keepAliveMQTTSemaphore, portMAX_DELAY); 
            MQTTclient.loop();
            xSemaphoreGive(keepAliveMQTTSemaphore);
        } else {       
            led = ERROR;
            xQueueSend(ws2812Queue, &led, 0);
            wl_status_t status = WiFi.status();
            ESP_LOGI(TAG, "MQTT keep alive: MQTT %s,  WiFi %s", 
                    MQTTclient.connected() ? "Connected" : "Not Connected", 
                    connectionStatusMessage(status)
            );
            if (!(status == WL_CONNECTED)) {
                connectToWiFi();
            }

            connectToMQTT();

            configTime(0, 0, SNTP_TIME_SERVER);  // connect to ntp time server
            updateLocalTime();                      // update the local time
    
            led = CLEAR_ERROR;
            char tx_item[] = "Set up complete";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAG, "Failed to send Ringbuffer item");
            } else {
                ESP_LOGI(TAG, "Item sent to Ringbuffer");
            }

            //updateLog("Set up complete");
            xQueueSend(ws2812Queue, &led, 0);
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
void receivePacketTask(void *parameter) { 
    byte packet[65];                // The CC1101 library sets the biggest packet at 61
    uint8_t addressLQI = 255;       // set received LQI to lowest value
    uint8_t rxLQI;                  // signal strength test 
    JsonDocument doc;               // Create JSON message for sending via MQTT
    char msg[MSG_BUFFER_SIZE];      // MQTT message
    ledMessage led = RECEIVE;
    bool flag = true;

    for( ;; ) {
        xSemaphoreTake(radioSemaphore, portMAX_DELAY);
        byte pkt_size = radio.getPacket(packet);
        if (pkt_size > 0 && radio.crcok()) {        // We have a valid packet with some data
            short heating;
            long p1, p2;
            byte boostTime;
            bool waterHeating, cylinderHot, batteryOk;
            int16_t rssi = radio.getRSSIdbm();

            #ifdef HEXDUMP  // declared in platformio.ini
                // log level needs to be ESP_LOG_ERROR to get something to print!!
                ESP_LOG_BUFFER_HEXDUMP(TAG, packet, pkt_size, ESP_LOG_ERROR);
            #endif

            //   buddy request                            sender packet
            if ((packet[2] == 0x21 && pkt_size == 29) || (packet[2] == 0x01 && pkt_size == 44)) {
                rxLQI = radio.getLQI();
                ESP_LOGI(TAG, "Buddy/Sender frame received: length=%d, RSSI=%d, LQI=%d", pkt_size, rssi, rxLQI);

                if(rxLQI < addressLQI) { // is the signal stronger than the previous/none
                    addressLQI = rxLQI;
                    iboostInfo.address[0] = packet[0]; // save the address of the packet	0x1c7b; //
                    iboostInfo.address[1] = packet[1];
                    iboostInfo.addressValid = true;

                    ESP_LOGI(TAG, "Updated iBoost address to: %02x,%02x", iboostInfo.address[0], iboostInfo.address[1]);
                }		
            }

            // main unit (sending info to iBoost Buddy)
            if (packet[2] == 0x22) {    
                ESP_LOGI(TAG, "iBoost frame received: length=%d, RSSI=%d, LQI=%d", pkt_size, rssi, rxLQI);
                heating = (* ( short *) &packet[16]);
                p1 = (* ( long*) &packet[18]);
                p2 = (* ( long*) &packet[25]); // this depends on the request

                // ESP_LOGI(TAG, "packet[6]: %d, packet[7]: %d", packet[6], packet[7]);
                if (packet[6] == 0) {
                    waterHeating = false;
                    setWaterTankFlag(false);
                } else {
                    waterHeating = true;
                    if (heating > 0) {  // more than 0 watts are being used
                        setWaterTankFlag(true);
                        setWTNow((int) heating);
                    } else {
                        setWaterTankFlag(false);
                    }
                }

                if (packet[7] == 1) {
                    cylinderHot = true;
                } else {
                    cylinderHot = false;
                }

                if (packet[12]) {
                    batteryOk = false;
                } else {
                    batteryOk = true;
                }

                boostTime=packet[5]; // boost time remaining (minutes)

                ESP_LOGI(TAG, "Heating: %d Watts  P1: %ld  %s: %ld Watts  P2: %ld", 
                    heating, p1, (p1/370 < 0 ? "Exporting": "Importing"), (p1/370 < 0 ? abs(p1/370): p1/370), p2); // was 390

                // See if we're importing or exporting electricity
                if (p1/377 < 0) {   // exporting
                    setGridExportFlag(true);
                    setExportNow((int) abs(p1/370));
                } else if (p1/370 > 0){            // importing
                    setGridImportFlag(true);
                    setImportNow((int) p1/370);
                }
                
                // Serial.print("  P3: ");
                // Serial.print((* (signed long*) &packet[29]) );
                // Serial.print("  P4: ");
                // Serial.println((* (signed long*) &packet[30]) );
            
                switch (packet[24]) {
                    case   SAVED_TODAY:
                        iboostInfo.today = p2;
                        setWTToday((int) p2);
                        break;
                    case   SAVED_YESTERDAY:
                        iboostInfo.yesterday = p2;
                        break;
                    case   SAVED_LAST_7:
                        iboostInfo.last7 = p2;
                        break;
                    case   SAVED_LAST_28:
                        iboostInfo.last28 = p2;
                        break;
                    case   SAVED_TOTAL:
                        iboostInfo.total = p2;
                        break;
                }

                if (cylinderHot)
                    ESP_LOGI(TAG, "Water Tank HOT");
                else if (boostTime > 0)
                    ESP_LOGI(TAG, "Manual Boost ON"); 
                else if (waterHeating) {
                    ESP_LOGI(TAG, "Heating by Solar = %d Watts", heating);
                }
                else {
                    ESP_LOGI(TAG, "Water Heating OFF");
                }

                if (batteryOk)
                    ESP_LOGI(TAG, "Sender Battery OK");
                else
                    ESP_LOGI(TAG, "Warning - Sender Battery LOW");

                ESP_LOGI(TAG, "Today: %ld Wh   Yesterday: %ld Wh   Last 7 Days: %ld Wh   Last 28 Days: %ld Wh   Total: %ld Wh   Boost Time: %d", 
                    iboostInfo.today, iboostInfo.yesterday, iboostInfo.last7, iboostInfo.last28, iboostInfo.total, boostTime);

                // Create JSON for sending via MQTT to MQTT server
                // How much solar we have used today to heat the hot water
                doc["savedToday"] = iboostInfo.today;
                
                // Water tank status
                if (cylinderHot) {
                    doc["hotWater"] =  "HOT";                        
                } else if (waterHeating) {
                    doc["hotWater"] =  "Heating by Solar";                        
                } else {
                    doc["hotWater"] =  "Off";                        
                }
                
                // Status of the sender battery
                if (batteryOk) {
                    doc["battery"] =  "OK"; 
                } else {
                    doc["battery"] = "LOW"; 
                }
                
                xSemaphoreTake(keepAliveMQTTSemaphore, portMAX_DELAY);
                if (MQTTclient.connected()) {
                    serializeJson(doc, msg);
                    MQTTclient.publish("iboost/iboost", msg);
                    ESP_LOGI(TAG, "Published MQTT message: %s", msg);           
                } else {
                    ESP_LOGW(TAG, "Unable to publish message: %s to MQTT - not connected!", msg);    
                }
                xSemaphoreGive(keepAliveMQTTSemaphore);

                //client.publish("iboost/savedYesterday", msg);
                //client.publish("iboost/savedLast7", msg);
                //client.publish("iboost/savedLast28", msg);
                //client.publish("iboost/savedTotal", msg);
            }


            // Send message to LED task to blink the LED to show we've received a packet
            xQueueSend(ledTaskQueue, &flag, 0); // internal led
            xQueueSend(ws2812Queue, &led, 0); // led strip

            // ESP_LOGI(TAG, "## Receive Task Stack Left: %d", uxTaskGetStackHighWaterMark(NULL));
         }   

        xSemaphoreGive(radioSemaphore);
        vTaskDelay(100 / portTICK_PERIOD_MS);       // Give some time so other tasks can run/complete
    }
    vTaskDelete (NULL);
}


/**
 * @brief Transmit a packet to the iBoost main unit to request information
 * 
 */
void transmitPacketTask(void *parameter) {
    uint8_t txBuf[32];
    uint8_t request = 0xca;
    ledMessage led = TX_FAKE_BUDDY_REQUEST;

    for( ;; ) {
        if(iboostInfo.addressValid) {
            // whilst radio is transmitting no other radio operation should be in progress
            xSemaphoreTake(radioSemaphore, portMAX_DELAY);

            memset(txBuf, 0, sizeof(txBuf));

            if ((request < 0xca) || (request > 0xce)) 
                request = 0xca;

            // Payload
            txBuf[0] = iboostInfo.address[0];
            txBuf[1] = iboostInfo.address[1];		  
            txBuf[2] = 0x21;
            txBuf[3] = 0x8;
            txBuf[4] = 0x92;
            txBuf[5] = 0x7;
            txBuf[8] = 0x24;
            txBuf[10] = 0xa0;
            txBuf[11] = 0xa0;
            txBuf[12] = request; // request information (on this topic) from the main unit
            txBuf[14] = 0xa0;
            txBuf[15] = 0xa0;
            txBuf[16] = 0xc8;

            radio.strobe(CC1101_SIDLE);
            radio.writeRegister(CC1101_TXFIFO, 0x1d);             // packet length
            radio.writeBurstRegister(CC1101_TXFIFO, txBuf, 29);   // write the data to the TX FIFO
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
                        
            xSemaphoreGive(radioSemaphore);

            xQueueSend(ws2812Queue, &led, 0);
                        
            // ESP_LOGI(TAG, "## Transmit Task Stack Left: %d", uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(PING / portTICK_PERIOD_MS);          // Ping iBoost unit every n seconds
    }
    vTaskDelete (NULL);
}


/**
 * @brief Set up the CC1101 for receiving iBoost packets
 * 
 */
void radioSetup() {
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

    //updateLog("CC1101 set up complete");
}


/**
 * @brief Attempt to connect with the MQTT broker
 * 
 */
void connectToMQTT(void) {
    xSemaphoreTake(keepAliveMQTTSemaphore, portMAX_DELAY); 

    // Create client ID from mac address
    String clientId = String(macAddress[0]) + String(macAddress[5]);
    ESP_LOGI(TAG, "Connecting to MQTT as client ID: %s", clientId);

    while (!MQTTclient.connected()) {
        // Attempt to connect
        if (MQTTclient.connect(clientId.c_str(), MQTT_USER, MQTT_USER_PASSWORD)) {
            ESP_LOGI(TAG, "   connecting to MQTT...");
            vTaskDelay(250);
        }      
    }

    // subscribe to topics we're interested in
    if (MQTTclient.subscribe("solar/pvnow", 0)) {
        ESP_LOGI(TAG, "Subscribed to MQTT topic solar/pvnow");
    };
    if (MQTTclient.subscribe("solar/pvtotal", 0)) {
        ESP_LOGI(TAG, "Subscribed to MQTT topic solar/pvtotal");
    }
    // if (MQTTclient.subscribe("weather/description", 0)) {
    //     ESP_LOGI(TAG, "Subscribed to MQTT topic weather/description");
    // }
    // if (MQTTclient.subscribe("weather/outsidetemp", 0)) {
    //     ESP_LOGI(TAG, "Subscribed to MQTT topic weather/outsidetemp");
    // }
     
    xSemaphoreGive(keepAliveMQTTSemaphore); 

    MQTTclient.setCallback(mqttCallback);

    ESP_LOGI(TAG, "Connected to MQTT");

    char tx_item[] = "Connected to MQTT";
    UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    }

    //updateLog("Connected to MQTT");
}

/**
 * @brief Connect to WiFi
 * 
 */
void connectToWiFi(void) {
    String tempString = "IP Address: ";
    char tx_item[50];
    UBaseType_t res = pdFALSE;

    memset(tx_item, '\0', sizeof(tx_item));

    ESP_LOGI(TAG, "Connecting to WiFi");
    strcpy(tx_item, "Connecting to WiFi");
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
    //updateLog("Connected to Wi-Fi");

    strcpy(tx_item, "Connected to WiFi");
    res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send Ringbuffer item");
    } else {
        ESP_LOGI(TAG, "Item sent to Ringbuffer");
    }

    WiFi.macAddress(macAddress);
    ESP_LOGI(TAG, "IP Address: %s  - MAC Address: %d.%d.%d.%d.%d.%d", 
        WiFi.localIP().toString(), macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
}


/**
 * @brief Convert connectionStatusMessage
 * 
 * @param String Status of WiFi connection
 */
String connectionStatusMessage(wl_status_t wifiStatus) {
    String status;

    switch (wifiStatus) {
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
static void mqttCallback(char* topic, byte* message, unsigned int length) {
    String messageTemp;
    int pvNow = 0;
    float pvToday = 0.0;
   
    for (int i = 0; i < length; i++) {
        Serial.print((char)message[i]);
        messageTemp += (char)message[i];
    }

    ESP_LOGI(TAG, "MQTT topic: %s, message: %s", topic, messageTemp);

    if (String(topic) == "solar/pvnow") {
        pvNow = messageTemp.toInt();
        if (pvNow > 30) {
            setPVNow(pvNow);
            setSolarGenerationFlag(true);
        } else {
            setSolarGenerationFlag(false);
        }
    }   
    
    if (String(topic) == "solar/pvtotal") {
        pvToday = messageTemp.toFloat();
        setPVToday(pvToday);
    }

    // if (String(topic) == "weather/description") {
    //     portENTER_CRITICAL_ISR(&myMux);
    //     messageTemp.toCharArray(weatherDescription, 35);
    //     portEXIT_CRITICAL_ISR(&myMux);
    // }   
    
    // if (String(topic) == "weather/outsidetemp") {
    //     portENTER_CRITICAL_ISR(&myMux);
    //     weatherTemperature = messageTemp.toFloat();
    //     portEXIT_CRITICAL_ISR(&myMux);
    // }
}
