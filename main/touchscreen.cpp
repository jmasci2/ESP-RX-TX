#include "touchscreen.hpp"

#include <vector>

#include <esp_display_panel.hpp>
#include <lvgl.h>

using esp_panel::drivers::TouchPoint;

static esp_panel::board::Board *displayBoard = nullptr;

static void readTouch(lv_indev_t *input, lv_indev_data_t *data) {
  (void)input;
  auto touch = displayBoard->getTouch();
  if (!touch) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  touch->readRawData(-1, -1, 0);
  std::vector<TouchPoint> points;
  touch->getPoints(points);
  if (points.empty()) {
    data->state = LV_INDEV_STATE_RELEASED;
  } else {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = points[0].x;
    data->point.y = points[0].y;
  }
}

void initializeTouchscreen(esp_panel::board::Board *board) {
  displayBoard = board;
  lv_indev_t *touchInput = lv_indev_create();
  lv_indev_set_type(touchInput, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touchInput, readTouch);
}
