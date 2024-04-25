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

    This project uses the HSPI port for ESP32 && TFT ILI9486 480x320 with touch & SD card reader
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

    CC1101      ---->       ESP32 - Using VSPI
    Power                   3.3V
    Ground                  GND
    MOSI                    23
    CLK                     18
    MISO                    D19
    GDO2                    Not currently used
    GDO0                     2
    CSN                      5
*/

// Logging tag
static const char* TAGS = "SCREEN";

static TFT_eSPI tft = TFT_eSPI();              // TFT object

static TFT_eSprite horizontal_line_sprite = TFT_eSprite(&tft);    // Sprite object
static TFT_eSprite vertical_line_sprite = TFT_eSprite(&tft);    // Sprite object
static TFT_eSprite log_sprite = TFT_eSprite(&tft);    // Sprite object for log area
static TFT_eSprite solar_dot_sprite = TFT_eSprite(&tft);
static TFT_eSprite clear_dot_sprite = TFT_eSprite(&tft);
static TFT_eSprite water_tank_dot_sprite = TFT_eSprite(&tft);
static TFT_eSprite grid_dot_sprite = TFT_eSprite(&tft);

// TFT specific defines
//#define TOUCH_CS 21             // Touch CS to PIN 21 for VSPI, PIN 4 for HSPI
#define REPEAT_CAL false        // True if calibration is requested after reboot
#define TFT_GREEN_ENERGY    0x1d85  // RGB 3 44 5

#define TFT_BACKGROUND 0x3189 // 0x2969 // 0x4a31 // TFT_BLACK
#define TFT_FOREGROUND TFT_WHITE
#define TFT_WATERTANK_HOT TFT_RED
#define TFT_WATERTANK_WARM TFT_VIOLET
#define TFT_WATERTANK_COLD TFT_BLUE

// Screen Saver / Touch
TFT_eSPI_Button key[1];     // TFT_eSPI button class

#define TEXT_HEIGHT 8     // Height of text to be printed and scrolled
#define TEXT_WIDTH 6      // Width of text to be printed and scrolled

#define LINE_HEIGHT 9     // TEXT_HEIGHT + 1
#define COL_WIDTH 8       // TEXT_WIDTH + 2

#define MAX_CHR 35        // characters per line (tft.height() / LINE_HEIGHT);
#define MAX_COL 54        // maximum number of columns (tft.width() / COL_WIDTH);
#define MAX_COL_DOT6 32   // MAX_COL * 0.6

#define WT_X 222          // Water tank x coordinates
#define WT_Y 185          // Water tank y coordinates

#define LQI_X 400         // LQI x coordinates
#define LQI_Y -3          // LQI y coordinates

#define BATTERY_X 452     // Battery x
#define BATTERY_Y 4       // Battery y

static int col_pos[MAX_COL];
static int chr_map[MAX_COL][MAX_CHR];
static byte color_map[MAX_COL][MAX_CHR];
static uint16_t yPos = 0;
static int rnd_x;
static int rnd_col_pos;
static int color;
//

// Animation
static int sun_x = 91;      // Sun x y
static int sun_y = 70;
static int grid_x = 276;
static int grid_y = 70;
static int water_y = 116;
static int width = 104;    // Width of drawing space minus width of arrow/dot 
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
static const uint16_t max_entries = 6;
static const uint16_t max_entry_chars = 90;
static CLOG_NEW my_log(max_entries, max_entry_chars, NO_TRIGGER, WRAP);
static bool b_update_logging = false;
//

//
static char time_buffer[9] = {0};       // buffer for time on the display
//

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
    bool b_update_wt_colour;     // Update the colour of the water tank
    bool b_clear_wt_dot;         // When water is hot or off clear the dot and text from the screen
    bool b_update_lqi;           // LQI indicator
    bool b_update_battery;       // Update CT battery icon

    ib_info_t sender_battery_status;   // Status of the sender battery in the CT clamp
    int water_tank_status;       // Water tank status (OFF, Heating by Solar, HOT)

    uint8_t lqi;                 // Radio LQI value
} solar_t;
//

// Initialise struct, set all flags to false and default values
solar_t volatile solar = {.b_solar_flag = false, .b_water_tank_flag = false, .b_import_flag = false, .b_export_flag = false,
                            .b_update_pv_now = false, .b_update_pv_today = false, .b_update_grid = false, .b_update_wt_now = false, 
                            .b_update_wt_today = false, .b_update_wt_colour = false, .b_clear_wt_dot = false, .b_update_lqi = false, 
                            .b_update_battery = false, .sender_battery_status = IB_BATTERY_LOW, .water_tank_status = IB_WT_OFF, 
                            .lqi = 255};
