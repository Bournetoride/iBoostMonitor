/*[  5830][I][main.cpp:781] mqttCallback(): [iBoost] MQTT topic: solar/pvnow, message: 72.0

assert failed: xQueueGenericSend queue.c:832 (pxQueue->pcHead != ((void *)0) || pxQueue->u.xSemaphore.xMutexHolder == ((void *)0) || pxQueue->u.xSemaphore.xMutexHolder == xTaskGetCurrentTaskHandle())

*/


/*
    TFT LCD Touch Monitor Test Program.

    Test to check the monitor (https://www.aliexpress.us/item/1005001999296476.html) works before using in anger.

    Do not forget to check user_setup.h in .pio\libdeps\upesy_wroom\TFT_eSPI folder to set display and pins.
        #define ILI9486_DRIVER
        VSPI
        #define TFT_MOSI 23     // In some display driver board, it might be written as "SDA" and so on.
        #define TFT_MISO 19     // DO NOT CONNECT TO LCD IF USING A TOUCH SCREEN
        #define TFT_SCLK 18
        #define TFT_CS   5      // Chip select control pin
        #define TFT_DC   2      // Data Command control pin
        #define TFT_RST  4      // Reset pin (could connect to Arduino RESET pin)
        #define TFT_BL   3.3v   // LED back-light
        #define TOUCH_CS 21     // Chip select pin (T_CS) of touch screen

        HSPI
        #define TFT_MOSI 13     // In some display driver board, it might be written as "SDA" and so on.
        #define TFT_MISO 12     // DO NOT CONNECT TO LCD IF USING A TOUCH SCREEN
        #define TFT_SCLK 14
        #define TFT_CS   15     // Chip select control pin
        #define TFT_DC   26     // Data Command control pin
        #define TFT_RST  27     // Reset pin (could connect to Arduino RESET pin)
        #define TFT_BL   3.3v   // LED back-light
        #define TOUCH_CS 4      // Chip select pin (T_CS) of touch screen

    Thanks to https://github.com/OscarCalero/TFT_ILI9486/blob/main/Imagenes_SD_y_Touch.ino for his video and
    code to get me started in the right direction.
*/
#include "main.h"
#include "TFT_eSPI.h"
#include "ESP32MQTTClient.h"

/*
    CLOG_ENABLE Needs to be defined before cLog.h is included.  
    
    Using cLog's linked list to hold log messages which will be displayed in the log
    area as required. Can display up to 7 messages each up to 43 characters wide. New
    messages will replace older ones and the screen will then be updated.
*/ 
#define CLOG_ENABLE true
#include "cLog.h"

/*
    VSPI port for ESP32 && TFT ILI9486 480x320 with touch & SD card reader
    LCD         ---->       ESP32 WROOM 32D
    1 Power                 3.3V
    2 Ground                GND
    3 CS (SPI SS)           5
    4 LCD Reset             4 
    5 DC/LCD Bus command    2
    6 LCD MOSI (SPI)        23
    7 LCD SCK (SPI)         18
    8 LCD Backlight         3.3V
    9 LCD MISO              19  (DO NOT CONNECT IF USING TOUCH SCREEN!)
    10 T_CLK (SPI)          18 (same as LCD)
    11 T_CS                 21
    12 T_DIN (SPI MOSI)     23 (same as LCD)
    13 T_DO (SPI MISO)      19 (same as LCD)
    14 T_IRQ                Currently not connected


    HSPI port for ESP32 && TFT ILI9486 480x320 with touch & SD card reader
    LCD         ---->       ESP32 WROOM 32D
    1 Power                 3.3V
    2 Ground                GND
    3 CS (SPI SS)           15
    4 LCD Reset             21 
    5 DC/LCD Bus command    2???
    6 LCD MOSI (SPI)        13
    7 LCD SCK (SPI)         14
    8 LCD Backlight         3.3V
    9 LCD MISO              12  (DO NOT CONNECT IF USING TOUCH SCREEN!)
    10 T_CLK (SPI)          14 (same as LCD)
    11 T_CS                  4
    12 T_DIN (SPI MOSI)     13 (same as LCD)
    13 T_DO (SPI MISO)      12 (same as LCD)
    14 T_IRQ                Currently not connected

*/

// freeRTOS - one task for all the screen activity, for use in iBoost as it's own task.
//

// Logging tag
static const char* TAGS = "LCD";

static TFT_eSPI tft = TFT_eSPI();              // TFT object

static TFT_eSprite lineSprite = TFT_eSprite(&tft);    // Sprite object
static TFT_eSprite rightArrowSprite = TFT_eSprite(&tft);    // Sprite object
static TFT_eSprite leftArrowSprite = TFT_eSprite(&tft);    // Sprite object
static TFT_eSprite fillFrameSprite = TFT_eSprite(&tft);    // Sprite object
static TFT_eSprite logSprite = TFT_eSprite(&tft);    // Sprite object for log area
static TFT_eSprite infoSprite = TFT_eSprite(&tft);    // Sprite object for info area

