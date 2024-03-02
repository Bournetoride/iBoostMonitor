#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>

#ifdef TTGO
#include <TFT_eSPI.h> 
#include "solar-panel.h"
#endif

#include "config.h"
#include "CC1101_RFx.h"


// ESP32 Wroom 32: SCK_PIN = 18; MISO_PIN = 19; MOSI_PIN = 23; SS_PIN = 5; GDO0 = 2;
#ifdef WROOM
    #define SS_PIN 5
    #define MISO_PIN 19

    CC1101 radio(SS_PIN,  MISO_PIN);
#endif

/* The Lilygo TTGO already uses VSPI for the display so we need to define 
   HSPI: https://github.com/Xinyuan-LilyGO/TTGO-T-Display/issues/14 to
   use the additional SPI GPIO pins on the board.
   Lilygo TTGO: SCK_PIN = 25; MISO_PIN = 27; MOSI_PIN = 26; SS_PIN = 33;
*/
#ifdef TTGO
    #define SS_PIN 33
    #define MISO_PIN 27
    #define MOSI_PIN 26
    #define SCK_PIN 25
    
    SPIClass SPITTGO(HSPI);

    CC1101 radio(SS_PIN,  MISO_PIN, SPITTGO);
#endif

//#define GDO0_PIN 2      // Not used
#define MSG_BUFFER_SIZE	(50)


