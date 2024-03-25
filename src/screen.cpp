/*[  5830][I][main.cpp:781] mqtt_callback(): [iBoost] MQTT topic: solar/pv_now, message: 72.0

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
//#include "ESP32MQTTClient.h"
#include "my_ringbuf.h"

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
static TFT_eSprite green_dot_sprite = TFT_eSprite(&tft);
static TFT_eSprite clear_dot_sprite = TFT_eSprite(&tft);
static TFT_eSprite import_dot_sprite = TFT_eSprite(&tft);

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
static int sun_x = 100;      // Sun x y
static int sun_y = 105;
static int grid_x = 260;
static int grid_y = 105;
static int water_x = 105;
static int water_y = 170;
static int width = 83;    // Width of drawing space minus width of arrow 
static int step = 1;       // How far to move the triangle each iteration
//

extern QueueHandle_t g_main_queue;

// Touchscreen related
static uint16_t t_x = 0, t_y = 0;      // touch screen coordinates
static int xw = tft.width()/2;         // xw, yh are middle of the screen
static int yh = tft.height()/2;
static bool b_touch_screen_pressed = false;
//

// Clog init
static const uint16_t max_entries = 7;
static const uint16_t max_entry_chars = 44;
static CLOG_NEW myLog1(max_entries, max_entry_chars, NO_TRIGGER, WRAP);
static bool b_update_logging = false;
//

// Ensure we don't change a value being changed somewhere else.  Needed as we are
// using tasks / callback which could update at the wrong time!
static portMUX_TYPE myMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool update = false;
static char date_time_buffer[35];        // buffer for time and date on the display
static char time_buffer[9] = {0};       // buffer for time on the display
//

// Update solar values flags
static bool b_update_total_today = false;

// structure to hold all solar related information and flags. 
typedef struct {
    int pv_now;                  // Current solar PV generation (Watts)
    int wt_now;                  // Current solar PV being used to heat the water tank (Watts)
    float pv_today;              // Total solar generated today (kW)
    float wt_today;              // Total solar used to heat hot water tank today (Watts)
    int export_now;              // Value of solar being exported to the grid now (Watts)
    int import_now;              // Value of grid import of electricity (Watts)

    // Flags to control aminmation arrows
    bool b_solar_flag;           // Electricity is being generated by solar PV
    bool b_water_tank_flag;      // Solar is being used to heat the hot water tank
    bool b_import_flag;          // Importing electricity from the grid
    bool b_export_flag;          // Exporting electricity to the grid from solar

    // Flags to control if to update solar values on the screen
    bool b_update_pv_now;        // PV now value has changed
    bool b_update_pv_today;      // PV today value has changed
    bool b_update_grid;          // Grid value has changed, doesn't matter if it is export/import
    bool b_update_wt_now;        // Water tank PV value has changed
    bool b_update_wt_today;      // Water tank PV total used today has changed

    int sender_battery_status;   // Status of the sender battery in the CT clamp
    int water_tank_status;       // Water tank status (OFF, Heating by Solar, HOT)
} solar_t;

// Initialise struct and set all flags to false, all other values will default to 0 in C99
solar_t volatile solar = {.b_solar_flag = false, .b_water_tank_flag = false, .b_import_flag = false, .b_export_flag = false,
                            .b_update_pv_now = false, .b_update_pv_today = false, .b_update_grid = false, .b_update_wt_now = false, 
                            .b_update_wt_today = false, .sender_battery_status = IB_BATTERY_LOW, .water_tank_status = IB_WT_OFF};
//

// Drawing related
enum alignment {
    LEFT,
    RIGHT,
    CENTER
};
//

// Function defenitions
static void initialise_screen(void);
static void draw_buttons(void);
static void show_message(String msg, int x, int y, int textSize, int font);
static void draw_house(int x, int y);
static void draw_pylon(int x, int y);
static void draw_sun(int x, int y);
static void draw_water_tank(int x, int y);
static void fill_water_tank(int temperature);
static void animation(void);
static void matrix(void);
static void touch(void);
static void setup_screen_saver(void);
static void draw_string(int x, int y, String text, alignment align);
static void electricity_event_handler(void);


static uint32_t inactiveRunTime = -99999;  // inactivity run time timer

static bool b_screen_saver_is_active = false;     // Is the screen saver active or not


/**
 * @brief Task to handle all things screen related.
 * 
 */