// TFT specific defines
//#define TOUCH_CS 21             // Touch CS to PIN 21 for VSPI, PIN 4 for HSPI
#define REPEAT_CAL false        // True if calibration is requested after reboot
#define TFT_GREY    0x5AEB
#define TFT_TEAL    0x028A      // RGB 00 80 80
#define TFT_GREEN_ENERGY    0x1d85  // RGB 3 44 5

#define TFT_BACKGROUND 0x3189 // 0x2969 // 0x4a31 // TFT_BLACK
#define TFT_FOREGROUND TFT_WHITE
#define TFT_WATERTANK_HOT TFT_RED
#define TFT_WATERTANK_WARM TFT_PURPLE
#define TFT_WATERTANK_COLD TFT_BLUE

// #define LABEL1_FONT &FreeSansOblique12pt7b  // Key label font 1
// #define LABEL2_FONT &FreeSansBold12pt7b     // Key label font 2

// Screen Saver / Touch
TFT_eSPI_Button key[1];     // TFT_eSPI button class

#define TEXT_HEIGHT 8     // Height of text to be printed and scrolled
#define TEXT_WIDTH 6      // Width of text to be printed and scrolled

#define LINE_HEIGHT 9     // TEXT_HEIGHT + 1
#define COL_WIDTH 8       // TEXT_WIDTH + 2

#define MAX_CHR 35        // characters per line (tft.height() / LINE_HEIGHT);
#define MAX_COL 54        // maximum number of columns (tft.width() / COL_WIDTH);
#define MAX_COL_DOT6 32   // MAX_COL * 0.6

static int col_pos[MAX_COL];
static int chr_map[MAX_COL][MAX_CHR];
static byte color_map[MAX_COL][MAX_CHR];
static uint16_t yPos = 0;
static int rnd_x;
static int rnd_col_pos;
static int color;
//

// Animation
static int sunX = 100;      // Sun x y
static int sunY = 105;
static int gridX = 260;
static int gridY = 105;
static int waterX = 105;
static int waterY = 170;
static int width = 83;    // Width of drawing space minus width of arrow 
static int step = 1;       // How far to move the triangle each iteration
//


// Touchscreen related
static uint16_t t_x = 0, t_y = 0;      // touch screen coordinates
static int xw = tft.width()/2;         // xw, yh are middle of the screen
static int yh = tft.height()/2;
static bool pressed = false;
//

// Clog init
static const uint16_t maxEntries = 7;
static const uint16_t maxEntryChars = 44;
static CLOG_NEW myLog1(maxEntries, maxEntryChars, NO_TRIGGER, WRAP);
static bool updateLogging = false;
//

// Ensure we don't change a value being changed somewhere else.  Needed as we are
// using tasks / callback which could update at the wrong time!
static portMUX_TYPE myMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool update = false;
char dateTimeStringBuff[35];        // buffer for time and date on the display
char timeStringBuff[9] = {0};       // buffer for time on the display
//

// Update solar values flags
static bool updateTotalToday = false;

// structure to hold all solar related information and flags. 
typedef struct {
    int pvNow;                  // Current solar PV generation (Watts)
    int wtNow;                  // Current solar PV being used to heat the water tank (Watts)
    float pvToday;              // Total solar generated today (kW)
    float wtToday;              // Total solar used to heat hot water tank today (Watts)
    int exportNow;              // Value of solar being exported to the grid now (Watts)
    int importNow;              // Value of grid import of electricity (Watts)

    // Flags to control aminmation arrows
    bool solarFlag;             // Electricity is being generated by solar PV
    bool waterTankFlag;         // Solar is being used to heat the hot water tank
    bool importFlag;            // Importing electricity from the grid
    bool exportFlag;            // Exporting electricity to the grid from solar

    // Flags to control if to update solar values on the screen
    bool updatePVNow;           // PV now value has changed
    bool updatePVToday;         // PV today value has changed
    bool updateGrid;            // Grid value has changed, doesn't matter if it is export/import
    bool updateWTNow;           // Water tank PV value has changed
    bool updateWTToday;         // Water tank PV total used today has changed

    int senderBatteryStatus;    // Status of the sender battery in the CT clamp
    int waterTankStatus;        // Water tank status (OFF, Heating by Solar, HOT)
} solar_t;

// Initialise struct and set all flags to false, all other values will default to 0 in C99
solar_t volatile solar = {.solarFlag = false, .waterTankFlag = false, .importFlag = false, .exportFlag = false,
                            .updatePVNow = false, .updatePVToday = false, .updateGrid = false, .updateWTNow = false, 
                            .updateWTToday = false, .senderBatteryStatus = BATTERY_OK, .waterTankStatus = OFF};
