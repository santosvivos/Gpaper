#include "ui_wifi_custom.h"
#include "../ui.h"
#include "../scr_mrg.h"
#include "assets.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

static lv_obj_t * ssid_list;
static lv_obj_t * saved_list;
static lv_obj_t * password_ta;
static lv_obj_t * keyboard;
static lv_obj_t * status_label;
static lv_obj_t * btn_scan;
static lv_obj_t * btn_connect;
static lv_obj_t * btn_disconnect;
static lv_obj_t * cb_remember; // The "Remember" checkbox
static char selected_ssid[33] = {0};
static lv_timer_t * wifi_timer = NULL;
static int wifi_timeout_counter = 0;

static void wifi_timer_cb(lv_timer_t * timer); // Forward declaration

static const char* WIFI_CREDS_FILE = "/wifi_creds.json";

// Helper: Save current network to JSON file
static void save_current_network() {
    DynamicJsonDocument *doc = new DynamicJsonDocument(4096);
    if (!doc) {
        Serial.println("Failed to allocate JSON doc");
        return;
    }

    File file_r = LittleFS.open(WIFI_CREDS_FILE, "r");
    if (file_r) {
        deserializeJson(*doc, file_r);
        file_r.close();
    }
    
    // Check if exists
    const char * pass = lv_textarea_get_text(password_ta);
    JsonArray networks = (*doc)["networks"];
    if (!networks.isNull()) {
        for (JsonObject net : networks) {
            const char* s = net["ssid"];
            if (strcmp(s, selected_ssid) == 0) {
                net["pass"] = pass; // Update existing
                File file_w = LittleFS.open(WIFI_CREDS_FILE, "w");
                serializeJson(*doc, file_w);
                file_w.close();
                delete doc;
                return;
            }
        }
    } else {
        networks = doc->createNestedArray("networks");
    }

    // Add new
    JsonObject new_net = networks.createNestedObject();
    new_net["ssid"] = selected_ssid;
    new_net["pass"] = pass;

    File file_w = LittleFS.open(WIFI_CREDS_FILE, "w");
    if (file_w) {
        serializeJson(*doc, file_w);
        file_w.close();
        Serial.println("Saved Network Credentials");
    }
    delete doc;
}

// Helper: Load Saved Networks into the 'saved_list'
static void load_saved_networks() {
    if(!saved_list) return; // Safety check
    
    lv_obj_clean(saved_list);
    lv_list_add_text(saved_list, "Saved Networks");

    if(!LittleFS.exists(WIFI_CREDS_FILE)) {
        lv_list_add_text(saved_list, "No saved networks.");
        return;
    }

    File file = LittleFS.open(WIFI_CREDS_FILE, "r");
    if (!file) {
        lv_list_add_text(saved_list, "Error opening file.");
        return;
    }

    DynamicJsonDocument *doc = new DynamicJsonDocument(4096);
    if (!doc) {
        file.close();
        lv_list_add_text(saved_list, "Mem Error");
        return;
    }

    DeserializationError error = deserializeJson(*doc, file);
    file.close();

    if (error) {
        lv_list_add_text(saved_list, "File Corrupt");
        delete doc;
        return;
    }

    JsonArray networks = (*doc)["networks"];
    if (networks.isNull() || networks.size() == 0) {
        lv_list_add_text(saved_list, "No saved networks.");
    } else {
        for (JsonObject net : networks) {
            const char* ssid = net["ssid"];
            const char* pass = net["pass"];
            
            // Create list button
            lv_obj_t * btn = lv_list_add_btn(saved_list, LV_SYMBOL_WIFI, ssid);
            
            // Allocate memory for password to persist beyond this function scope (and doc scope)
            char * pass_copy = strdup(pass); 
            
            lv_obj_add_event_cb(btn, [](lv_event_t * e){
                lv_obj_t * btn = lv_event_get_target(e);
                String txt = lv_list_get_btn_text(saved_list, btn);
                char * p = (char*)lv_event_get_user_data(e);
                
                if (p) {
                    strncpy(selected_ssid, txt.c_str(), 32);
                    lv_textarea_set_text(password_ta, p); // Fill password
                    
                    lv_label_set_text_fmt(status_label, "Connecting to Saved: %s", selected_ssid);
                    ui_if_epd_refr(EPD_REFRESH_TIME);
                    
                    if (wifi_timer) lv_timer_del(wifi_timer);
                    WiFi.begin(selected_ssid, p);
                    wifi_timeout_counter = 0;
                    wifi_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
                }
            }, LV_EVENT_CLICKED, pass_copy);
        }
    }
    delete doc;
}

static void ta_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
    
    if(code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        ui_if_epd_refr(EPD_REFRESH_TIME);
    }

    if(code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        ui_if_epd_refr(EPD_REFRESH_TIME);
    }
    
    // Auto-hide keyboard on check/cancel (Enter/Close check)
    if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_indev_reset(NULL, ta);  
        ui_if_epd_refr(EPD_REFRESH_TIME); 
    }

    // Refresh display on every character input
    if(code == LV_EVENT_VALUE_CHANGED) {
        ui_if_epd_refr(EPD_REFRESH_TIME);
    }
}