//

// Function defenitions
static void initialise_screen(void);
static void draw_buttons(void);
static void draw_house(int x, int y);
static void draw_pylon(int x, int y);
static void draw_solar_panel(int x, int y);
static void draw_sun(int x, int y);
static void draw_water_tank(int x, int y);
static void fill_water_tank(int temperature);
static void animate(void);
static void matrix(void);
static void touch(void);
static void setup_screen_saver(void);
static void reset_screen_saver_colour_map(void);
static void electricity_event_handler(void);
static void draw_horizontal_battery(int x, int y, ib_info_t battery_status);
static void draw_vertical_battery(int x, int y, ib_info_t battery_status);
static void draw_lqi_signal_strength(int x, int y, uint8_t lqi);
static void print_pv_today(void);
static void print_pv_now(void);
static void print_wt_today(void);
static void print_wt_now(void);

// Screensaver related
typedef enum {
    SS_COLD         = 4,    // Colour shift for screen saver
    SS_WARM         = 11,   
    SS_HOT          = 14
} ss_colour_t;  // colour shift for screen saver, 4 = blue (cold water), 11 = orange (warm water), 14 = red (hot water)

static uint32_t inactive_timer = -99999;  // inactivity run time timer
static bool b_screen_saver_is_active = false;     // Is the screen saver active or not
static uint8_t screen_saver_colour_shift = SS_COLD;   
//

/**
 * @brief Task to handle all things screen related.
 * 
 */