// codes for the various requests and responses
enum { 
    SAVED_TODAY = 0xCA,
    SAVED_YESTERDAY = 0xCB,
    SAVED_LAST_7 = 0xCC,
    SAVED_LAST_28 = 0xCD,
    SAVED_TOTAL = 0xCE
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

//long today, yesterday, last7, last28, total = 0;
  
uint32_t pingTimer;         // used for the periodic pings see below
uint32_t ledTimer;          // used for LED blinking when we receive a packet
uint32_t rxTimer;  
uint8_t txBuf[32];
uint8_t request;
uint8_t addressLQI, rxLQI;  // signal strength test 
byte packet[65];            // The CC1101 library sets the biggest packet at 61
WiFiClient wifiClient;
PubSubClient client(wifiClient);
char msg[MSG_BUFFER_SIZE];

long lastReconnectAttempt = 0;  // Used for non blocking MQTT reconnect
String clientId = "ESP32Client-";


/* Function prototypes */
void connectToMQTTBroker(void);
bool reconnectToMQTTBroker(void);
void radioSetup();
void receivePacket(void);
void transmitPacket(void);


/**
 * @brief Set up everything. SPI, WiFi, MQTT, and CC1101
 * 
 */
void setup() {
    addressLQI = 255;       // set received LQI to lowest value

    iboostInfo.today = 0;
    iboostInfo.yesterday = 0;
    iboostInfo.last7 = 0;
    iboostInfo.last28 = 0;
    iboostInfo.total = 0;
    iboostInfo.addressValid = false;

    #ifdef WROOM
        SPI.begin();
    #endif

    #ifdef TTGO
        SPITTGO.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    #endif

    Serial.begin(115200);
    Serial.println();
    Serial.println("SPI OK");

    Serial.print("Connecting to Wi-Fi...");
    WiFi.begin(SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.print("IP Address:"); 
    Serial.println(WiFi.localIP().toString());

    /* MQTT setup - this will be used to send data to the MQTT broker on a Raspberry 
       Pi so that it can be saved to a database (InfluxDb) for viewing in Grafana and
       for another ESP32 with a display to show the information along with all the 
       solar information it currently displays from the solar invertor.  Aditionally, 
       the state of the water (HOT or not) will be sent to an external web page so 
       we can view it if we're not at home to see if we need to put the hot water on
       or not.
    */
    clientId += String(random(0xffff), HEX);
    client.setServer(MQTT_SERVER, MQTT_PORT);
    connectToMQTTBroker();

    // Set up the radio
    radioSetup();
    
    /* LED setup - so we can use the module without serial terminal,
       set low to start so it's off and flashes when it receives a packet */
    pinMode(LED_BUILTIN, OUTPUT);   
    digitalWrite(LED_BUILTIN, LOW);

    Serial.println("Setup Finished");
}

/**
 * @brief Main loop
 * 
 */
void loop(void) {
    // Ensure we're connected to the MQTT server
    if (!client.connected()) {
        long now = millis();
        if (now - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = now;
            // Attempt to reconnect
            if (reconnectToMQTTBroker()) {
                lastReconnectAttempt = 0;
            }
        }
    } else {
        // MQTT - We're not interested in receiving anything, only publishing.
        client.loop();
    }


    /* Logic to turn on the LED for 200ms without blocking the loop - move to RTOS task? */
    if (millis() - ledTimer > 200)
        digitalWrite(LED_BUILTIN, LOW);
    else
        digitalWrite(LED_BUILTIN, HIGH);

    receivePacket();

    if(iboostInfo.addressValid) {
        if (millis() - pingTimer > 10000) { // ping every 10sec
            if ( (millis() - rxTimer) > 1000 &&  (millis() - rxTimer) < 2000) { // wait between 1-2 seconds after rx to tx
                transmitPacket();
            }
        }
    }
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

    radio.setRXstate();             // Set the current state to RX : listening for RF packets
    Serial.println("CC1101 set up complete, set to Rx");
}

/**
 * @brief Handle the receipt of a packet from the CC1101
 * 
 */
void receivePacket(void) { 
    // Receive part. if GDO0 is connected with D1 you can use it to detect incoming packets
    //if (digitalRead(D1)) {
    byte pkt_size = radio.getPacket(packet);
    if (pkt_size > 0 && radio.crcok()) {        // We have a valid packet with some data
        short heating;
        long p1, p2;
        char pbuf[32];
        byte boostTime;
        bool waterHeating,cylinderHot, batteryOk;

        Serial.print("Frame: ");
        rxTimer = millis();
        rxLQI = radio.getLQI();
        // Print hex values of packet
        for (int i = 0; i < pkt_size; i++) {
            sprintf(pbuf, "%02x", packet[i]);
            Serial.print(pbuf); 
            Serial.print(",");
        }

        Serial.print("len=");
        Serial.print(pkt_size);
        Serial.print(" RSSI="); // for field tests to check the signal strength
        Serial.print(radio.getRSSIdbm());
        Serial.print(" LQI="); // for field tests to check the signal quality
        Serial.println(rxLQI);

        //   buddy request                            sender packet
        if ((packet[2] == 0x21 && pkt_size == 29) || (packet[2] == 0x01 && pkt_size == 44)) {
            if(rxLQI < addressLQI) { // is the signal stronger than the previous/none
                addressLQI = rxLQI;
                iboostInfo.address[0] = packet[0]; // save the address of the packet	0x1c7b; //
                iboostInfo.address[1] = packet[1];
                iboostInfo.addressValid = true;
                Serial.print("Updated the address to:");
                sprintf(pbuf, "%02x,%02x",iboostInfo.address[0],iboostInfo.address[1]);
                Serial.println(pbuf);
            }		
        }

        // main unit (sending info to iBoost Buddy)
        if (packet[2] == 0x22) {    
            heating = (* ( short *) &packet[16]);
            p1 = (* ( long*) &packet[18]);
            p2 = (* ( long*) &packet[25]); // this depends on the request

            if (packet[6])
                waterHeating = false;
            else
                waterHeating = true;

            if (packet[7])
                cylinderHot = true;
            else
                cylinderHot = false;

            if (packet[12])
                batteryOk = false;
            else
                batteryOk = true;

            boostTime=packet[5]; // boost time remaining (minutes)
            Serial.print("Heating: ");
            Serial.print(heating );
            Serial.print("  P1: ");
            Serial.print(p1 );
            Serial.print("  Import: ");
            Serial.print(p1 / 390 );        // was 360
            Serial.print("  P2: ");
            Serial.print(p2 );
            Serial.print("  P3: ");
            Serial.print((* (signed long*) &packet[29]) );
            Serial.print("  P4: ");
            Serial.println((* (signed long*) &packet[30]) );

            switch (packet[24]) {
                case   SAVED_TODAY:
                    iboostInfo.today = p2;
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
                Serial.println("Water Tank HOT");       // Hot water tank is hot!
            else if (boostTime > 0)
                Serial.println("Manual Boost ON");
            else if (waterHeating) {
                Serial.print("Heating by Solar = ");    // How many watts of solar we're using to heat the water
                Serial.println(heating);
            }
            else
                Serial.println("Water Heating OFF");    

            if (batteryOk)
                Serial.println("Warning: Sender Battery OK");
            else
                Serial.println("Sender Battery LOW");

            Serial.print("Today: ");
            Serial.print(iboostInfo.today);
            Serial.print(" Wh   Yesterday: ");
            Serial.print(iboostInfo.yesterday);
            Serial.print(" Wh   Last 7 Days: ");
            Serial.print(iboostInfo.last7);
            Serial.print(" Wh   Last 28 Days: ");
            Serial.print(iboostInfo.last28);
            Serial.print(" Wh   Total: ");
            Serial.print(iboostInfo.total);
            Serial.print(" Wh   Boost Time: ");
            Serial.println(boostTime);

            Serial.println("MQTT publish message: ");

            // How much solar we've used today to heat the hot water
            snprintf (msg, MSG_BUFFER_SIZE, "%ld", iboostInfo.today);                        
            Serial.print("  Saved Today: ");
            Serial.println(msg);
            client.publish("iboost/savedToday", msg);

            // Status of the hot water, is it hot, heating up or off
            if (cylinderHot) {
                snprintf (msg, MSG_BUFFER_SIZE, "HOT");                        
            } else if (waterHeating) {
                snprintf (msg, MSG_BUFFER_SIZE, "%Heating by Solar");                        
            } else {
                snprintf (msg, MSG_BUFFER_SIZE, "Off");                        
            }

            Serial.print("  Hot Water Status: ");
            Serial.println(msg);
            client.publish("iboost/hotWater", msg);

            // Status of the sender battery
            if (batteryOk) {
                snprintf (msg, MSG_BUFFER_SIZE, "OK"); 
            } else {
                snprintf (msg, MSG_BUFFER_SIZE, "LOW"); 
            }

            Serial.print("  Sender Battery: ");
            Serial.println(msg);
            client.publish("iboost/battery", msg);


            //client.publish("iboost/savedYesterday", msg);
            //client.publish("iboost/savedLast7", msg);
            //client.publish("iboost/savedLast28", msg);
            //client.publish("iboost/savedTotal", msg);
        }
        // Update LED timer to flash LED (packet received)
        ledTimer = millis();
    }
}

/**
 * @brief Transmit a packet to the iBoost main unit to request information
 * 
 */
void transmitPacket(void) {
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
    radio.strobe(CC1101_SRX);
    Serial.print("Sent request: ");
    switch (request) {
        case   SAVED_TODAY:
            Serial.println("Saved Today");
            break;
        case   SAVED_YESTERDAY:
            Serial.println("Saved Yesterday");
            break;
        case   SAVED_LAST_7:
            Serial.println("Saved Last 7 Days");
            break;
        case   SAVED_LAST_28:
            Serial.println("Saved Last 28 Days");
            break;
        case   SAVED_TOTAL:
            Serial.println("Saved In Total");
            break;
    }

    request++;

    // Update timer for pinging main unit for information, currently every 10 seconds
    pingTimer = millis();
}

/**
 * @brief Attempt to connect with the MQTT broker
 * 
 */
void connectToMQTTBroker(void) {
    // Loop until we're reconnected
    boolean result = false;

    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (client.connect(clientId.c_str(), MQTT_USER, MQTT_USER_PASSWORD)) {
            Serial.println("connected");
            client.setSocketTimeout(120);

            //client.publish("solar/water", "Connected", false);
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" trying again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

/**
 * @brief Attempt to reconnect with the MQTT broker, non blocking
 * 
 * @return bool True if reconnect was successful, false if not
 */
bool reconnectToMQTTBroker(void) {
    bool connected = false;

    Serial.print("Attempting to reconnect with MQTT server...");
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_USER_PASSWORD)) {
        Serial.println("connected");
        client.setSocketTimeout(120);
        connected = true;
    } 

    return connected;
}