void display_task(void *parameter) {
    uint32_t check_animation = -99999;  // time for next update
    uint8_t update_animation = 50;        // update every 40ms
    uint32_t check_matrix = -99999;    // time for next update
    uint8_t update_matrix = 200;         // update matrix screen saver every 150ms
    uint32_t inactive = 1000 * 60 * 15;  // inactivity of 15 minutes then start screen saver
    uint32_t check_one_second = 0;            // one second counter

    char *item = NULL;
    size_t item_size;
    char tx_item[50];
    UBaseType_t res = pdFALSE;

        
    memset(tx_item, '\0', sizeof(tx_item));

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

    green_dot_sprite.createSprite(10, 10);
    green_dot_sprite.fillSprite(TFT_BACKGROUND);
    green_dot_sprite.drawLine(0, 5, 9, 5, TFT_LIGHTGREY);
    green_dot_sprite.fillSmoothCircle(5, 5, 4, TFT_GREEN_ENERGY);

    import_dot_sprite.createSprite(10, 10);
    import_dot_sprite.fillSprite(TFT_BACKGROUND);
    import_dot_sprite.drawLine(0, 5, 9, 5, TFT_LIGHTGREY);
    import_dot_sprite.fillSmoothCircle(5, 5, 4, TFT_RED);

    clear_dot_sprite.createSprite(10, 10);
    clear_dot_sprite.fillSprite(TFT_BACKGROUND);
    clear_dot_sprite.drawLine(0, 5, 5, 5, TFT_LIGHTGREY);

    lineSprite.drawLine(0, 0, 95, 0, TFT_LIGHTGREY);

    rightArrowSprite.fillTriangle(11, 10, 1, 0, 1, 20, TFT_GREEN_ENERGY);  // > small right pointing sideways triangle
    rightArrowSprite.drawPixel(0, 10, TFT_LIGHTGREY);    

    leftArrowSprite.fillTriangle(0, 10, 10, 0, 10, 20, TFT_RED);  // < small left pointing sideways triangle
    leftArrowSprite.drawPixel(11, 10, TFT_LIGHTGREY);    

    fillFrameSprite.drawLine(0, 10, 11, 10, TFT_LIGHTGREY);

    initialise_screen(); 

    // green_dot_sprite.pushSprite(30, 30);
    // import_dot_sprite.pushSprite(30, 50);

    ESP_LOGI(TAGS, "Display Initialisation Complete");

    inactiveRunTime = millis();     // start inactivity timer for turning on the screen saver

    for ( ;; ) {
        if (b_screen_saver_is_active) {
            if (millis() - check_matrix >= update_matrix) {  // time has elapsed, update display
                check_matrix = millis();
                matrix();
            }
        } else {
            if (solar.b_update_pv_today) {  
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 45, 4);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                tft.print(solar.pv_today);
                tft.print(" kW   ");

                portENTER_CRITICAL(&myMux);
                solar.b_update_pv_today = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.b_update_pv_now) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 85, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                tft.print(solar.pv_now);
                tft.print(" W   ");

                portENTER_CRITICAL(&myMux);
                solar.b_update_pv_now = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.b_update_wt_now) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 150, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                tft.print(solar.wt_now);
                tft.print(" W   ");

                portENTER_CRITICAL(&myMux);
                solar.b_update_wt_now = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.b_update_wt_today) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(110, 205, 1);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(2);
                tft.printf("%.2f kW", solar.wt_today);
                
                portENTER_CRITICAL(&myMux);
                solar.b_update_wt_today = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (solar.b_update_grid) {
                // for testing only, need to move to own function and a sprite
                tft.setCursor(290, 85, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                if (solar.b_import_flag) {
                    tft.print(solar.import_now);
                } else {
                    tft.print(solar.export_now);
                }
                tft.print(" W   ");

                portENTER_CRITICAL(&myMux);
                solar.b_update_grid = false;
                portEXIT_CRITICAL(&myMux);
            }

            if (b_update_logging) {
                logSprite.fillSprite(TFT_BACKGROUND);
                logSprite.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                logSprite.setTextFont(0);

                //portENTER_CRITICAL(&myMux);
                for (int i = 0, y = 3; i < myLog1.numEntries; i++, y+=10) {
                    logSprite.setCursor(5, y);
                    logSprite.print(myLog1.get(i));
                }

                logSprite.pushSprite(211, 246);

                b_update_logging = false;
                //portEXIT_CRITICAL(&myMux);
            }

            if (millis() - check_animation >= update_animation) {  // time has elapsed, update display
                check_animation = millis();
                animation();
            }

            if (millis() >= inactiveRunTime + inactive) {       // We've been inactive for 'n' minutes, start screensaver
//                updateLog("No activity, start screen saver");
                strcpy(tx_item, "No activity, start screen saver");
                res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
                if (res != pdTRUE) {
                    ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                } 
                setup_screen_saver();
            }
        }

        touch();    // has the touch screen been pressed, check each loop or can we add a wait time?

        if (millis() - check_one_second >= 1000) {  // update time every second
            check_one_second = millis();

            update_local_time();
            if (!b_screen_saver_is_active)
                show_message(time_buffer, 5, 250, 1, 2);
        }

        //Receive an item from no-split ring buffer - used for logging purposes
        item = (char *)xRingbufferReceive(buf_handle, &item_size, pdMS_TO_TICKS(0));
        if (item != NULL) {
            ESP_LOGI(TAGS, "Retrieved item from ringbuffer");

            // Add time to message then add to CLOG
            CLOG(myLog1.add(), "%s %s", time_buffer, item);
            b_update_logging = true;   // set flag so log is redrawn

            //Return Item
            vRingbufferReturnItem(buf_handle, (void *)item);
        } 

        electricity_event_handler();

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
    static int sunStartPosition = sun_x;       // 
    static int gridImportStartPosition = grid_x;     //
    static int gridExportStartPosition = grid_x;     //
    static int waterStartPosition = water_x;     //
    static int sunArrow = sunStartPosition + 40;                // solar generation arrow start point 
    static int gridImportArrow = gridImportStartPosition + width;      // grid import arrow start point
    static int gridExportArrow = gridExportStartPosition;      // grid export arrow start point
    static int waterArrow = waterStartPosition + 15;            // water heating arrow start point - move so it's not the same position as sum

    // Solar generation arrow
    if (solar.b_solar_flag) {
        rightArrowSprite.pushSprite(sunArrow, sun_y);
        sunArrow += step;
        if (sunArrow > (width + sunStartPosition)) {
            fillFrameSprite.pushSprite(sunArrow-step, sun_y);
            sunArrow = sunStartPosition;
        }         
        // green_dot_sprite.pushSprite(sunArrow, sun_y+5);
        // sunArrow += step;
        // if (sunArrow > ((width + 5) + sunStartPosition)) {
        //     clear_dot_sprite.pushSprite(sunArrow-(step), sun_y+5);
        //     sunArrow = sunStartPosition;
        // }         
    }

    // Grid import arrow
    if (solar.b_import_flag) {
        leftArrowSprite.pushSprite(gridImportArrow, grid_y);
        gridImportArrow -= step;
        if (gridImportArrow < gridImportStartPosition) {
            fillFrameSprite.pushSprite(gridImportArrow+step, grid_y);
            gridImportArrow = gridImportStartPosition + width;
        } 
    }

    // Grid export arrow
    if (solar.b_export_flag) {
        rightArrowSprite.pushSprite(gridExportArrow, grid_y);
        gridExportArrow += step;
        if (gridExportArrow > gridExportStartPosition + width) {
            fillFrameSprite.pushSprite(gridExportArrow-step, grid_y);
            gridExportArrow = gridExportStartPosition;
        } 
    }

    // Water tank heating by solar arrow
    if (solar.b_water_tank_flag) {
        rightArrowSprite.pushSprite(waterArrow, water_y);
        waterArrow += step;
        if (waterArrow > waterStartPosition + width) {
            fillFrameSprite.pushSprite(waterArrow-step, water_y);
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
        b_touch_screen_pressed = true;

    if (b_touch_screen_pressed) {
        b_touch_screen_pressed = false;
        key[0].press(true);
    }

    if (key[0].justReleased()) {
        key[0].press(false);
        if (!b_screen_saver_is_active) {
            ESP_LOGI(TAGS, "Screen saver started by user");
            // updateLog("Screen saver started by user");
            char tx_item[] = "Screen saver started by user";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
            } 
            
            setup_screen_saver();
        } else if (b_screen_saver_is_active) {
            ESP_LOGI(TAGS, "Stop screen saver");
            b_screen_saver_is_active = false;
            inactiveRunTime = millis();     // reset inactivity timer to now
            initialise_screen();
            //updateLog("Screen saver stopped by user");
            char tx_item[] = "Screen saver stopped by user";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
            } 
        }
    }

    if (key[0].justPressed()) {
        key[0].press(false);
        vTaskDelay(10 / portTICK_PERIOD_MS); // debounce
    }
}/**
 * @brief An event has occured which we need to action.  The event can be an iBoost message or
 * an item we have received from the MQTT queue.
 * 
 */