//

// Drawing related
enum alignment {
    LEFT,
    RIGHT,
    CENTER
};
//

// Function defenitions
static void initialiseScreen(void);
static void drawButtons(void);
static void showMessage(String msg, int x, int y, int textSize, int font);
static void drawHouse(int x, int y);
static void drawPylon(int x, int y);
static void drawSun(int x, int y);
static void drawWaterTank(int x, int y);
static void animation(void);
static void matrix(void);
static void touch(void);
static void startScreenSaver(void);
static void drawString(int x, int y, String text, alignment align);


static uint32_t inactiveRunTime = -99999;  // inactivity run time timer

static bool screenSaverActive = false;     // Is the screen saver active or not


/**
 * @brief Task to handle all things screen related.
 * 
 */
void displayTask(void *parameter) {
    uint32_t animationRunTime = -99999;  // time for next update
    uint8_t updateAnimation = 50;        // update every 40ms
    uint32_t matrixRunTime = -99999;  // time for next update
    uint8_t updateMatrix = 200;        // update matrix screen saver every 150ms
    uint32_t inactive = 1000 * 60 * 1;  // inactivity of 15 minutes then start screen saver



    // Set all chip selects high to astatic void bus contention during initialisation of each peripheral
    digitalWrite(TOUCH_CS, HIGH);   // ********** TFT_eSPI touch **********
    digitalWrite(TFT_CS, HIGH);     // ********** TFT_eSPI screen library **********

    randomSeed(analogRead(A0));


    tft.init();
    tft.invertDisplay(false); // Required for my LCD TFT screen for color correction

    tft.setRotation(3);
    tft.setSwapBytes(true); // Color bytes are swapped when writing to RAM, this introduces a small overhead but
                            // there is a net performance gain by using swapped bytes.

    // tft.pushImage(75, 75, 320, 170, (uint16_t *)img_logo);

    tft.setTextSize(2);

    // Create the Sprites
    logSprite.createSprite(270, 75);
    logSprite.fillSprite(TFT_BACKGROUND);
    infoSprite.createSprite(150, 75);
    infoSprite.fillSprite(TFT_BACKGROUND);
    
    // Sprites for animations
    lineSprite.createSprite(95, 1);
    rightArrowSprite.createSprite(12, 21);
    leftArrowSprite.createSprite(12, 21);
    fillFrameSprite.createSprite(12, 21);
    lineSprite.fillSprite(TFT_BACKGROUND);
    rightArrowSprite.fillSprite(TFT_BACKGROUND);
    leftArrowSprite.fillSprite(TFT_BACKGROUND);
    fillFrameSprite.fillSprite(TFT_BACKGROUND);

    lineSprite.drawLine(0, 0, 95, 0, TFT_LIGHTGREY);

    rightArrowSprite.fillTriangle(11, 10, 1, 0, 1, 20, TFT_GREEN_ENERGY);  // > small right pointing sideways triangle
    rightArrowSprite.drawPixel(0, 10, TFT_LIGHTGREY);    

    leftArrowSprite.fillTriangle(0, 10, 10, 0, 10, 20, TFT_RED);  // < small left pointing sideways triangle
    leftArrowSprite.drawPixel(11, 10, TFT_LIGHTGREY);    

    fillFrameSprite.drawLine(0, 10, 11, 10, TFT_LIGHTGREY);

    initialiseScreen(); 

    ESP_LOGI(TAGS, "Display Initialisation Complete");

    inactiveRunTime = millis();     // start inactivity timer for turning on the screen saver

    for ( ;; ) {
        if (screenSaverActive) {
            if (millis() - matrixRunTime >= updateMatrix) {  // time has elapsed, update display
                matrixRunTime = millis();
                matrix();
            }
        } else {
            if (solar.updatePVToday) {  
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 45, 4);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                tft.print(solar.pvToday);
                tft.print(" kW   ");

                portENTER_CRITICAL(&myMux);
                solar.updatePVToday = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.updatePVNow) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 85, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                tft.print(solar.pvNow);
                tft.print(" W   ");

                portENTER_CRITICAL(&myMux);
                solar.updatePVNow = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.updateWTNow) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 150, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                tft.print(solar.wtNow);
                tft.print(" W   ");

                portENTER_CRITICAL(&myMux);
                solar.updateWTNow = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.updateWTToday) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 205, 1);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(2);
                tft.printf("%.2f kW", solar.wtToday);
                
                portENTER_CRITICAL(&myMux);
                solar.updateWTToday = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.updateGrid) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(290, 85, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                if (solar.importFlag) {
                    tft.print(solar.importNow);
                } else {
                    tft.print(solar.exportNow);
                }
                tft.print(" W   ");

                portENTER_CRITICAL(&myMux);
                solar.updateGrid = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (updateLogging) {
                logSprite.fillSprite(TFT_BACKGROUND);
                logSprite.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                logSprite.setTextFont(0);

                portENTER_CRITICAL(&myMux);
                for (int i = 0, y = 3; i < myLog1.numEntries; i++, y+=10) {
                    logSprite.setCursor(5, y);
                    logSprite.print(myLog1.get(i));
                }

                logSprite.pushSprite(211, 246);

                updateLogging = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (millis() - animationRunTime >= updateAnimation) {  // time has elapsed, update display
                animationRunTime = millis();
                animation();
            }

            if (millis() >= inactiveRunTime + inactive) {       // We've been inactive for 'n' minutes, start screensaver
                updateLog("No activity, start screen saver");
                startScreenSaver();
            }
        }

        touch();    // has the touch screen been pressed, check each loop or can we add a wait time?

        // 1 second timer for clock on screen
        if (update) {
            // update flag
            portENTER_CRITICAL(&myMux);
            update = false;
            portEXIT_CRITICAL(&myMux);

            updateLocalTime();
            if (!screenSaverActive)
                showMessage(timeStringBuff, 5, 250, 1, 2);
        }

        vTaskDelay(30 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}


/**
 * @brief Animation of arrows to show the flow of electricity. Solar generation, water tank
 * heating, grid import or export.
 * 
 */
static void animation(void) {
    static int sunStartPosition = sunX;       // 
    static int gridImportStartPosition = gridX;     //
    static int gridExportStartPosition = gridX;     //
    static int waterStartPosition = waterX;     //
    static int sunArrow = sunStartPosition + 40;                // solar generation arrow start point 
    static int gridImportArrow = gridImportStartPosition + width;      // grid import arrow start point
    static int gridExportArrow = gridExportStartPosition;      // grid export arrow start point
    static int waterArrow = waterStartPosition + 15;            // water heating arrow start point - move so it's not the same position as sum

    // Solar generation arrow
    if (solar.solarFlag) {
        rightArrowSprite.pushSprite(sunArrow, sunY);
        sunArrow += step;
        if (sunArrow > (width + sunStartPosition)) {
            fillFrameSprite.pushSprite(sunArrow-step, sunY);
            sunArrow = sunStartPosition;
        }         
    }

    // Grid import arrow
    if (solar.importFlag) {
        leftArrowSprite.pushSprite(gridImportArrow, gridY);
        gridImportArrow -= step;
        if (gridImportArrow < gridImportStartPosition) {
            fillFrameSprite.pushSprite(gridImportArrow+step, gridY);
            gridImportArrow = gridImportStartPosition + width;
        } 
    }

    // Grid export arrow
    if (solar.exportFlag) {
        rightArrowSprite.pushSprite(gridExportArrow, gridY);
        gridExportArrow += step;
        if (gridExportArrow > gridExportStartPosition + width) {
            fillFrameSprite.pushSprite(gridExportArrow-step, gridY);
            gridExportArrow = gridExportStartPosition;
        } 
    }

    // Water tank heating by solar arrow
    if (solar.waterTankFlag) {
        rightArrowSprite.pushSprite(waterArrow, waterY);
        waterArrow += step;
        if (waterArrow > waterStartPosition + width) {
            fillFrameSprite.pushSprite(waterArrow-step, waterY);
            waterArrow = waterStartPosition;
        } 
    }
}

/**
 * @brief Touch screen has been touched!
 * 
 */
static void touch(void) {
    if (tft.getTouch(&t_x, &t_y))
        pressed = true;

    if (pressed) {
        pressed = false;
        key[0].press(true);
    }

    if (key[0].justReleased()) {
        key[0].press(false);
        if (!screenSaverActive) {
            ESP_LOGI(TAGS, "Screen saver started by user");
            updateLog("Screen saver started by user");
            startScreenSaver();
        } else if (screenSaverActive) {
            ESP_LOGI(TAGS, "Stop screen saver");
            screenSaverActive = false;
            inactiveRunTime = millis();     // reset inactivity timer to now
            initialiseScreen();
            updateLog("Screen saver stopped by user");
        }
    }

    if (key[0].justPressed()) {
        key[0].press(false);
        vTaskDelay(10 / portTICK_PERIOD_MS); // debounce
    }
}

/**
 * @brief Matrix style screen saver.
 * 
 */
static void matrix(void) {

    for (int j = 0; j < MAX_COL; j++) {
        rnd_col_pos = random(1, MAX_COL);

        rnd_x = rnd_col_pos * COL_WIDTH;

        col_pos[rnd_col_pos - 1] = rnd_x; // save position

        for (int i = 0; i < MAX_CHR; i++) { 
            // 4 = blue
            // 5 = green
            // 10 = green/orange
            // 14 = red

            tft.setTextColor(color_map[rnd_col_pos][i] << 4, TFT_BLACK); // Set the character colour/brightness

            if (color_map[rnd_col_pos][i] == 63) {
                tft.setTextColor(TFT_DARKGREY, TFT_BLACK); // Draw different colour character
            }

            if ((chr_map[rnd_col_pos][i] == 0) || (color_map[rnd_col_pos][i] == 63)) {
                chr_map[rnd_col_pos][i] = random(31, 128);

                if (i > 1) {
                    chr_map[rnd_col_pos][i - 1] = chr_map[rnd_col_pos][i];
                    chr_map[rnd_col_pos][i - 2] = chr_map[rnd_col_pos][i];
                }
            }

            yPos += LINE_HEIGHT;

            tft.drawChar(chr_map[rnd_col_pos][i], rnd_x, yPos, 1); // Draw the character

        }

        yPos = 0;

        for (int n = 0; n < MAX_CHR-1; n++) {   // added -1 so we don't get undefinded behaviour from next line
            chr_map[rnd_col_pos][n] = chr_map[rnd_col_pos][n + 1];   // compiler doesn't like this line
        }
        
        for (int n = MAX_CHR; n > 0; n--) {
            color_map[rnd_col_pos][n] = color_map[rnd_col_pos][n - 1];
        }

        chr_map[rnd_col_pos][0] = 0;

        if (color_map[rnd_col_pos][0] > 20) {
            color_map[rnd_col_pos][0] -= 3; // Rapid fade initially brightness values
        }

        if (color_map[rnd_col_pos][0] > 0) {
            color_map[rnd_col_pos][0] -= 1; // Slow fade later
        }

        if ((random(20) == 1) && (j < MAX_COL_DOT6)) { // MAX_COL * 0.6
            color_map[rnd_col_pos][0] = 63; // ~1 in 20 probability of a new character
        }
    }        
}

/**
 * @brief Set up the screen. This will be called at program startup and when the screen
 * saver ends.  This will draw all static elements, i.e. house, sun, pylon, hot water
 * tank, menu buttons etc.
 */
static void initialiseScreen(void) {
    tft.fillScreen(TFT_BACKGROUND);

    // Define area at top of screen for date, time etc.
    tft.fillRect(0, 20, 480, 2, TFT_BLACK);
    tft.fillRect(0, 0, 480, 20, TFT_SKYBLUE);

    // Define message area at the bottom
    tft.drawLine(0, 245, 480, 245, TFT_FOREGROUND);
    tft.drawLine(210, 245, 210, 320, TFT_FOREGROUND);

    drawSun(65, 145);
    drawHouse(210, 130);
    drawPylon(380, 130);
    drawWaterTank(213, 160);

    lineSprite.pushSprite(sunX, sunY+10);
    lineSprite.pushSprite(gridX, gridY+10);
    lineSprite.pushSprite(waterX, waterY+10);

    tft.setCursor(75, 3, 1);   // position and font
    tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
    tft.setTextSize(2);
    tft.print("House Electricity Monitor");

    logSprite.pushSprite(211, 246);

    //showMessage("13:43:23", 5, 250, 1, 2);
    showMessage("Sun 17 Mar 24", 110, 250, 1, 2);

    showMessage("Water Tank: Heating by solar", 5, 270, 1, 2);
    showMessage("Sender Battery: OK", 5, 288, 1, 2);

    showMessage("IP: 192.168.5.67", 5, 310, 0, 1);
    showMessage("LQI: 23", 160, 310, 0, 1);

    // Demo values

    // Solar generation now
    // tft.setCursor(110, 85, 2);   // position and font
    // tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    // tft.setTextSize(1);
    // tft.print("2.34 kW");

    // // Electricity import/export values
    // tft.setCursor(280, 85, 2);   // position and font
    // tft.print("1.67 kW");

    // // Total solar generated today
    // // tft.setCursor(100, 45, 4);   // position and font
    // // tft.setTextSize(1);
    // // tft.print("12.67 kWh");

    // // Water import to heat water
    // tft.setCursor(110, 150, 2);   // position and font
    // tft.setTextSize(1);
    // tft.print("0.89 kW");

    // // Total saved today to heat water
    // tft.setCursor(110, 205, 1);   // position and font
    // tft.setTextSize(2);
    // tft.print("2.57 kWh");

}

/**
 * @brief Start/setup the screen saver.  Will be started by the user touching the screen
 * or after 'n' minutes of inactivity to save the screen from burn-in.
 */
static void startScreenSaver(void) {
    screenSaverActive = true;
            
    tft.fillScreen(TFT_BLACK);

    for (int j = 0; j < MAX_COL; j++) {
        for (int i = 0; i < MAX_CHR; i++) {
            chr_map[j][i] = 0;
            color_map[j][i] = 0;
        }

        color_map[j][0] = 63;
    }
}

/**
 * @brief Show a message on the screen, mainly used for time, date and mainly fixed
 * information that does not change a lot (except the time obviously!).
 * 
 * @param msg String to write to the display
 * @param x Pixel location horizontal
 * @param y Pixel location vertical
 * @param textSize Text size to use
 * @param font Default font to use, 1, 2 etc.
 */
static void showMessage(String msg, int x, int y, int textSize, int font) {
    tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    tft.setCursor(x, y, font);   // position and font
    tft.setTextSize(textSize);
    tft.print(msg);
}

/**
 * @brief add item to clog
 * 
 */
void updateLog(const char *msg) {
    portENTER_CRITICAL(&myMux);
    
    // Add time to message then add to CLOG
    CLOG(myLog1.add(), "%s %s", timeStringBuff, msg);
    
    updateLogging = true;
    portEXIT_CRITICAL(&myMux);
}

void printLogging(void) {
    int y = 3; // top of log area

    logSprite.fillSprite(TFT_BACKGROUND);
    logSprite.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    logSprite.setTextFont(0);
    for (uint8_t i = 0; i < myLog1.numEntries; i++, y+=10) {
        logSprite.setCursor(5, y);
        logSprite.print(myLog1.get(i));
    }

    logSprite.pushSprite(211, 246);
    portENTER_CRITICAL(&myMux);
    updateLogging = true;
    portEXIT_CRITICAL(&myMux);

}

/**
 * @brief Draw a house where xy is the bottom left of the house
 * 
 * @param x Bottom left x of house
 * @param y Bottom left y of house
 */
static void drawHouse(int x, int y) {
    tft.drawLine(x, y, x+36, y, TFT_FOREGROUND);      // Bottom
    tft.drawLine(x, y, x, y-30, TFT_FOREGROUND);      // Left wall
    tft.drawLine(x+36, y, x+36, y-30, TFT_FOREGROUND);      // Right wall
    tft.drawLine(x-2, y-28, x+18, y-45, TFT_FOREGROUND);      // Left angled roof
    tft.drawLine(x+38, y-28, x+18, y-45, TFT_FOREGROUND);      // Right angled roof

    tft.drawRect(x+5, y-28, 8, 8, TFT_FOREGROUND);   // Left top window
    tft.drawRect(x+23, y-28, 8, 8, TFT_FOREGROUND);   // Right top window
   
    tft.drawRect(x+15, y-13, 8, 13, TFT_FOREGROUND);   // Door
}

/**
 * @brief Draw an electricity pylon
 * 
 * @param x Bottom left x position of pylon
 * @param y Bottom left y position of pylon
 */
static void drawPylon(int x, int y) {
    tft.drawLine(x, y, x+5, y-25, TFT_FOREGROUND);      // left foot
    tft.drawLine(x+5, y-25, x+5, y-40, TFT_FOREGROUND);      // left straight
    tft.drawLine(x+5, y-40, x+10, y-50, TFT_FOREGROUND);      // left top angle

    tft.drawLine(x+20, y, x+15, y-25, TFT_FOREGROUND);      // right foot
    tft.drawLine(x+15, y-25, x+15, y-40, TFT_FOREGROUND);      // right straight
    tft.drawLine(x+15, y-40, x+10, y-50, TFT_FOREGROUND);      // right top angle

    // lines across starting at bottom
    tft.drawLine(x+1, y-5, x+19, y-5, TFT_FOREGROUND);      
    tft.drawLine(x+3, y-15, x+18, y-15, TFT_FOREGROUND);  

    tft.drawLine(x-5, y-25, x+25, y-25, TFT_FOREGROUND);    // bottom wider line across
    tft.drawLine(x+5, y-30, x+15, y-30, TFT_FOREGROUND);
    tft.drawLine(x-5, y-25, x+5, y-30, TFT_FOREGROUND);    // angle left
    tft.drawLine(x+25, y-25, x+15, y-30, TFT_FOREGROUND);    // angle right

    tft.drawLine(x-5, y-35, x+25, y-35, TFT_FOREGROUND);    // top wider line across
    tft.drawLine(x+5, y-40, x+15, y-40, TFT_FOREGROUND);
    tft.drawLine(x-5, y-35, x+5, y-40, TFT_FOREGROUND);    // angle left
    tft.drawLine(x+25, y-35, x+15, y-40, TFT_FOREGROUND);    // angle right

    // cross sections starting at bottom
    tft.drawLine(x+3, y-5, x+18, y-15, TFT_FOREGROUND);
    tft.drawLine(x+18, y-5, x+3, y-15, TFT_FOREGROUND);

    tft.drawLine(x+3, y-15, x+15, y-25, TFT_FOREGROUND);
    tft.drawLine(x+18, y-15, x+5, y-25, TFT_FOREGROUND);

    tft.drawLine(x+5, y-25, x+15, y-30, TFT_FOREGROUND);
    tft.drawLine(x+15, y-25, x+5, y-30, TFT_FOREGROUND);

    tft.drawLine(x+5, y-30, x+15, y-35, TFT_FOREGROUND);
    tft.drawLine(x+15, y-30, x+5, y-35, TFT_FOREGROUND);

    tft.drawLine(x+5, y-35, x+15, y-40, TFT_FOREGROUND);
    tft.drawLine(x+15, y-35, x+5, y-40, TFT_FOREGROUND);

    // dots at end of pylon
    tft.drawLine(x-5, y-34, x-5, y-33, TFT_FOREGROUND); // top left
    tft.drawLine(x+25, y-34, x+25, y-33, TFT_FOREGROUND); // top right
    tft.drawLine(x-5, y-24, x-5, y-23, TFT_FOREGROUND); // bottom left
    tft.drawLine(x+25, y-24, x+25, y-23, TFT_FOREGROUND); // bottom right
}

/**
 * @brief Display the sun
 * 
 * @param x Display x coordinates
 * @param y Display y coordinates
 */
static void drawSun(int x, int y) {
    int scale = 12;  // 6

    int linesize = 3;
    int dxo, dyo, dxi, dyi;

    tft.fillCircle(x, y, scale, TFT_RED);

    for (float i = 0; i < 360; i = i + 45) {
        dxo = 2.2 * scale * cos((i - 90) * 3.14 / 180);
        dxi = dxo * 0.6;
        dyo = 2.2 * scale * sin((i - 90) * 3.14 / 180);
        dyi = dyo * 0.6;
        if (i == 0 || i == 180) {
            tft.drawLine(dxo + x - 1, dyo + y, dxi + x - 1, dyi + y, TFT_RED);
            tft.drawLine(dxo + x + 0, dyo + y, dxi + x + 0, dyi + y, TFT_RED);
            tft.drawLine(dxo + x + 1, dyo + y, dxi + x + 1, dyi + y, TFT_RED);
        }
        if (i == 90 || i == 270) {
            tft.drawLine(dxo + x, dyo + y - 1, dxi + x, dyi + y - 1, TFT_RED);
            tft.drawLine(dxo + x, dyo + y + 0, dxi + x, dyi + y + 0, TFT_RED);
            tft.drawLine(dxo + x, dyo + y + 1, dxi + x, dyi + y + 1, TFT_RED);
        }
        if (i == 45 || i == 135 || i == 225 || i == 315) {
            tft.drawLine(dxo + x - 1, dyo + y, dxi + x - 1, dyi + y, TFT_RED);
            tft.drawLine(dxo + x + 0, dyo + y, dxi + x + 0, dyi + y, TFT_RED);
            tft.drawLine(dxo + x + 1, dyo + y, dxi + x + 1, dyi + y, TFT_RED);
        }
    }
}

static void drawWaterTank(int x, int y) {
//350, 160
    tft.drawRoundRect(x, y, 22, 33, 6, TFT_FOREGROUND);
    tft.fillRoundRect(x+1, y+1, 20, 31, 6, TFT_WATERTANK_HOT);

    // shower hose
    tft.drawLine(x+11, y, x+11, y-5, TFT_FOREGROUND);
    tft.drawLine(x+11, y-5, x+35, y-5, TFT_FOREGROUND);
    tft.drawLine(x+35, y-5, x+35, y+5, TFT_FOREGROUND);
    tft.drawLine(x+30, y+6, x+40, y+6, TFT_FOREGROUND);
    tft.drawLine(x+31, y+7, x+39, y+7, TFT_FOREGROUND);

    // water
    tft.drawLine(x+31, y+8, x+27, y+15, TFT_WATERTANK_HOT); // left
    tft.drawLine(x+33, y+8, x+30, y+15, TFT_WATERTANK_HOT); // left

    tft.drawLine(x+35, y+8, x+35, y+15, TFT_WATERTANK_HOT); // middle

    tft.drawLine(x+37, y+8, x+39, y+15, TFT_WATERTANK_HOT); // right
    tft.drawLine(x+39, y+8, x+42, y+15, TFT_WATERTANK_HOT); // right
}

void updateLocalTime(void) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

  // Update buffer with current time
  strftime(dateTimeStringBuff, sizeof(dateTimeStringBuff), "%H:%M:%S %a %b %d %Y", &timeinfo);
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
}

