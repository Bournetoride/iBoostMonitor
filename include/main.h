#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

// Sender battery values
enum iBoostBattery {
    BATTERY_OK,
    BATTERY_LOW
};

// Heating of water tank status as reported by iBoost
enum iBoostWaterTank {
    OFF,
    HEATING_BY_SOLAR,
    HOTT
};

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

