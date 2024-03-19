#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <SPI.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "TFT_eSPI.h"

void displayTask(void *parameter);

#endif