static void wifi_timer_cb(lv_timer_t * timer) {
    wifi_timeout_counter++;
    
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text_fmt(status_label, "Connected to %s!", selected_ssid);
        
        // SAVE if Checkbox checked
        if (lv_obj_get_state(cb_remember) & LV_STATE_CHECKED) {
            save_current_network();
            load_saved_networks(); // Refresh list logic
        }

        ui_if_epd_refr(EPD_REFRESH_TIME);
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    } else if (wifi_timeout_counter > 20) { 
        lv_label_set_text(status_label, "Connection Failed (Timeout)");
        ui_if_epd_refr(EPD_REFRESH_TIME);
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    } else if (WiFi.status() == WL_CONNECT_FAILED) {
        lv_label_set_text(status_label, "Connection Failed");
        ui_if_epd_refr(EPD_REFRESH_TIME);
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    }
}

// Timer to perform the actual scan after UI update
static void scan_timer_cb(lv_timer_t * t) {
    int n = WiFi.scanNetworks();
    
    lv_obj_clean(ssid_list);
    
    // Hide Saved List, Show Scanned List
    lv_obj_add_flag(saved_list, LV_OBJ_FLAG_HIDDEN); 
    lv_obj_clear_flag(ssid_list, LV_OBJ_FLAG_HIDDEN); // Make sure this is shown

    if (n == 0) {
        lv_label_set_text(status_label, "No networks found.");
        lv_list_add_text(ssid_list, "No SSIDs Found"); 
    } else {
        lv_label_set_text_fmt(status_label, "Found %d networks", n);
        for (int i = 0; i < n && i < 10; ++i) {
            String ssid = WiFi.SSID(i);
            lv_obj_t * btn = lv_list_add_btn(ssid_list, LV_SYMBOL_WIFI, ssid.c_str());
            lv_obj_add_event_cb(btn, [](lv_event_t * e){
                lv_obj_t * btn = lv_event_get_target(e);
                String txt = lv_list_get_btn_text(ssid_list, btn);
                strncpy(selected_ssid, txt.c_str(), 32);
                
                lv_textarea_set_text(password_ta, "");
                lv_label_set_text_fmt(status_label, "Selected: %s", selected_ssid);
                
                // Show Input UI
                lv_obj_clear_flag(password_ta, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
                
                // Show "Remember" Checkbox
                lv_obj_clear_flag(cb_remember, LV_OBJ_FLAG_HIDDEN);
                
                // FORCE FOCUS Logic
                lv_obj_add_state(password_ta, LV_STATE_FOCUSED);
                lv_keyboard_set_textarea(keyboard, password_ta);
                lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER); 
                
                ui_if_epd_refr(EPD_REFRESH_TIME);
            }, LV_EVENT_CLICKED, NULL);
        }
    }
    
    WiFi.scanDelete();
    ui_if_epd_refr(EPD_REFRESH_TIME);
    lv_timer_del(t); 
}

static void refresh_networks_event_cb(lv_event_t * e) {
    lv_obj_clean(ssid_list);
    lv_label_set_text(status_label, "Scanning...");
    ui_if_epd_refr(EPD_REFRESH_TIME);
    
    // Schedule the scan to happen AFTER this frame renders
    lv_timer_create(scan_timer_cb, 200, NULL);
}

static void connect_event_cb(lv_event_t * e) {
    const char * pass = lv_textarea_get_text(password_ta);
    if (strlen(selected_ssid) == 0) {
        lv_label_set_text(status_label, "Error: No SSID selected.");
        ui_if_epd_refr(EPD_REFRESH_TIME);
        return;
    }
    
    if (wifi_timer) {
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    }
    
    lv_label_set_text_fmt(status_label, "Connecting to %s...", selected_ssid);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(password_ta, LV_OBJ_FLAG_HIDDEN);
    ui_if_epd_refr(EPD_REFRESH_TIME);
    
    WiFi.begin(selected_ssid, pass);
    
    wifi_timeout_counter = 0;
    wifi_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
}

static void disconnect_event_cb(lv_event_t * e) {
    if (wifi_timer) {
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    }
    WiFi.disconnect();
    lv_label_set_text(status_label, "Disconnected.");
    ui_if_epd_refr(EPD_REFRESH_TIME);
}