/**
 * @brief Set the Solar Generation Flag object
 * 
 * @param setting Bool value to set flag, true solar generation
 */
void setSolarGenerationFlag(bool setting) {
    if (solar.solarFlag != setting) {
        solar.solarFlag = setting;
    }
}

/**
 * @brief Set the Grid Import Flag object. If importing is true, exporting
 * has to be false.
 * 
 * @param setting Bool value to set flag, true importing from grid
 */
void setGridImportFlag(bool setting) {
    if (solar.importFlag != setting) {
        solar.importFlag = setting;
        if (solar.importFlag) {           // can't import and export at the same time
            solar.exportFlag = false;
            updateLog("Importing from the grid");
        }
    }
}

/**
 * @brief Set the Grid Export Flag object. If exporting is true, importing has 
 * to be false.
 * 
 * @param setting Bool value to set flag, true if exporting to grid
 */
void setGridExportFlag(bool setting) {
    if (solar.exportFlag != setting) {
        solar.exportFlag = setting;
        if (solar.exportFlag) {           // can't import and export at the same time
            solar.importFlag = false;
            updateLog("Exporting to the grid");
        }
    }
}

/**
 * @brief Set the Water Tank Flag object. 
 *
 * @param setting Bool value to set flag, true if heating water tank via solar
 */
