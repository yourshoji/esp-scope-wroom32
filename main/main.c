#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_continuous.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "nvs_flash.h"

static const char* TAG = "ESP-SCOPE";

// --- WROOM-32D HARDWARE SETTINGS ---
// Don't lower this. The classic ESP32 ADC driver freaks out/crashes below 20kHz.
#define MIN_SAMPLE_RATE         20000 

// Moved from GPIO 1 because that killed my Serial logs. 18 is safe.
#define TEST_SIGNAL_PIN         18
// DISPLAYED_SIGNAL_PIN         36 (VP, ADC1_0) * Just a reminder, in case you forgot, alr?
#define STATUS_LED_PIN          2     
#define BOOT_BUTTON_PIN         0     

// WROOM/Classic ESP32 Specifics
// We MUST use Type 1 for single unit mode. Type 2 is for S3/C3 chips.
#define ADC_UNIT                ADC_UNIT_1
#define ADC_CONV_MODE           ADC_CONV_SINGLE_UNIT_1
#define ADC_OUTPUT_TYPE         ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define ADC_GET_DATA(p_data)    ((p_data)->type1.data) 
#define ADC_READ_LEN            4096

// Embedded files
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_end");

// Globals
static httpd_handle_t s_server = NULL;
static adc_continuous_handle_t adc_handle = NULL;
static int client_fd = -1; // Socket for the connected browser
static volatile bool need_reconfig = false;
static bool is_ap_mode = false;

// Defaults
static uint32_t s_sample_rate = MIN_SAMPLE_RATE;
static adc_atten_t s_atten = ADC_ATTEN_DB_12;
static uint16_t s_test_hz = 100;

// Forward decls
static void adc_init_hardware(adc_channel_t* channel, uint8_t channel_num);
static void start_webserver(void);

// Just aligns buffer size to 4 bytes so DMA is happy
static uint32_t calc_buffer_size(uint32_t rate) {
    uint32_t bytes = rate * sizeof(adc_digi_output_data_t);
    uint32_t size = bytes / 50; // ~20ms chunks

    if (size < 128) size = 128;
    if (size > ADC_READ_LEN) size = ADC_READ_LEN;

    return (size + 3) & ~3;
}

