/**
 * UI Carousel Application
 *
 * This file implements a multi-window carousel application for the E-paper
 * display. The carousel consists of 3 windows that users can navigate between:
 *   - Window 1: API Text Display (shows messages from backend server)
 *   - Window 2: Line Chart (CPU usage visualization)
 *   - Window 3: Bar Chart (Daily visits visualization)
 *
 * Navigation: Users tap the left or right edge of the screen to switch windows
 * Exit: Physical Home Button (Hardware handled)
 *
 * Data Source: HTTP polling to backend API every 15 seconds
 */

#include "ui_carousel.h"
#include "../scr_mrg.h"
#include "../ui.h"
#include "assets.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

// ============================================================================
// GLOBAL STATE VARIABLES
// ============================================================================

// UI Component References
static lv_obj_t
    *carousel_tileview; // Main tileview container (holds the 3 windows)
static lv_obj_t *api_text_label = NULL;     // Text label in Window 1
static lv_obj_t *line_chart = NULL;         // Line chart object in Window 2
static lv_chart_series_t *line_ser1 = NULL; // Data series for line chart
static lv_obj_t *bar_chart = NULL;          // Bar chart object in Window 3
static lv_chart_series_t *bar_ser1 = NULL;  // Data series for bar chart

// Navigation & Indicator
static lv_obj_t *indicator_bar = NULL; // Custom scrollbar/indicator
static lv_obj_t *nav_overlay = NULL;   // ID for transparent touch layer

// Timer for periodic API polling
static lv_timer_t *api_poll_timer = NULL;

// MANUAL column tracking to avoid coordinate bugs
// Start at 1 because we set the tile to 1 in initialization
static int current_tile_col = 1;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void parent_click_event_cb(lv_event_t *e);

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Update the visual indicator bar position
 *
 * Moves the custom scrollbar/indicator to match the current window.
 *
 * @param col Current column index (0-2)
 */
static void update_indicator_pos(int col) {
  if (!indicator_bar)
    return;

  // Total width is 100%, we have 3 screens, so bar is 33% wide.
  // Positions: 0%, 33%, 66%
  int x_pos = col * 33; // Approx percentage

  // Verify bounds
  if (x_pos > 66)
    x_pos = 66;

  // Move the bar
  lv_obj_set_x(indicator_bar, lv_pct(x_pos));
}

/**
 * Update API text label only if the content has changed
 */
static void update_api_label_safely(const char *new_text) {
  if (!api_text_label)
    return;

  const char *current_text = lv_label_get_text(api_text_label);
  if (current_text == NULL || strcmp(current_text, new_text) != 0) {
    lv_label_set_text(api_text_label, new_text);
    ui_if_epd_refr(EPD_REFRESH_TIME);
  }
}

static int get_current_tile_col(lv_obj_t *tileview) {
  lv_obj_t *tile = lv_tileview_get_tile_act(tileview);
  if (!tile)
    return 0;
  uint32_t col = lv_tileview_get_tile_act(tileview)->coords.x1 /
                 lv_obj_get_width(tileview);
  return col;
}

