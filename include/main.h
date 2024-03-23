#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"


// iBoost derived information
typedef enum {
    IB_NONE         = 0,    // No iBoost event
    IB_BATTERY_OK   = 1,    // CT battery ok  
    IB_BATTERY_LOW  = 2,    // CT battery low
    IB_WT_OFF          = 3,    // Water tank heating off
    IB_WT_HEATING      = 4,    // Water tank heating by solar
    IB_WT_HOT          = 5     // Water tank hot
} ib_info_t;

// Solar/iboost event/value
typedef enum {
    SL_EXPORT       = 0,    // Export to grid
    SL_IMPORT       = 1,    // Import from grid
    SL_NOW          = 2,    // Solar PV now
    SL_TODAY        = 3,    // Solar PV today
    SL_WT_NOW       = 4,    // Solar water tank PV now - TODO: Check if used or not
    SL_WT_TODAY     = 5,    // Solar water tank PV today
    SL_BATTERY      = 6,    // CT battery status (LOW/OK)
    SL_WT_STATUS    = 7     // Off, Heating by solar, Hot
} sl_event_t;

typedef struct {
    sl_event_t event;       // Event that has happened
    ib_info_t info;         // iBoost information (if present)
    float watts;            // Value in watts
} electricity_event_t;



void displayTask(void *parameter);
void updateLog(const char *msg);
void updateLocalTime(void);
void setSolarGenerationFlag (bool setting);
void setGridImportFlag (bool setting);
void setGridExportFlag (bool setting);
void setWaterTankFlag (bool setting);
void setPVNow(int pv);
void setPVToday(float total);
void setWTNow(int pv);
void setWTToday(int total);
void setExportNow(int pv);
void setImportNow(int grid);