static void electricity_event_handler(void) {
    electricity_event_t electricity_event;
    float watts;
    ib_info_t info;
    char tx_item[50];

    // Receive information from other tasks to display on the screen
    if (xQueueReceive(g_main_queue, &electricity_event, (TickType_t)0) == pdPASS) {
        ESP_LOGI(TAGS, "displayQ Event: %d, watts: %f, info: %d", 
                    electricity_event.event, electricity_event.watts, electricity_event.info);
        
        switch (electricity_event.event) {
            case SL_EXPORT:     // 0 - Export to grid
                solar.export_now = electricity_event.watts;  // PV amount being exported to grid 
                solar.b_update_grid = true;
                if (!solar.b_export_flag) {  // currently not showing exporting to grid arrow
                    solar.b_export_flag = true;
                    solar.b_import_flag = false;   // can not import if we are exporting

                    // TODO: Send message to logging
                }
            break;

            case SL_IMPORT:     // 1- Import from grid
                solar.import_now = electricity_event.watts;  // Amount of electricity being imported
                solar.b_update_grid = true;
                if (!solar.b_import_flag) {    // currently not show import arrow
                    solar.b_import_flag = true;
                    solar.b_export_flag = false;   // can not export if we are importing

                    // TODO: Send message to logging
                }
            break;

            case SL_NOW:        // 2 - Solar PV now
                solar.pv_now = electricity_event.watts;  // PV now
                if (solar.pv_now > 0) {
                    solar.b_solar_flag = true;
                } else {
                    solar.b_solar_flag = false;
                }
                solar.b_update_pv_now = true;
            break;

            case SL_TODAY:      // 3 - Solar PV today
                solar.pv_today = electricity_event.watts;  // PV today
                solar.b_update_pv_today = true;

            break;

            case SL_WT_NOW:     // 4 - Solar water tank PV now
                solar.wt_now = electricity_event.watts;
                if (solar.wt_now > 0) {  // using pv to heat water
                    solar.b_water_tank_flag = true;
                } else {
                    solar.b_water_tank_flag = false;
                }
                solar.b_update_wt_now = true;
            break;
            
            case SL_WT_TODAY:   // 5- Solar water tank PV today
                solar.wt_today = electricity_event.watts;
                if (solar.wt_today > 0) {
                    solar.wt_today /= 1000;  // Want value as n.nn kW
                } else {
                    solar.wt_today = 0.00;
                }
                solar.b_update_wt_today = true;
            break;

            case SL_BATTERY:    // 6 - CT battery status (LOW/OK)
                if (solar.sender_battery_status != electricity_event.info) {    // update if not the same already
                    switch (electricity_event.info) {
                        case IB_BATTERY_OK:
                            solar.sender_battery_status = electricity_event.info;
                            strcpy(tx_item, "Sender battery: OK");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 
                        break;

                        case IB_BATTERY_LOW:
                            solar.sender_battery_status = electricity_event.info;
                            strcpy(tx_item, "Warning, sender battery: LOW");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 
                        break;
                    }
                }                
            break;

            case SL_WT_STATUS:  // 7 - Off, Heating by solar, Hot
                if (solar.water_tank_status != electricity_event.info) {
                    switch(electricity_event.info) {
                        case IB_WT_OFF:
                            solar.water_tank_status = electricity_event.info;
                            strcpy(tx_item, "WT: Off");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 
                        break;

                        case IB_WT_HEATING:         ////// Unlikely to be used.....
                            solar.water_tank_status = electricity_event.info;
                            strcpy(tx_item, "WT: Heating by solar");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 
                        break;

                        case IB_WT_HOT:
                            solar.water_tank_status = electricity_event.info;
                            strcpy(tx_item, "WT: HOT");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 
                            // TODO, need to set a flag so this is done in the loop so it
                            // can check on screensaver status....
                            fill_water_tank(2);
                        break;
                    }
                }           
            break;
        }
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
static void initialise_screen(void) {
    tft.fillScreen(TFT_BACKGROUND);

    // Define area at top of screen for date, time etc.
    tft.fillRect(0, 20, 480, 2, TFT_BLACK);
    tft.fillRect(0, 0, 480, 20, TFT_SKYBLUE);

    // Define message area at the bottom
    tft.drawLine(0, 245, 480, 245, TFT_FOREGROUND);
    tft.drawLine(210, 245, 210, 320, TFT_FOREGROUND);

    draw_sun(65, 145);
    draw_house(210, 130);
    draw_pylon(380, 130);
    draw_water_tank(213, 160);

    lineSprite.pushSprite(sun_x, sun_y+10);
    lineSprite.pushSprite(grid_x, grid_y+10);
    lineSprite.pushSprite(water_x, water_y+10);

    tft.setCursor(75, 3, 1);   // position and font
    tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
    tft.setTextSize(2);
    tft.print("House Electricity Monitor");

    logSprite.pushSprite(211, 246);

    //showMessage("13:43:23", 5, 250, 1, 2);
    show_message("Sun 17 Mar 24", 110, 250, 1, 2);

    show_message("Water Tank: Heating by solar", 5, 270, 1, 2);
    show_message("Sender Battery: OK", 5, 288, 1, 2);

    show_message("IP: 192.168.5.67", 5, 310, 0, 1);
    show_message("LQI: 23", 160, 310, 0, 1);

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
static void setup_screen_saver(void) {
    b_screen_saver_is_active = true;
            
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
static void show_message(String msg, int x, int y, int textSize, int font) {
    tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    tft.setCursor(x, y, font);   // position and font
    tft.setTextSize(textSize);
    tft.print(msg);
}

// /**
//  * @brief add item to clog
//  * 
//  */
// void updateLog(const char *msg) {
//     portENTER_CRITICAL(&myMux);
    
//     // Add time to message then add to CLOG
//     CLOG(myLog1.add(), "%s %s", timeStringBuff, msg);
    
//     b_update_logging = true;
//     portEXIT_CRITICAL(&myMux);
// }

// void printLogging(void) {
//     int y = 3; // top of log area

//     logSprite.fillSprite(TFT_BACKGROUND);
//     logSprite.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
//     logSprite.setTextFont(0);
//     for (uint8_t i = 0; i < myLog1.numEntries; i++, y+=10) {
//         logSprite.setCursor(5, y);
//         logSprite.print(myLog1.get(i));
//     }

//     logSprite.pushSprite(211, 246);
//     portENTER_CRITICAL(&myMux);
//     b_update_logging = true;
//     portEXIT_CRITICAL(&myMux);

// }

/**
 * @brief Draw a house where xy is the bottom left of the house
 * 
 * @param x Bottom left x of house
 * @param y Bottom left y of house
 */
static void draw_house(int x, int y) {
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
static void draw_pylon(int x, int y) {
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
static void draw_sun(int x, int y) {
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

static void draw_water_tank(int x, int y) {
//350, 160
    tft.drawRoundRect(x, y, 22, 33, 6, TFT_FOREGROUND);
    tft.fillRoundRect(x+1, y+1, 20, 31, 6, TFT_WATERTANK_COLD);

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

/**
 * @brief Fill the water tank (water) with the colour of how hot the 
 * water is. Blue for cold, red for hot and purple if heating and over 2kw 
 * of PV used.
 * 
 * @param temperature Temperature of the water
 */
static void fill_water_tank(int temperature) {
    switch (temperature) {
        case 0: // cold
            tft.fillRoundRect(213+1, 160+1, 20, 31, 6, TFT_WATERTANK_COLD);
        break;

        case 1: // warm
            tft.fillRoundRect(213+1, 160+1, 20, 31, 6, TFT_WATERTANK_WARM);
        break;

        case 2: // hot
            tft.fillRoundRect(213+1, 160+1, 20, 31, 6, TFT_WATERTANK_HOT);
        break;
    }
}


void update_local_time(void) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

  // Update buffer with current time
  //strftime(date_time_buffer, sizeof(date_time_buffer), "%H:%M:%S %a %b %d %Y", &timeinfo);
  strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &timeinfo);
}

/**
 * @brief Set the Solar Generation Flag object
 * 
 * @param setting Bool value to set flag, true solar generation
 */
void setSolarGenerationFlag(bool setting) {
    if (solar.b_solar_flag != setting) {
        solar.b_solar_flag = setting;
    }
}

/**
 * @brief Set the Grid Import Flag object. If importing is true, exporting
 * has to be false.
 * 
 * @param setting Bool value to set flag, true importing from grid
 */
void setGridb_import_flag(bool setting) {
    if (solar.b_import_flag != setting) {
        solar.b_import_flag = setting;
        if (solar.b_import_flag) {           // can't import and export at the same time
            solar.b_export_flag = false;
            
            // updateLog("Importing from the grid");
            char tx_item[] = "Importing from the grid";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
            } 
        }
    }
}

/**
 * @brief Set the Grid Export Flag object. If exporting is true, importing has 
 * to be false.
 * 
 * @param setting Bool value to set flag, true if exporting to grid
 */
void setGridb_export_flag(bool setting) {
    if (solar.b_export_flag != setting) {
        solar.b_export_flag = setting;
        if (solar.b_export_flag) {           // can't import and export at the same time
            solar.b_import_flag = false;
//            updateLog("Exporting to the grid");
            char tx_item[] = "Exporting to the grid";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
            }
        }
    }
}

/**
 * @brief Set the Water Tank Flag object. 
 *
 * @param setting Bool value to set flag, true if heating water tank via solar
 */
void setb_water_tank_flag(bool setting) {
    if (solar.b_water_tank_flag != setting)
        solar.b_water_tank_flag = setting;
}

/**
 * @brief Set pv_now value - current solar (watts) being generated
 * 
 * @param pv Current value of PV (Watts)
 */
void setpv_now(int pv) {
    portENTER_CRITICAL(&myMux);
    solar.pv_now = pv;
    solar.b_update_pv_now = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set the Total Today object
 * 
 * @param total Total value of PV generated today (kW)
 */
void setpv_today(float total) {
    portENTER_CRITICAL(&myMux);
    solar.pv_today = total;
    solar.b_update_pv_today = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set wt_now object
 * 
 * @param pv Current valaue of PV being used now to heat water tank
*/
void setwt_now(int pv) {
    portENTER_CRITICAL(&myMux);
    solar.wt_now = pv;
    solar.b_update_wt_now = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set the Total used to heat the how water tank object
 * 
 * @param total Total value of PV used today to heat the water tank (Watts)
 */
void setwt_today(int total) {
    portENTER_CRITICAL(&myMux);
    if (total > 0) {
        solar.wt_today = (float) total / 1000; //(float) temp;  // round to nearest value, 2 dicmal points
    } else {
        solar.wt_today = 0.00;
    }
    solar.b_update_wt_today = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set Export Now object
 * 
 * @param pv Current valaue of PV being exported to the grid
*/
void setexport_now(int pv) {
    portENTER_CRITICAL(&myMux);
    solar.export_now = pv;
    solar.b_update_grid = true;
    portEXIT_CRITICAL(&myMux);
}

/**
 * @brief Set Import Now object
 * 
 * @param pv Current valaue of electricity being imported from the grid
*/
void setimport_now(int grid) {
    portENTER_CRITICAL(&myMux);
    solar.import_now = grid;
    solar.b_update_grid = true;
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
static void draw_string(int x, int y, String text, alignment align) {
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
[731172][I][main.cpp:786] mqtt_callback(): [iBoost] MQTT topic: solar/pv_now, message: 266.0
[731706][I][main.cpp:786] mqtt_callback(): [iBoost] MQTT topic: solar/pvtotal, message: 7.5
[733185][I][main.cpp:428] receive_packet_task(): [iBoost] Heating: 0 Watts  P1: 598323  Importing: 1534 Watts  P2: 21701
[733193][I][main.cpp:465] receive_packet_task(): [iBoost] Heating by Solar = 0 Watts
[733201][I][main.cpp:472] receive_packet_task(): [iBoost] Sender Battery OK
[733207][I][main.cpp:477] receive_packet_task(): [iBoost] Today: 2583 Wh   Yesterday: 3388 Wh   Last 7 Days: 21701 Wh   Last 28 Days: 74012 Wh   Total: 1902312 Wh   Boost Time: 0
[733225][I][main.cpp:503] receive_packet_task(): [iBoost] Published MQTT message: {"savedToday":2583,"hotWater":"Heating by Solar","battery":"OK"}
[790468][I][main.cpp:786] mqtt_callback(): [iBoost] MQTT topic: solar/pv_now, message: 266.0
[791000][I][main.cpp:786] mqtt_callback(): [iBoost] MQTT topic: solar/pvtotal, message: 7.5
*/