// Timer callback for periodic API polling
void api_poll_timer_cb(lv_timer_t *t) {
  if ((WiFi.status() == WL_CONNECTED)) {
    HTTPClient http;
    http.setConnectTimeout(200);
    http.setTimeout(500);

    Serial.println("API Polling...");

    // Fetch unified data (Text + Graphs)
    http.begin("http://192.168.0.189:8000/api/graph-data/");

    int httpCode = http.GET();
    Serial.printf("API Code: %d\n", httpCode);

    if (httpCode == 200) {
      String payload = http.getString();

      // Parse JSON
      // Allocation: 2KB on heap for safety
      DynamicJsonDocument *doc_ptr = new DynamicJsonDocument(2048);
      if (!doc_ptr) {
        http.end();
        return;
      }
      DeserializationError error = deserializeJson(*doc_ptr, payload);

      if (!error) {
        Serial.println("JSON Parsed Success"); // Debug

        // 1. TEXT UPDATE (Window 1)
        const char *content =
            (*doc_ptr)["content"]; // KEY FIX: "content", not "message"
        if (content) {
          Serial.printf("Content Found: %s\n", content);
          update_api_label_safely(content);
        } else {
          // Fallback for plain "text" key if used
          const char *text = (*doc_ptr)["text"];
          if (text) {
            Serial.printf("Text Found: %s\n", text);
            update_api_label_safely(text);
          } else {
            Serial.println("No 'message' or 'text' key found in JSON");
          }
        }

        // 2. LINE CHART UPDATE (Window 2)
        JsonObject doc = doc_ptr->as<JsonObject>();
        JsonArray line_data = doc["line_data"];
        if (line_chart && line_ser1 && !line_data.isNull()) {
          for (int i = 0; i < line_data.size() && i < 10; i++) {
            lv_chart_set_value_by_id(line_chart, line_ser1, i, line_data[i]);
          }
          lv_chart_refresh(line_chart);
        }

        // 3. BAR CHART UPDATE (Window 3)
        JsonArray bar_data = doc["bar_data"];
        if (bar_chart && bar_ser1 && !bar_data.isNull()) {
          for (int i = 0; i < bar_data.size() && i < 10; i++) {
            lv_chart_set_value_by_id(bar_chart, bar_ser1, i, bar_data[i]);
          }
          lv_chart_refresh(bar_chart);
        }
      } else {
        Serial.print("JSON Error: ");
        Serial.println(error.c_str());
      }
      delete doc_ptr;
    } else {
      // Show HTTP error to user
      String err = "API Error: " + String(httpCode);
      update_api_label_safely(err.c_str());
    }
    http.end();
  } else {
    Serial.println("WiFi Disconnected during poll");
    update_api_label_safely("WiFi Disconnected");
  }
}

// ============================================================================
// WINDOW CREATION FUNCTIONS
// ============================================================================

/**
 * Create Window 1: API Text Display
 */