void create_wifi_custom_app(lv_obj_t * parent) {
    // Status Label at top
    status_label = lv_label_create(parent);
    lv_label_set_text(status_label, WiFi.isConnected() ? "Connected" : "Not Connected");
    lv_obj_set_style_text_font(status_label, &Font_Mono_Bold_20, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 15);

    // SSID List (Hidden initially? No, we show SAVED list initially)
    // We reuse ssid_list for scan results, and saved_list for saved
    
    // 1. Saved List (Visible by Default) gets the "empty space" initially
    saved_list = lv_list_create(parent);
    lv_obj_set_size(saved_list, 900, 190); 
    lv_obj_align(saved_list, LV_ALIGN_TOP_MID, 0, 310); // Bottom area where keyboard usually goes? 
    
    // 2. Scan Results List (Top half)
    ssid_list = lv_list_create(parent);
    lv_obj_set_size(ssid_list, 900, 190); 
    lv_obj_align(ssid_list, LV_ALIGN_TOP_MID, 0, 45);
    
    // Buttons
    int btn_y = 250;
    
    btn_scan = lv_btn_create(parent);
    lv_obj_set_size(btn_scan, 180, 50);
    lv_obj_align(btn_scan, LV_ALIGN_TOP_LEFT, 30, btn_y);
    lv_obj_t *label_scan = lv_label_create(btn_scan);
    lv_label_set_text(label_scan, "Scan");
    lv_obj_center(label_scan);
    lv_obj_add_event_cb(btn_scan, refresh_networks_event_cb, LV_EVENT_CLICKED, NULL);
    
    btn_connect = lv_btn_create(parent);
    lv_obj_set_size(btn_connect, 180, 50);
    lv_obj_align(btn_connect, LV_ALIGN_TOP_MID, 0, btn_y);
    lv_obj_t *label_connect = lv_label_create(btn_connect);
    lv_label_set_text(label_connect, "Connect");
    lv_obj_center(label_connect);
    lv_obj_add_event_cb(btn_connect, connect_event_cb, LV_EVENT_CLICKED, NULL);

    // "Remember" Checkbox (Next to Connect?)
    cb_remember = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb_remember, "Remember");
    lv_obj_align(cb_remember, LV_ALIGN_TOP_MID, 120, btn_y + 15); // Offset to right of Connect
    lv_obj_add_flag(cb_remember, LV_OBJ_FLAG_HIDDEN); // Hidden until needed

    btn_disconnect = lv_btn_create(parent);
    lv_obj_set_size(btn_disconnect, 180, 50);
    lv_obj_align(btn_disconnect, LV_ALIGN_TOP_RIGHT, -30, btn_y);
    lv_obj_t *label_disconnect = lv_label_create(btn_disconnect);
    lv_label_set_text(label_disconnect, "Disconnect");
    lv_obj_center(label_disconnect);
    lv_obj_add_event_cb(btn_disconnect, disconnect_event_cb, LV_EVENT_CLICKED, NULL);

    // Keyboard (Created BEFORE TA to be passed in event?) 
    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 960, 180);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0); // Y=360
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    // Password Text Area
    password_ta = lv_textarea_create(parent);
    lv_obj_set_size(password_ta, 900, 45); 
    lv_obj_align(password_ta, LV_ALIGN_TOP_MID, 0, 310);
    lv_textarea_set_placeholder_text(password_ta, "Password");
    lv_textarea_set_one_line(password_ta, true);
    lv_obj_add_flag(password_ta, LV_OBJ_FLAG_HIDDEN);
    
    // Attach event callback to TA to manage keyboard
    lv_obj_add_event_cb(password_ta, ta_event_cb, LV_EVENT_ALL, keyboard);
    
    // Back Button with Custom Style for "WiFi Mgr"
    lv_obj_t * btn_back = lv_btn_create(parent);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_set_height(btn_back, 50);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_obj_set_style_border_width(btn_back, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, [](lv_event_t * e){
        if (wifi_timer) {
            lv_timer_del(wifi_timer);
            wifi_timer = NULL;
        }
        scr_mgr_switch(SCREEN0_ID, false);
        ui_if_epd_refr(EPD_REFRESH_TIME);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_arrow = lv_label_create(btn_back);
    lv_obj_align(label_arrow, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(label_arrow, lv_color_hex(EPD_COLOR_TEXT), LV_PART_MAIN);
    lv_label_set_text(label_arrow, " " LV_SYMBOL_LEFT);

    lv_obj_t *label_title = lv_label_create(parent);
    lv_obj_align_to(label_title, btn_back, LV_ALIGN_OUT_RIGHT_MID, 10, -5); // Moved up slightly
    lv_obj_set_style_text_font(label_title, &Font_Mono_Bold_20, LV_PART_MAIN); // Smaller font (20 vs 30)
    lv_obj_set_style_text_color(label_title, lv_color_hex(EPD_COLOR_TEXT), LV_PART_MAIN);
    lv_label_set_text(label_title, "WiFi Mgr");
    lv_obj_add_flag(label_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_title, [](lv_event_t * e){
        if (wifi_timer) {
            lv_timer_del(wifi_timer);
            wifi_timer = NULL;
        }
        scr_mgr_switch(SCREEN0_ID, false);
        ui_if_epd_refr(EPD_REFRESH_TIME);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(label_title, 30);
    
    // Init: Load saved networks
    load_saved_networks();
}