void display_task(void *parameter) {
    uint32_t check_animation = 0;       // time for next update
    uint8_t update_animation = 50;      // update every 40ms
    uint32_t check_matrix = 0;          // time for next update
    uint16_t update_matrix = 500;       // update matrix screen saver every 'n' ms
    uint32_t inactive = 1000 * 60 * 15; // inactivity of 15 minutes then start screen saver
    uint32_t check_one_second = 0;      // one second counter

    char *item = NULL;
    size_t item_size;
    char tx_item[50];                   // buffer used to send messages for logging
    UBaseType_t res = pdFALSE;

    bool b_show_date = true;            // Once we have the time, show the date, only needs to be run once
    bool b_heartbeat = true;

    // screen saver variables
    int x = 0;                       
    int y = 0;
    int previous_x = 0;
    int previous_y = 0;
    int previous_text_width = 0;
    int counter = 0;
    uint8_t font_size = 4;
    uint8_t text_size = 1;
    int text_width = tft.textWidth("22.22 kW / Heating by Solar", font_size);
    // TODO - get text width of each possible string

    memset(tx_item, '\0', sizeof(tx_item));

    //ESP_LOGI(TAGS, "Executing on core: %d", xPortGetCoreID());

    // Set all chip selects high to astatic void bus contention during initialisation of each peripheral
    digitalWrite(TOUCH_CS, HIGH);   // ********** TFT_eSPI touch **********
    digitalWrite(TFT_CS, HIGH);     // ********** TFT_eSPI screen library **********

    randomSeed(analogRead(A0));

    tft.init();
    tft.invertDisplay(false); // Required for my LCD TFT screen for color correction

    tft.setRotation(3);
    tft.setSwapBytes(true); // Color bytes are swapped when writing to RAM, this introduces a small overhead but
                            // there is a net performance gain by using swapped bytes.

    tft.setTextSize(2);

    // Create the Sprites
    log_sprite.createSprite(420, 70);
    log_sprite.fillSprite(TFT_BACKGROUND);
    
    // Sprites for animations
    horizontal_line_sprite.createSprite(114, 1);
    vertical_line_sprite.createSprite(1, 44);
    horizontal_line_sprite.fillSprite(TFT_BACKGROUND);
    vertical_line_sprite.fillSprite(TFT_BACKGROUND);

    solar_dot_sprite.createSprite(10, 10);
    solar_dot_sprite.fillSprite(TFT_BACKGROUND);
    solar_dot_sprite.drawLine(0, 5, 9, 5, TFT_GREEN_ENERGY);
    solar_dot_sprite.fillSmoothCircle(5, 5, 4, TFT_GREEN_ENERGY);

    grid_dot_sprite.createSprite(10, 10);
    grid_dot_sprite.fillSprite(TFT_BACKGROUND);
    grid_dot_sprite.drawLine(0, 5, 9, 5, TFT_ORANGE);
    grid_dot_sprite.fillSmoothCircle(5, 5, 4, TFT_ORANGE);

    water_tank_dot_sprite.createSprite(10, 10);
    water_tank_dot_sprite.fillSprite(TFT_BACKGROUND);
    water_tank_dot_sprite.drawLine(5, 0, 5, 9, TFT_CYAN);
    water_tank_dot_sprite.fillSmoothCircle(5, 5, 4, TFT_CYAN);

    clear_dot_sprite.createSprite(10, 10);
    clear_dot_sprite.fillSprite(TFT_BACKGROUND);
    clear_dot_sprite.drawLine(0, 5, 10, 5, TFT_GREEN_ENERGY);

    vertical_line_sprite.drawLine(0, 0, 0, 44, TFT_CYAN);

    initialise_screen(); 
        
    ESP_LOGI(TAGS, "text_width: %d", text_width);

    ESP_LOGI(TAGS, "Display Initialisation Complete");

    inactive_timer = millis();     // start inactivity timer for turning on the screen saver

    for ( ;; ) {
        if (b_screen_saver_is_active) {
            // Move screen saver message every 2 seconds (60 x vTaskDelay below)
            if (counter++ > 60) {   
                counter = 0;

                x = random(1, 478 - text_width);
                y = random(1, 295);

                // Clear last text                    
                tft.fillRect(previous_x, previous_y-5, text_width, 30, TFT_BLACK);
                
                tft.setCursor(x, y, font_size);   // position and font
                tft.setTextSize(text_size);
                if (solar.b_water_tank_flag) {
                    tft.setTextColor(TFT_GREEN_ENERGY, TFT_BLACK);
                    tft.print(solar.pv_today);
                    tft.print(" kW / Heating by Solar");
                } else if (solar.water_tank_status == IB_WT_OFF) {
                    tft.setTextColor(TFT_CYAN, TFT_BLACK);
                    tft.print(solar.pv_today);
                    tft.print(" kW / Heating Off");
                } else if (solar.water_tank_status == IB_WT_HOT) {
                    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
                    tft.print(solar.pv_today);
                    tft.print(" kW / Tank is HOT");
                } else {
                    tft.setTextColor(TFT_WHITE, TFT_BLACK);
                    tft.print(solar.pv_today);
                    tft.print(" kW");
                }

                previous_x = x;
                previous_y = y;
            }
            // Matrix is nice but takes too long to draw!!
            // if (millis() - check_matrix >= update_matrix) {  // time has elapsed, update display
            //     check_matrix = millis();
            //     matrix();
            // }
        } else { // check flags and update display as appropriate
            if (solar.b_update_pv_today) {
                print_pv_today();

                solar.b_update_pv_today = false;
            }

            if (solar.b_update_pv_now) {
                print_pv_now();

                solar.b_update_pv_now = false;
            }

            if (solar.b_update_wt_now) {
                print_wt_now();

                solar.b_update_wt_now = false;
            }

            if (solar.b_update_wt_today) {
                print_wt_today();
                
                solar.b_update_wt_today = false;
            }

            if (solar.b_update_wt_colour) {
                switch (screen_saver_colour_shift) {
                    case SS_COLD: // cold
                        fill_water_tank(0);
                    break;
                    
                    case SS_WARM: // warm
                        fill_water_tank(1);
                    break;

                    case SS_HOT: // hot
                        fill_water_tank(2);
                    break;
                }

                solar.b_update_wt_colour = false;
            }

            if (solar.b_update_grid) {
                // TODO need to move to own function and a sprite
                tft.setCursor(312, 86, 2);   // position and font
                tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                tft.setTextSize(1);
                if (solar.b_import_flag) {
                    tft.print(solar.import_now);
                } else {
                    tft.print(solar.export_now);
                }
                tft.print(" W   ");

                solar.b_update_grid = false;
            }

            if (b_update_logging) {
                log_sprite.fillSprite(TFT_BACKGROUND);
                log_sprite.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
                log_sprite.setTextFont(1);

                for (int i = 0, y = 0; i < my_log.numEntries; i++, y+=10) {
                    log_sprite.setCursor(7, y);
                    log_sprite.print(my_log.get(i));
                }

                log_sprite.pushSprite(0, 258);

                b_update_logging = false;

                /* In production version only update/important messages will be logged,
                   if the screensaver isn't active then reset timer for activation of
                   the screensaver. 
                */
                inactive_timer = millis();     // reset inactivity timer to now
            }

            if (solar.b_update_lqi) {
                solar.b_update_lqi = false;
                draw_lqi_signal_strength(LQI_X, LQI_Y, solar.lqi);
            }

            if (solar.b_update_battery) {
                solar.b_update_battery = false;
                draw_horizontal_battery(BATTERY_X, BATTERY_Y, solar.sender_battery_status);
            }

            if (millis() - check_animation >= update_animation) {  // time has elapsed, update display
                check_animation = millis();
                animate();
            }

            if (millis() >= inactive_timer + inactive) {       // We've been inactive for 'n' minutes, start screensaver
                strcpy(tx_item, "No activity, starting screen saver");
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
            if (!b_screen_saver_is_active) {
                tft.setCursor(5, 3, 1);   // position and font
                tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
                tft.setTextSize(2);
                tft.print(time_buffer);
            }
        }

        //Receive an item from no-split ring buffer - used for logging purposes
        item = (char *)xRingbufferReceive(buf_handle, &item_size, pdMS_TO_TICKS(0));
        if (item != NULL) {
            ESP_LOGI(TAGS, "Retrieved item from ringbuffer");

            // Add time to message then add to CLOG
            CLOG(my_log.add(), "%s  %s", time_buffer, item);
            b_update_logging = true;   // set flag so log is redrawn

            //Return Item
            vRingbufferReturnItem(buf_handle, (void *)item);
        } 

        electricity_event_handler();

        // JUST A DARK SCREEN FOR SCREEN SAVER SO DELAY A BIT LONGER NOW
        vTaskDelay(30 / portTICK_PERIOD_MS);

        // vTaskDelay(30 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}


/**
 * @brief Animation of arrows to show the flow of electricity. Solar generation, water tank
 * heating, grid import or export.
 * 
 */
static void animate(void) {
    static int sun_start_position = sun_x;       // 
    static int grid_import_start_position = grid_x;     //
    static int grid_export_start_position = grid_x;     //
    static int water_start_position = water_y;     // y as it is vertical line
    static int solar_dot_x = sun_start_position;                // solar generation arrow start point 
    static int grid_import_dot_x = grid_import_start_position + width;      // grid import arrow start point
    static int grid_export_dot_x = grid_export_start_position;      // grid export arrow start point
    static int water_dot_y = water_start_position;            // water heating arrow start point
    static int grid_dot_x = grid_x + (width/2);      // start grid dot in the middle at the start of the program

    // Solar generation dot
    if (solar.b_solar_flag) {
        solar_dot_sprite.pushSprite(solar_dot_x, sun_y+5);
        solar_dot_x += step;
        if (solar_dot_x > (width + sun_start_position)) {
            clear_dot_sprite.fillSprite(TFT_BACKGROUND);
            clear_dot_sprite.drawLine(0, 5, 10, 5, TFT_GREEN_ENERGY);          
            clear_dot_sprite.pushSprite(solar_dot_x-step, sun_y+5);
            solar_dot_x = sun_start_position;
        }         
    }

    // Grid import dot
    if (solar.b_import_flag) {
        grid_dot_sprite.pushSprite(grid_dot_x, grid_y+5);
        grid_dot_x -= step;
        if (grid_dot_x < grid_import_start_position) {
            clear_dot_sprite.fillSprite(TFT_BACKGROUND);
            clear_dot_sprite.drawLine(0, 5, 10, 5, TFT_ORANGE);          
            clear_dot_sprite.pushSprite(grid_dot_x+step, grid_y+5);
            grid_dot_x = grid_import_start_position + width;
        } 
    }

    // Grid export dot
    if (solar.b_export_flag) {
        grid_dot_sprite.pushSprite(grid_dot_x, grid_y+5);
        grid_dot_x += step;
        if (grid_dot_x > grid_export_start_position + width) {
            clear_dot_sprite.fillSprite(TFT_BACKGROUND);
            clear_dot_sprite.drawLine(0, 5, 10, 5, TFT_ORANGE);          
            clear_dot_sprite.pushSprite(grid_dot_x-step, grid_y+5);
            grid_dot_x = grid_export_start_position;
        } 
    }

    // Water tank heating by solar animation
    if (solar.b_water_tank_flag) {
        water_tank_dot_sprite.pushSprite(234, water_dot_y);
        water_dot_y += step;
        if (water_dot_y > water_start_position + 34) {
            clear_dot_sprite.fillSprite(TFT_BACKGROUND);
            clear_dot_sprite.drawLine(5, 0, 5, 10, TFT_CYAN);          
            clear_dot_sprite.pushSprite(234, water_dot_y-step);
            water_dot_y = water_start_position;
        } 
    } else if (solar.b_clear_wt_dot) {  // clear the dot, water tank is hot or off
        clear_dot_sprite.fillSprite(TFT_BACKGROUND);
        clear_dot_sprite.drawLine(5, 0, 5, 10, TFT_CYAN);          
        clear_dot_sprite.pushSprite(234, water_dot_y-step);
        solar.b_clear_wt_dot = false;
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
            char tx_item[] = "Screen saver started by user";
            UBaseType_t res =  xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0));
            if (res != pdTRUE) {
                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
            } 
            
            setup_screen_saver();
        } else if (b_screen_saver_is_active) {
            ESP_LOGI(TAGS, "Stop screen saver");
            b_screen_saver_is_active = false;
            inactive_timer = millis();     // reset inactivity timer to now
            initialise_screen();
            char tx_item[] = "Screen saver stopped by user interaction";
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
}

/**
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
        ESP_LOGI(TAGS, "Electricity Event: %d, watts: %f, info: %d", 
                    electricity_event.event, electricity_event.value, electricity_event.info);
        
        switch (electricity_event.event) {
            case SL_EXPORT:     // 0 - Export to grid
                solar.export_now = electricity_event.value;  // PV amount being exported to grid 
                solar.b_update_grid = true;
                if (!solar.b_export_flag) {  // currently not showing exporting to grid arrow
                    solar.b_export_flag = true;
                    solar.b_import_flag = false;   // can not import if we are exporting
                    // reset grid dot
                    grid_dot_sprite.fillSprite(TFT_BACKGROUND);
                    grid_dot_sprite.drawLine(0, 5, 9, 5, TFT_ORANGE);
                    grid_dot_sprite.fillSmoothCircle(5, 5, 4, TFT_ORANGE);
                }
            break;

            case SL_IMPORT:     // 1- Import from grid
                solar.import_now = electricity_event.value;  // Amount of electricity being imported
                solar.b_update_grid = true;
                if (!solar.b_import_flag) {    // currently not show import arrow
                    solar.b_import_flag = true;
                    solar.b_export_flag = false;   // can not export if we are importing
                    // reset grid dot
                    grid_dot_sprite.fillSprite(TFT_BACKGROUND);
                    grid_dot_sprite.drawLine(0, 5, 9, 5, TFT_ORANGE);
                    grid_dot_sprite.fillSmoothCircle(4, 4, 4, TFT_ORANGE);        
                }
            break;

            case SL_NOW:        // 2 - Solar PV now
                solar.pv_now = electricity_event.value;  // PV now
                if (solar.pv_now > 0) {
                    solar.b_solar_flag = true;
                } else {
                    solar.b_solar_flag = false;
                    // TODO: set a flag clear any solar pv_now values and the dot
                }
                solar.b_update_pv_now = true;
            break;

            case SL_TODAY:      // 3 - Solar PV today
                solar.pv_today = electricity_event.value;  // PV today
                solar.b_update_pv_today = true;
            break;

            case SL_WT_NOW:     // 4 - Solar water tank PV now
                solar.wt_now = electricity_event.value;
                if (solar.wt_now > 0) {  // using pv to heat water
                    solar.b_water_tank_flag = true;
                } else {
                    solar.b_water_tank_flag = false;
                    // TODO: set a flag clear any water tank writing for wt_now and the do
                    solar.b_clear_wt_dot = true;
                }
                solar.b_update_wt_now = true;

                // Some times the water tank can be the wrong colour :-(  Belts and
                // braces check to correct that...
                if (solar.wt_today > 2 && screen_saver_colour_shift == SS_COLD) {
                    screen_saver_colour_shift = SS_WARM;
                    solar.b_update_wt_colour = true;
                    reset_screen_saver_colour_map();
                }
            break;
            
            case SL_WT_TODAY:   // 5- Solar water tank PV today
                solar.wt_today = electricity_event.value;
                if (solar.wt_today > 0) {
                    solar.wt_today /= 1000;  // Want value as n.nn kW
                } else {
                    solar.wt_today = 0.00;
                }
                solar.b_update_wt_today = true;

                solar.b_update_wt_colour = true;
                
                // Set screen saver colour shift
                if (solar.water_tank_status == IB_WT_HOT) {
                    screen_saver_colour_shift = SS_HOT;
                    reset_screen_saver_colour_map();
                } else if (solar.wt_today > 2) {
                    // More than 2000 watts of energy will give us warm water for a shower
                    screen_saver_colour_shift = SS_WARM;
                    reset_screen_saver_colour_map();
                } else {
                    screen_saver_colour_shift = SS_COLD;
                    reset_screen_saver_colour_map();
                }
            break;

            case SL_BATTERY:    // 6 - CT battery status (LOW/OK)
                if (solar.sender_battery_status != electricity_event.info) {    // update if not the same already
                    switch (electricity_event.info) {
                        case IB_BATTERY_OK:
                            solar.sender_battery_status = electricity_event.info;
                            solar.b_update_battery = true;
                            strcpy(tx_item, "CT battery reports it is OK");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 
                        break;

                        case IB_BATTERY_LOW:
                            solar.sender_battery_status = electricity_event.info;
                            solar.b_update_battery = true;
                            strcpy(tx_item, "Warning, CT battery is reporting it is LOW");
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
                            solar.b_update_wt_colour = true;
                            solar.b_clear_wt_dot = true;
                            strcpy(tx_item, "Water tank heating is now off");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 

                            // Set screen saver colour shift
                            if (solar.wt_today > 2) {
                                // More than 2000 watts of energy will give us warm water for a shower
                                screen_saver_colour_shift = SS_WARM;
                            } else {
                                screen_saver_colour_shift = SS_COLD;
                            }
                        break;

                        case IB_WT_HEATING:         ////// Unlikely to be used.....
                            solar.water_tank_status = electricity_event.info;
                            solar.b_update_wt_colour = true;
                            strcpy(tx_item, "Water tank is heating by solar");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 

                            // Set screen saver colour shift
                            if (solar.wt_today > 2) {
                                // More than 2000 watts of energy will give us warm water for a shower
                                screen_saver_colour_shift = SS_WARM;
                            } else {
                                screen_saver_colour_shift = SS_COLD;
                            }
                            
                        break;

                        case IB_WT_HOT:
                            solar.water_tank_status = electricity_event.info;
                            solar.b_update_wt_colour = true;
                            solar.b_clear_wt_dot = true;
                            strcpy(tx_item, "Water Tank is HOT");
                            if (xRingbufferSend(buf_handle, tx_item, sizeof(tx_item), pdMS_TO_TICKS(0)) != pdTRUE) {
                                ESP_LOGE(TAGS, "Failed to send Ringbuffer item");
                            } 

                            // Set screen saver colour shift
                            screen_saver_colour_shift = SS_HOT;  // red screen saver
                        break;
                    }
                }           
            break;

            case SL_LQI: // 8 - Link quality identifier - how good the radio signal is
                solar.lqi = electricity_event.value;
                solar.b_update_lqi = true;
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
            tft.setTextColor(color_map[rnd_col_pos][i] << screen_saver_colour_shift, TFT_BLACK); // Set the character colour/brightness

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
 * tank, etc.
 */
static void initialise_screen(void) {
    tft.fillScreen(TFT_BACKGROUND);

    // Define area at top of screen for date, time etc.
    tft.fillRect(0, 20, 480, 2, TFT_BLACK);
    tft.fillRect(0, 0, 480, 20, TFT_SKYBLUE);

    // Define message area at the bottom
    tft.drawLine(0, 252, 480, 252, TFT_FOREGROUND);
    tft.drawLine(0, 254, 480, 254, TFT_FOREGROUND);

    tft.drawSmoothCircle(55, 80, 35, TFT_GREEN_ENERGY, TFT_BACKGROUND); // solar
    tft.drawSmoothCircle(240, 80, 35, TFT_CYAN, TFT_BACKGROUND); // house
    tft.drawSmoothCircle(425, 80, 35, TFT_ORANGE, TFT_BACKGROUND); // pylon
    tft.drawSmoothCircle(240, 195, 35, TFT_CYAN, TFT_BACKGROUND); // water tank

    draw_solar_panel(40, 64);
    draw_house(239, 58); 
    draw_pylon(415, 104);
    draw_water_tank(WT_X, WT_Y);
    draw_lqi_signal_strength(LQI_X, LQI_Y, solar.lqi);
    draw_horizontal_battery(BATTERY_X, BATTERY_Y, solar.sender_battery_status);

    // lines connecting circles
    horizontal_line_sprite.drawLine(0, 0, 114, 0, TFT_GREEN_ENERGY);   // solar to house
    horizontal_line_sprite.pushSprite(sun_x, sun_y+10);
    horizontal_line_sprite.drawLine(0, 0, 114, 0, TFT_ORANGE);         // house to grid
    horizontal_line_sprite.pushSprite(grid_x, grid_y+10);
    vertical_line_sprite.pushSprite(239, 116);                         // house to water tank

    tft.setCursor(190, 3, 1);   // position and font
    tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
    tft.setTextSize(2);
    tft.print("eMon 1.0");

    if (solar.wt_today > 0) {
        print_wt_today();
    }

    if (solar.pv_today > 0) {
        print_pv_today();
    }

    log_sprite.pushSprite(0, 258);
}

/**
 * @brief Setup the screen saver.  Will be started by the user touching the screen
 * or after 'n' minutes of inactivity to save the screen from burn-in.
 */
static void setup_screen_saver(void) {
    b_screen_saver_is_active = true;

    tft.fillScreen(TFT_BLACK);

    // tft.fillScreen(TFT_BLACK);

    // for (int j = 0; j < MAX_COL; j++) {
    //     for (int i = 0; i < MAX_CHR; i++) {
    //         chr_map[j][i] = 0;
    //         color_map[j][i] = 0;
    //     }

    //     color_map[j][0] = 63;
    // }
}

/**
 * @brief Reset the screen saver colour map, needed as the water is heated by PV
 * so that we can indicate to the user the heat of the water when the screen saver
 * is active
 */
static void reset_screen_saver_colour_map(void) {
    for (int j = 0; j < MAX_COL; j++) {
        for (int i = 0; i < MAX_CHR; i++) {
            color_map[j][i] = 0;
        }
        color_map[j][0] = 63;
    }
}

/**
 * @brief Draw a house where xy is the roof apex of the house
 * 
 * @param x Roof apex x of house
 * @param y Roof apex y of house
 */
static void draw_house(int x, int y) {
    tft.fillTriangle(x, y, x-20, y+18, x+20, y+18, TFT_WHITE); // roof
    tft.fillRect(x-13, y+19, 27, 17, TFT_WHITE); // house body
    tft.fillRect(x-5, y+24, 11, 14, TFT_BACKGROUND); // hole for door
}

/**
 * @brief Draw horizontal battery
 * 
 * @param x Bottom left x
 * @param y Bottom left y
 * @param battery_status Is battery ok or low, only 2 options we have
 */
static void draw_horizontal_battery(int x, int y, ib_info_t battery_status) {
    tft.drawSmoothRoundRect(x, y, 1, 1, 18, 11, TFT_BLACK, TFT_SKYBLUE);
    tft.drawRect(x+19, y+3, 2, 6, TFT_BLACK); // positive

    if (battery_status == IB_BATTERY_OK) {
        tft.fillRect(x+1, y+1, 14, 10, TFT_BLACK); // OK

    } else {
        tft.fillRect(x+1, y+1, 8, 10, TFT_RED); // LOW
    }
}

/**
 * @brief Draw vertical battery
 * 
 * @param x Bottom left x
 * @param y Bottom left y
 * @param battery_status Is battery ok or low, only 2 options we have
 */
static void draw_vertical_battery(int x, int y, ib_info_t battery_status) {
    tft.drawSmoothRoundRect(x, y, 1, 1, 12, 18, TFT_WHITE, TFT_BACKGROUND);
    tft.drawRect(x+3, y-2, 6, 2, TFT_WHITE); // positive

    if (battery_status == IB_BATTERY_OK) {
        tft.fillRect(x+1, y+4, 11, 14, TFT_WHITE); // 90%
    } else {
        tft.fillRect(x+1, y+15, 11, 8, TFT_RED); // 20%
    }
}

/**
 * @brief Draw signal strength
 * 
 * @param x Bottom left
 * @param y Bottom left
 * @param lqi Signal strength
 */
static void draw_lqi_signal_strength(int x, int y, uint8_t lqi) {
    tft.fillRect(x, y+15, 6, 6, TFT_BLACK); // first bar, always drawn

    if (lqi < 6) {
        tft.fillRect(x+9, y+10, 6, 11, TFT_BLACK); // second bar
        tft.fillRect(x+18, y+5, 6, 16, TFT_BLACK); // third bar
    } else if (lqi < 40) {
        tft.fillRect(x+9, y+10, 6, 11, TFT_BLACK);
        tft.drawRect(x+18, y+5, 6, 16, TFT_BLACK);
    } else {
        tft.drawRect(x+9, y+10, 6, 11, TFT_BLACK);
        tft.drawRect(x+18, y+5, 6, 16, TFT_BLACK);
    }
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
 * @brief Draw a solar panel
 * 
 * @param x Display x coordinates
 * @param y Display y coordinates
 */
static void draw_solar_panel(int x, int y) {
    // Solar panel outline
    tft.drawSmoothRoundRect(x, y, 1, 1, 30, 26, TFT_WHITE, TFT_BACKGROUND);

    tft.drawLine(x, y+6, x+30, y+6, TFT_WHITE); // across lines
    tft.drawLine(x, y+13, x+30, y+13, TFT_WHITE);
    tft.drawLine(x, y+19, x+30, y+19, TFT_WHITE);

    tft.drawLine(x+8, y, x+8, y+26, TFT_WHITE); // down lines
    tft.drawLine(x+15, y, x+15, y+26, TFT_WHITE);
    tft.drawLine(x+23, y, x+23, y+26, TFT_WHITE);

    tft.drawLine(x+12, y+26, x+12, y+32, TFT_WHITE); // left leg
    tft.drawLine(x+18, y+26, x+18, y+32, TFT_WHITE); // right leg

    tft.drawLine(x+12, y+32, x+9, y+32, TFT_WHITE); // left leg bottom
    tft.drawLine(x+18, y+32, x+21, y+32, TFT_WHITE); // right leg bottom
}

/**
 * @brief Draw a water tank and shower head
 * 
 * @param x Bottom x of water tank
 * @param y Bottom y of water tank
 */
static void draw_water_tank(int x, int y) {
    tft.drawRoundRect(x, y, 22, 33, 6, TFT_FOREGROUND);

    // shower hose
    tft.drawLine(x+11, y, x+11, y-5, TFT_FOREGROUND);
    tft.drawLine(x+11, y-5, x+35, y-5, TFT_FOREGROUND);
    tft.drawLine(x+35, y-5, x+35, y+5, TFT_FOREGROUND);
    tft.drawLine(x+30, y+6, x+40, y+6, TFT_FOREGROUND);
    tft.drawLine(x+31, y+7, x+39, y+7, TFT_FOREGROUND);

    switch (screen_saver_colour_shift) {
        case SS_WARM: // warm
            fill_water_tank(1);
        break;

        case SS_HOT: // hot
            fill_water_tank(2);
        break;

        default: // cold
            fill_water_tank(0);
        break;
    }
}

/**
 * @brief Fill the water tank (water) with the colour of how hot the 
 * water is. Blue for cold, red for hot and purple if heating and over 2kw 
 * of PV used.
 * 
 * @param temperature Temperature of the water
 */
static void fill_water_tank(int temperature) {
    uint32_t water_colour = TFT_WATERTANK_COLD;

    switch (temperature) {
        case 0: // cold
            ESP_LOGI(TAGS, "Set water tank colour blue");
        break;

        case 1: // warm
            water_colour = TFT_WATERTANK_WARM;
            ESP_LOGI(TAGS, "Set water tank colour purple");
        break;

        case 2: // hot
            water_colour = TFT_WATERTANK_HOT;
            ESP_LOGI(TAGS, "Set water tank colour red");
        break;
    }
    
    // water tank
    tft.fillRoundRect(WT_X+1, WT_Y+1, 20, 31, 6, water_colour);

    // water for shower head
    tft.drawLine(WT_X+31, WT_Y+8, WT_X+27, WT_Y+15, water_colour); // left
    tft.drawLine(WT_X+33, WT_Y+8, WT_X+30, WT_Y+15, water_colour); // left

    tft.drawLine(WT_X+35, WT_Y+8, WT_X+35, WT_Y+15, water_colour); // middle

    tft.drawLine(WT_X+37, WT_Y+8, WT_X+39, WT_Y+15, water_colour); // right
    tft.drawLine(WT_X+39, WT_Y+8, WT_X+42, WT_Y+15, water_colour); // right
}


void update_local_time(void) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

  // Update buffer with current time
  strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &timeinfo);
}

/**
 * @brief Print pv today value to screen
 * 
 */
static void print_pv_today(void) {
    tft.setCursor(213, 25, 2);   // position and font
    tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    tft.setTextSize(1);
    tft.print(solar.pv_today);
    tft.print(" kW   ");
}

/**
 * @brief Print pv now value to screen
 * 
 */
static void print_pv_now(void) {
    tft.setCursor(128, 86, 2);   // position and font
    tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    tft.setTextSize(1);
    tft.print(solar.pv_now);
    tft.print(" W   ");
}

/**
 * @brief Print water tank today value to screen
 * 
 */
static void print_wt_today(void) {
    tft.setCursor(216, 231, 2);   // position and font
    tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    tft.setTextSize(1);
    tft.printf("%.2f kW", solar.wt_today);
}

/**
 * @brief Print water tank PV now value to screen
 * 
 */
static void print_wt_now(void) {
    tft.setCursor(246, 140, 2);   // position and font
    tft.setTextColor(TFT_FOREGROUND, TFT_BACKGROUND);
    tft.setTextSize(1);

    // iBoost can report heating by solar with 0 watts, no point showing that value
    if (solar.wt_now > 0) {
        tft.print(solar.wt_now);
        tft.print(" W   ");
    } else {
        tft.print("         ");

    }
}