void setWaterTankFlag(bool setting) {
    if (solar.waterTankFlag != setting)
        solar.waterTankFlag = setting;
}

/**
 * @brief Set pvNow value - current solar (watts) being generated
 * 
 * @param pv Current value of PV (Watts)
 */
void setPVNow(int pv) {
    portENTER_CRITICAL(&myMux);
    solar.pvNow = pv;
    solar.updatePVNow = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set the Total Today object
 * 
 * @param total Total value of PV generated today (kW)
 */
void setPVToday(float total) {
    portENTER_CRITICAL(&myMux);
    solar.pvToday = total;
    solar.updatePVToday = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set wtNow object
 * 
 * @param pv Current valaue of PV being used now to heat water tank
*/
void setWTNow(int pv) {
    portENTER_CRITICAL(&myMux);
    solar.wtNow = pv;
    solar.updateWTNow = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set the Total used to heat the how water tank object
 * 
 * @param total Total value of PV used today to heat the water tank (Watts)
 */
void setWTToday(int total) {
    portENTER_CRITICAL(&myMux);
    if (total > 0) {
        solar.wtToday = (float) total / 1000; //(float) temp;  // round to nearest value, 2 dicmal points
    } else {
        solar.wtToday = 0.00;
    }
    solar.updateWTToday = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set Export Now object
 * 
 * @param pv Current valaue of PV being exported to the grid
*/
void setExportNow(int pv) {
    portENTER_CRITICAL(&myMux);
    solar.exportNow = pv;
    solar.updateGrid = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set Import Now object
 * 
 * @param pv Current valaue of electricity being imported from the grid
*/
void setImportNow(int grid) {
    portENTER_CRITICAL(&myMux);
    solar.importNow = grid;
    solar.updateGrid = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Draw a string to the screen - main function for all text written to the display
 * 
 * @param x Display x coordinates
 * @param y Display y coordinates
 * @param text Test to display
 * @param align Text alignment on the screen
 */
static void drawString(int x, int y, String text, alignment align) {
    int16_t x1, y1; // the bounds of x,y and w and h of the variable 'text' in pixels.
    int w = 448;
    int h = 320;

    tft.setTextWrap(false);
    if (align == RIGHT) {
        x = x - w;
    }

    if (align == CENTER) {
        x = x - w / 2;
    }

    tft.setCursor(x, y + h);
    tft.print(text);
}


/*
[731172][I][main.cpp:786] mqttCallback(): [iBoost] MQTT topic: solar/pvnow, message: 266.0
[731706][I][main.cpp:786] mqttCallback(): [iBoost] MQTT topic: solar/pvtotal, message: 7.5
[733185][I][main.cpp:428] receivePacketTask(): [iBoost] Heating: 0 Watts  P1: 598323  Importing: 1534 Watts  P2: 21701
[733193][I][main.cpp:465] receivePacketTask(): [iBoost] Heating by Solar = 0 Watts
[733201][I][main.cpp:472] receivePacketTask(): [iBoost] Sender Battery OK
[733207][I][main.cpp:477] receivePacketTask(): [iBoost] Today: 2583 Wh   Yesterday: 3388 Wh   Last 7 Days: 21701 Wh   Last 28 Days: 74012 Wh   Total: 1902312 Wh   Boost Time: 0
[733225][I][main.cpp:503] receivePacketTask(): [iBoost] Published MQTT message: {"savedToday":2583,"hotWater":"Heating by Solar","battery":"OK"}
[790468][I][main.cpp:786] mqttCallback(): [iBoost] MQTT topic: solar/pvnow, message: 266.0
[791000][I][main.cpp:786] mqttCallback(): [iBoost] MQTT topic: solar/pvtotal, message: 7.5
*/