static void adc_task(void* arg) {
    esp_err_t ret;
    uint32_t ret_num = 0;
    
    // Huge buffer -> moved to static so we don't smash the stack
    static uint8_t raw_data[ADC_READ_LEN] = {0};
    static int skip_count = 0;
    
    // Setup ADC on Ch0 (GPIO 36)
    adc_channel_t chans[1] = {ADC_CHANNEL_0};
    adc_init_hardware(chans, 1);
    adc_continuous_start(adc_handle);

    while (1) {
        if (need_reconfig) {
            // Stop everything, re-init, restart. 
            if (adc_handle) {
                adc_continuous_stop(adc_handle);
                adc_continuous_deinit(adc_handle);
                adc_handle = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(20)); // Let the hardware settle
            adc_init_hardware(chans, 1);
            adc_continuous_start(adc_handle);
            need_reconfig = false;
        }

        ret = adc_continuous_read(adc_handle, raw_data, calc_buffer_size(s_sample_rate), &ret_num, 0);

        if (ret == ESP_OK) {
            // Only convert data if someone is actually watching
            if (client_fd != -1) {
                
                // Throttle: Send every 2nd frame so WiFi doesn't choke
                if (++skip_count < 2) {
                    // Skip
                } else {
                    skip_count = 0;
                    
                    static uint16_t json_data[ADC_READ_LEN / 4];
                    int idx = 0;

                    for (int i = 0; i < ret_num; i += sizeof(adc_digi_output_data_t)) {
                        adc_digi_output_data_t* p = (adc_digi_output_data_t*)&raw_data[i];
                        // Using the Type 1 macro from top of file
                        json_data[idx++] = (uint16_t)ADC_GET_DATA(p);
                    }

                    if (idx > 0) {
                        httpd_ws_frame_t ws_pkt = {
                            .final = true,
                            .fragmented = false,
                            .type = HTTPD_WS_TYPE_BINARY,
                            .payload = (uint8_t*)json_data,
                            .len = idx * sizeof(uint16_t)
                        };

                        // Async send is safer for high frequency
                        esp_err_t ws_ret = httpd_ws_send_frame_async(s_server, client_fd, &ws_pkt);

                        if (ws_ret != ESP_OK) {
                            if (ws_ret == ESP_ERR_INVALID_ARG) {
                                // Client probably closed tab
                                client_fd = -1;
                            }
                            // If it's just busy, we drop the frame. It's fine for a scope.
                        } else {
                            // Give the network stack a tiny break
                            vTaskDelay(pdMS_TO_TICKS(1)); 
                        }
                    }
                }
            }
            taskYIELD(); // Crucial: prevents watchdog timeout
        } else if (ret == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void adc_init_hardware(adc_channel_t* channel, uint8_t channel_num) {
    uint32_t frame_size = calc_buffer_size(s_sample_rate);
    
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 16384,
        .conv_frame_size = frame_size,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &adc_handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = s_sample_rate,
        .conv_mode = ADC_CONV_MODE,
        .format = ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;

    for (int i = 0; i < channel_num; i++) {
        adc_pattern[i].atten = s_atten;
        adc_pattern[i].channel = channel[i] & 0x7;
        adc_pattern[i].unit = ADC_UNIT;
        adc_pattern[i].bit_width = ADC_BITWIDTH_12;
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));
}

// Simple PWM for testing
static void enable_test_signal(uint32_t hz) {
    static bool is_setup = false;
    if (is_setup) {
        // Reset if we change freq
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    }
    
    // Timer 0, Chan 0, Pin 18
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t chan = {
        .gpio_num = TEST_SIGNAL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 4096, // ~25% duty cycle
        .hpoint = 0
    };
    ledc_channel_config(&chan);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    is_setup = true;
    ESP_LOGI(TAG, "PWM Signal on GPIO %d @ %" PRIu32 "Hz", TEST_SIGNAL_PIN, hz);
}

// Handles the status LED + Boot button factory reset
static void ui_task() {
    gpio_set_direction(STATUS_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);

    int64_t press_start = 0;

    while (true) {
        // LED Logic
        if (is_ap_mode) {
            // Slow blink in AP mode
            gpio_set_level(STATUS_LED_PIN, 1); vTaskDelay(500 / portTICK_PERIOD_MS);
            gpio_set_level(STATUS_LED_PIN, 0); vTaskDelay(500 / portTICK_PERIOD_MS);
        } else if (client_fd != -1) {
            // Fast blink if user is connected
            gpio_set_level(STATUS_LED_PIN, 1); vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(STATUS_LED_PIN, 0); vTaskDelay(100 / portTICK_PERIOD_MS);
        } else {
            // Short blip if connected to WiFi but idle
            gpio_set_level(STATUS_LED_PIN, 1); vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(STATUS_LED_PIN, 0); vTaskDelay(900 / portTICK_PERIOD_MS);
        }

        // Button Logic (Hold 3s to reset)
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            if (press_start == 0) press_start = esp_timer_get_time();
            if ((esp_timer_get_time() - press_start) > 3000000) {
                 ESP_LOGW(TAG, "Factory Reset...");
                 wifi_manager_erase_config();
                 esp_restart();
            }
        } else {
            press_start = 0;
        }
    }
}

void app_main(void) {
    // 1. Setup NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. Start WiFi (Manager handles the AP/STA logic)
    is_ap_mode = wifi_manager_init_wifi();

    // 3. Start hardware
    enable_test_signal(s_test_hz);
    
    // Lower priority than WiFi so we don't starve the network
    xTaskCreate(adc_task, "adc_reader", 4096, NULL, 2, NULL);
    
    start_webserver();
    
    // UI loop (LEDs/Buttons) - just running it here in main
    ui_task(); 
}

// --- WEB STUFF BELOW ---

static esp_err_t ws_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) return ESP_OK; // Handshake

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Read headers first to get length
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) return ESP_FAIL;

    if (ws_pkt.len) {
        uint8_t* buf = calloc(1, ws_pkt.len + 1);
        ws_pkt.payload = buf;
        
        httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);

        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && strstr((char*)ws_pkt.payload, "hello")) {
            ESP_LOGI(TAG, "Client connected");
            client_fd = httpd_req_to_sockfd(req);
        }
        free(buf);
    }
    return ESP_OK;
}

static esp_err_t params_handler(httpd_req_t* req) {
    char buf[200];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        cJSON* root = cJSON_Parse(buf);
        if (root) {
            cJSON* rate = cJSON_GetObjectItem(root, "sample_rate");
            if (rate) {
                // Safety clamp
                s_sample_rate = (rate->valueint < MIN_SAMPLE_RATE) ? MIN_SAMPLE_RATE : rate->valueint;
                need_reconfig = true;
            }
            
            cJSON* atten = cJSON_GetObjectItem(root, "atten");
            if (atten) { 
                s_atten = (adc_atten_t)atten->valueint; 
                need_reconfig = true; 
            }
            
            cJSON* thz = cJSON_GetObjectItem(root, "test_hz");
            if (thz) { 
                s_test_hz = thz->valueint; 
                enable_test_signal(s_test_hz); 
            }
            cJSON_Delete(root);
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t serve_index(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start);
}

static esp_err_t serve_js(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/javascript");
    return httpd_resp_send(req, (const char*)index_js_start, index_js_end - index_js_start);
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    
    // Start server
    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t u_idx = { .uri = "/", .method = HTTP_GET, .handler = serve_index };
        httpd_uri_t u_js = { .uri = "/index.js", .method = HTTP_GET, .handler = serve_js };
        httpd_uri_t u_ws = { .uri = "/signal", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
        httpd_uri_t u_api = { .uri = "/params", .method = HTTP_POST, .handler = params_handler };

        httpd_register_uri_handler(s_server, &u_idx);
        httpd_register_uri_handler(s_server, &u_js);
        httpd_register_uri_handler(s_server, &u_ws);
        httpd_register_uri_handler(s_server, &u_api);
        
        // Let the wifi manager add its own pages too
        wifi_manager_register_uri(s_server);
    }
}