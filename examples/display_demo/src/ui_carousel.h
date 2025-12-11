#ifndef UI_CAROUSEL_H
#define UI_CAROUSEL_H

#include <Arduino.h>
#include "lvgl.h"

// Entry point to create the Carousel App Screen
// parent: The root object for the screen (usually created by the screen manager)
void create_carousel_app(lv_obj_t *parent);

#endif // UI_CAROUSEL_H
