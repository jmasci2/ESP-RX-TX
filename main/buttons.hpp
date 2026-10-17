#pragma once

#include <lvgl.h>

bool initializeButtonHardware();
bool probeAds1115();
bool probeButtonExpander();
void pollInputButtonsAndAnalog();
void createTrackButtons(lv_obj_t *parent);