static void create_window_1(lv_obj_t *tile) {
  api_text_label = lv_label_create(tile);
  lv_label_set_text(api_text_label, "Waiting for API Data...");
  lv_obj_set_style_text_align(api_text_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(api_text_label, 800);
  lv_label_set_long_mode(api_text_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(api_text_label, &Font_Mono_Bold_25, 0);
  lv_obj_center(api_text_label);
}

/**
 * Create Window 2: Line Chart
 */
static void create_window_2(lv_obj_t *tile) {
  lv_obj_t *label = lv_label_create(tile);
  lv_label_set_text(label, "Server CPU Usage (%)");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_font(label, &Font_Mono_Bold_20, 0);

  line_chart = lv_chart_create(tile);
  lv_obj_set_size(line_chart, 700, 400);
  lv_obj_center(line_chart);
  lv_chart_set_type(line_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(line_chart, 10);
  lv_chart_set_range(line_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

  line_ser1 = lv_chart_add_series(line_chart, lv_palette_main(LV_PALETTE_RED),
                                  LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < 10; i++)
    lv_chart_set_next_value(line_chart, line_ser1, 0);
}

/**
 * Create Window 3: Bar Chart
 */
static void create_window_3(lv_obj_t *tile) {
  lv_obj_t *label = lv_label_create(tile);
  lv_label_set_text(label, "Daily Visits (7 Days)");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_font(label, &Font_Mono_Bold_20, 0);

  bar_chart = lv_chart_create(tile);
  lv_obj_set_size(bar_chart, 700, 400);
  lv_obj_center(bar_chart);
  lv_chart_set_type(bar_chart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(bar_chart, 7);
  lv_chart_set_range(bar_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

  bar_ser1 = lv_chart_add_series(bar_chart, lv_palette_main(LV_PALETTE_BLUE),
                                 LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < 7; i++)
    lv_chart_set_next_value(bar_chart, bar_ser1, 0);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

/**
 * Global Tap Navigation Handler
 */
static void parent_click_event_cb(lv_event_t *e) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);

  Serial.printf("Handler Event Code: %d at X:%d Y:%d\n", lv_event_get_code(e),
                p.x, p.y);

  int next_col = current_tile_col; // Default to no change

  // Left Zone (0 - 300px)
  if (p.x < 300) {
    Serial.println("Zone: LEFT");
    if (current_tile_col > 0)
      next_col--;
    // No wrap-around
  }
  // Right Zone (660 - 960px)
  else if (p.x > 660) {
    Serial.println("Zone: RIGHT");
    if (current_tile_col < 2)
      next_col++;
    // No wrap-around
  }

  // Perform Navigation if changed
  if (next_col != current_tile_col) {
    Serial.printf("Switching to %d\n", next_col);

    // Update global tracker
    current_tile_col = next_col;

    // --- KEY FIXES HERE ---
    // 1. Use LV_ANIM_OFF for instant E-paper switch
    lv_obj_set_tile_id(carousel_tileview, next_col, 0, LV_ANIM_OFF);

    // 2. Update indicator
    update_indicator_pos(next_col);

    // 3. FORCE E-PAPER REFRESH
    // Use Standard Default Refresh
    ui_if_epd_refr(EPD_REFRESH_TIME);
  }
}

static void carousel_delete_event_cb(lv_event_t *e) {
  if (api_poll_timer) {
    lv_timer_del(api_poll_timer);
    api_poll_timer = NULL;
  }

  api_text_label = NULL;
  line_chart = NULL;
  line_ser1 = NULL;
  bar_chart = NULL;
  bar_ser1 = NULL;
  indicator_bar = NULL;

  indicator_bar = NULL;
  nav_overlay = NULL;
}

void create_carousel_app(lv_obj_t *parent) {
  // 1. Tileview
  carousel_tileview = lv_tileview_create(parent);
  lv_obj_set_size(carousel_tileview, lv_pct(100), lv_pct(100));

  // Disable swipe completely
  lv_obj_clear_flag(carousel_tileview, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(carousel_tileview, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *tile1 = lv_tileview_add_tile(carousel_tileview, 0, 0, LV_DIR_RIGHT);
  lv_obj_t *tile2 =
      lv_tileview_add_tile(carousel_tileview, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
  lv_obj_t *tile3 = lv_tileview_add_tile(carousel_tileview, 2, 0, LV_DIR_LEFT);

  // Window order: 0=Bar Chart (Daily Visits), 1=API Text (Messages), 2=Line
  // Chart (CPU)
  create_window_3(tile1); // Bar Chart on LEFT
  create_window_1(tile2); // API Text in CENTER
  create_window_2(tile3); // Line Chart on RIGHT

  // Start on center tile (index 1)
  lv_obj_set_tile_id(carousel_tileview, 1, 0, LV_ANIM_OFF);

  // 2. Custom Scrollbar
  lv_obj_t *bar_bg = lv_obj_create(parent);
  lv_obj_set_size(bar_bg, lv_pct(100), 6);
  lv_obj_align(bar_bg, LV_ALIGN_BOTTOM_MID, 0, 0);

  // FIX: Make background TRANSPARENT so user doesn't see a dark bar
  lv_obj_set_style_bg_opa(bar_bg, LV_OPA_TRANSP, 0);

  lv_obj_set_style_border_width(bar_bg, 0, 0);
  lv_obj_set_style_pad_all(bar_bg, 0, 0);
  lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_CLICKABLE);

  indicator_bar = lv_obj_create(bar_bg);
  lv_obj_set_size(indicator_bar, lv_pct(33), lv_pct(100));
  lv_obj_set_style_bg_color(indicator_bar, lv_color_black(), 0);
  lv_obj_set_style_border_width(indicator_bar, 0, 0);
  // Start indicator in CENTER position (33%)
  lv_obj_set_x(indicator_bar, lv_pct(33));
  lv_obj_clear_flag(indicator_bar, LV_OBJ_FLAG_CLICKABLE);

  // 3. TRANSPARENT NAVIGATION OVERLAY
  // This sits ON TOP of duplicates/tiles and catches ALL clicks.
  nav_overlay = lv_obj_create(parent);
  lv_obj_set_size(nav_overlay, lv_pct(100), lv_pct(100));
  lv_obj_center(nav_overlay);
  lv_obj_set_style_bg_opa(nav_overlay, LV_OPA_TRANSP, 0); // Invisible
  lv_obj_set_style_border_width(nav_overlay, 0, 0);
  lv_obj_set_style_pad_all(nav_overlay, 0, 0);

  // Allow clicking on this overlay
  lv_obj_add_flag(nav_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(nav_overlay, LV_OBJ_FLAG_SCROLLABLE);

  // Attach event handler ONLY to this overlay
  lv_obj_add_event_cb(nav_overlay, parent_click_event_cb, LV_EVENT_CLICKED,
                      NULL);

  // 4. Timers and Cleanup
  if (api_poll_timer)
    lv_timer_del(api_poll_timer);
  api_poll_timer = lv_timer_create(api_poll_timer_cb, 5000, NULL);

  lv_obj_add_event_cb(carousel_tileview, carousel_delete_event_cb,
                      LV_EVENT_DELETE, NULL);
}
