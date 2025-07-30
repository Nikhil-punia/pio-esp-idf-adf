/* Play an MP3, AAC or WAV file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "equalizer.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "audio_alc.h"
#include "led_strip.h"

#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

// Include all possible decoders for auto-detection
#include "aac_decoder.h"
#include "amr_decoder.h"
#include "flac_decoder.h"
#include "mp3_decoder.h"
#include "ogg_decoder.h"
#include "wav_decoder.h"

// Audio format detection enumeration
typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_AAC,
    AUDIO_FORMAT_FLAC,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_OGG,
    AUDIO_FORMAT_AMR
} audio_format_t;

// Global handles for the audio task
static audio_pipeline_handle_t global_pipeline = NULL;
static audio_element_handle_t global_http_stream = NULL;
static audio_element_handle_t global_decoder = NULL;
static audio_element_handle_t global_equalizer = NULL;
static audio_element_handle_t global_i2s_stream = NULL;
static audio_event_iface_handle_t evt = NULL;

// Current audio format tracking
static audio_format_t current_audio_format = AUDIO_FORMAT_UNKNOWN;
static char current_decoder_name[16] = "unknown";

// LED strip handle for ESP32-S3-N8R8 DevKit WS2812 RGB LED
static led_strip_handle_t led_strip = NULL;

// RGB LED configuration for ESP32-S3-N8R8 DevKit (using WS2812 RGB LED on GPIO38)
#define RGB_LED_GPIO 38  // GPIO38 for ESP32-S3-N8R8 DevKit RGB LED (WS2812)

// WiFi event group and status
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// HTTP Server and stream control
static httpd_handle_t server = NULL;
static char current_stream_url[512] = "";
static bool stream_change_requested = false;
static bool pipeline_restarting = false;
static SemaphoreHandle_t stream_control_mutex = NULL;
static int current_volume = 70;  // Default volume (0-100)
static bool volume_change_requested = false;

// Equalizer settings (10-band EQ, -13 to +13 dB)
static int equalizer_gains[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // Default flat response
static bool equalizer_change_requested = false;

// LED control settings
static int led_color_red = 128;
static int led_color_green = 0;
static int led_color_blue = 128;
static bool led_manual_mode = false;
static bool led_change_requested = false;

// Sample stream URLs for different formats
static const char *sample_streams[] = {
    "https://stream.radioparadise.com/flac",           // FLAC stream
    "https://stream.zeno.fm/vq6p5vxb4v8uv",           // MP3 radio stream
    "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac", // AAC file
    "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.wav", // WAV file
};

// Application tag for logging
static const char *TAG = "HTTP_AUTO_DECODER_EXAMPLE";

// Audio format detection based on URL extension and content-type
static audio_format_t detect_audio_format_from_url(const char* url)
{
    if (!url) return AUDIO_FORMAT_UNKNOWN;
    
    // Convert to lowercase for comparison
    char *url_lower = strdup(url);
    if (!url_lower) return AUDIO_FORMAT_UNKNOWN;
    
    for (int i = 0; url_lower[i]; i++) {
        url_lower[i] = tolower(url_lower[i]);
    }
    
    audio_format_t format = AUDIO_FORMAT_UNKNOWN;
    
    // Check file extension in URL
    if (strstr(url_lower, ".mp3") || strstr(url_lower, "mp3")) {
        format = AUDIO_FORMAT_MP3;
    } else if (strstr(url_lower, ".flac") || strstr(url_lower, "flac")) {
        format = AUDIO_FORMAT_FLAC;
    } else if (strstr(url_lower, ".aac") || strstr(url_lower, "aac")) {
        format = AUDIO_FORMAT_AAC;
    } else if (strstr(url_lower, ".wav") || strstr(url_lower, "wav")) {
        format = AUDIO_FORMAT_WAV;
    } else if (strstr(url_lower, ".ogg") || strstr(url_lower, "ogg")) {
        format = AUDIO_FORMAT_OGG;
    } else if (strstr(url_lower, ".amr") || strstr(url_lower, "amr")) {
        format = AUDIO_FORMAT_AMR;
    } else {
        // Try to guess from known streaming services
        if (strstr(url_lower, "radioparadise.com")) {
            format = AUDIO_FORMAT_FLAC;  // Radio Paradise typically streams FLAC
        } else if (strstr(url_lower, "zeno.fm") || strstr(url_lower, "icecast") || 
                   strstr(url_lower, "shoutcast") || strstr(url_lower, "radio")) {
            format = AUDIO_FORMAT_MP3;   // Most internet radio is MP3
        } else {
            format = AUDIO_FORMAT_MP3;   // Default fallback to MP3
        }
    }
    
    free(url_lower);
    return format;
}

// Get decoder name string from format
static const char* get_decoder_name(audio_format_t format)
{
    switch (format) {
        case AUDIO_FORMAT_MP3:   return "mp3";
        case AUDIO_FORMAT_AAC:   return "aac";
        case AUDIO_FORMAT_FLAC:  return "flac";
        case AUDIO_FORMAT_WAV:   return "wav";
        case AUDIO_FORMAT_OGG:   return "ogg";
        case AUDIO_FORMAT_AMR:   return "amr";
        default:                 return "unknown";
    }
}

// Create decoder element based on detected format
static audio_element_handle_t create_decoder_for_format(audio_format_t format)
{
    audio_element_handle_t decoder = NULL;
    
    switch (format) {
        case AUDIO_FORMAT_MP3: {
            mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
            mp3_cfg.out_rb_size = 512 * 1024;
            mp3_cfg.task_stack = 4096;  // Increase stack size
            mp3_cfg.task_prio = 5;      // Set priority
            mp3_cfg.task_core = 1;      // Pin to core 1
            decoder = mp3_decoder_init(&mp3_cfg);
            break;
        }
        case AUDIO_FORMAT_AAC: {
            aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
            aac_cfg.out_rb_size = 512 * 1024;
            aac_cfg.task_stack = 4096;  // Increase stack size
            aac_cfg.task_prio = 5;      // Set priority
            aac_cfg.task_core = 1;      // Pin to core 1
            decoder = aac_decoder_init(&aac_cfg);
            break;
        }
        case AUDIO_FORMAT_FLAC: {
            flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
            flac_cfg.out_rb_size = 1024 * 1024;  // FLAC needs more buffer
            flac_cfg.task_stack = 6144;  // FLAC needs even more stack
            flac_cfg.task_prio = 5;      // Set priority
            flac_cfg.task_core = 1;      // Pin to core 1
            decoder = flac_decoder_init(&flac_cfg);
            break;
        }
        case AUDIO_FORMAT_WAV: {
            wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
            wav_cfg.out_rb_size = 256 * 1024;
            wav_cfg.task_stack = 4096;  // Increase stack size
            wav_cfg.task_prio = 5;      // Set priority
            wav_cfg.task_core = 1;      // Pin to core 1
            decoder = wav_decoder_init(&wav_cfg);
            break;
        }
        case AUDIO_FORMAT_OGG: {
            ogg_decoder_cfg_t ogg_cfg = DEFAULT_OGG_DECODER_CONFIG();
            ogg_cfg.out_rb_size = 512 * 1024;
            ogg_cfg.task_stack = 4096;  // Increase stack size
            ogg_cfg.task_prio = 5;      // Set priority
            ogg_cfg.task_core = 1;      // Pin to core 1
            decoder = ogg_decoder_init(&ogg_cfg);
            break;
        }
        case AUDIO_FORMAT_AMR: {
            amr_decoder_cfg_t amr_cfg = DEFAULT_AMR_DECODER_CONFIG();
            amr_cfg.out_rb_size = 256 * 1024;
            amr_cfg.task_stack = 4096;  // Increase stack size
            amr_cfg.task_prio = 5;      // Set priority
            amr_cfg.task_core = 1;      // Pin to core 1
            decoder = amr_decoder_init(&amr_cfg);
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown audio format, falling back to MP3 decoder");
            mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
            mp3_cfg.out_rb_size = 512 * 1024;
            mp3_cfg.task_stack = 4096;  // Increase stack size
            mp3_cfg.task_prio = 5;      // Set priority
            mp3_cfg.task_core = 1;      // Pin to core 1
            decoder = mp3_decoder_init(&mp3_cfg);
            break;
    }
    
    return decoder;
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI("wifi_idf", "WiFi disconnected, trying to reconnect...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("wifi_idf", "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi
static void init_wifi(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Groid",
            .password = "ghotu440@440",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("wifi_idf", "WiFi initialization completed. Waiting for connection...");
}

// Initialize RGB LED using official ESP-IDF led_strip component
static void init_rgb_led(void)
{
    ESP_LOGI("RGB_LED", "Initializing WS2812 RGB LED on GPIO%d using RMT", RGB_LED_GPIO);
    
    // LED strip configuration using RMT peripheral
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,        // GPIO for LED strip data pin
        .max_leds = 1,             // Only 1 LED on the devkit
        .led_model = LED_MODEL_WS2812,  // WS2812 LED model          
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // WS2812 uses GRB format
        .flags.invert_out = false,             // Don't invert output
    };
    
    // RMT configuration for LED strip
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,        // Use default RMT clock source
        .resolution_hz = 10 * 1000 * 1000,    // 10MHz resolution for precise timing
        .flags.with_dma = false,               // DMA not needed for single LED
    };
    
    // Create LED strip handle
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE("RGB_LED", "Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }
    
    // Clear LED (turn off)
    ret = led_strip_clear(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE("RGB_LED", "Failed to clear LED: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI("RGB_LED", "✅ WS2812 RGB LED initialized successfully with RMT");
}

// Set LED color based on stream type and status using official led_strip component
static void update_led_for_stream(const char* url, bool is_playing)
{
    if (!led_strip) {
        return; // LED strip not initialized
    }
    
    // If in manual mode, don't auto-update LED
    if (led_manual_mode) {
        return;
    }
    
    if (!is_playing) {
        // Off when not playing
        led_strip_clear(led_strip);
        led_strip_refresh(led_strip);
        return;
    }
    
    // Determine color based on stream content
    if (strstr(url, ".flac") || strstr(url, "flac")) {
        // Purple for FLAC (high quality)
        led_strip_set_pixel(led_strip, 0, 128, 0, 128);
    } else if (strstr(url, ".mp3") || strstr(url, "mp3")) {
        // Blue for MP3
        led_strip_set_pixel(led_strip, 0, 0, 0, 255);
    } else if (strstr(url, ".wav") || strstr(url, "wav")) {
        // Green for WAV
        led_strip_set_pixel(led_strip, 0, 0, 255, 0);
    } else if (strstr(url, ".aac") || strstr(url, "aac")) {
        // Orange for AAC
        led_strip_set_pixel(led_strip, 0, 255, 165, 0);
    } else if (strstr(url, "zeno.fm") || strstr(url, "radio")) {
        // Red for radio streams
        led_strip_set_pixel(led_strip, 0, 255, 0, 0);
    } else {
        // White for unknown formats
        led_strip_set_pixel(led_strip, 0, 255, 255, 255);
    }
    
    // Refresh the LED to show the new color
    led_strip_refresh(led_strip);
}

// Set LED color manually
static void set_led_color_manual(int red, int green, int blue)
{
    if (!led_strip) {
        return;
    }
    
    led_strip_set_pixel(led_strip, 0, red, green, blue);
    led_strip_refresh(led_strip);
    ESP_LOGI("RGB_LED", "💡 LED color set to R:%d G:%d B:%d", red, green, blue);
}

// HTTP Server handlers
static esp_err_t root_handler(httpd_req_t *req)
{
    const char* html_page = 
        "<!DOCTYPE html>"
        "<html><head><title>ESP32-S3 Audio Streamer</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;}"
        ".container{max-width:800px;margin:auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}"
        "h1{color:#333;text-align:center;}"
        ".current{background:#e8f5e8;padding:15px;border-radius:5px;margin:10px 0;}"
        ".form-group{margin:15px 0;}"
        "label{display:block;margin-bottom:5px;font-weight:bold;}"
        "input[type=text]{width:100%;padding:10px;border:1px solid #ddd;border-radius:5px;box-sizing:border-box;}"
        "input[type=range]{width:100%;margin:10px 0;}"
        ".volume-control{background:#f8f9fa;padding:15px;border-radius:5px;margin:15px 0;border:1px solid #dee2e6;}"
        ".volume-display{text-align:center;font-size:18px;font-weight:bold;color:#007bff;margin:5px 0;}"
        ".equalizer-control{background:#f8f9fa;padding:15px;border-radius:5px;margin:15px 0;border:1px solid #dee2e6;}"
        ".eq-bands{display:grid;grid-template-columns:repeat(10,1fr);gap:10px;margin:10px 0;}"
        ".eq-band{text-align:center;}"
        ".eq-band label{font-size:12px;margin-bottom:5px;color:#666;}"
        ".eq-band input[type=range]{writing-mode:bt-lr;-webkit-appearance:slider-vertical;width:30px;height:100px;background:#ddd;}"
        "button{background:#007bff;color:white;padding:12px 24px;border:none;border-radius:5px;cursor:pointer;font-size:16px;}"
        "button:hover{background:#0056b3;}"
        ".presets{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px;margin:20px 0;}"
        ".preset{background:#f8f9fa;padding:10px;border-radius:5px;border:1px solid #dee2e6;cursor:pointer;text-align:center;}"
        ".preset:hover{background:#e9ecef;}"
        ".status{text-align:center;margin:20px 0;padding:10px;border-radius:5px;}"
        ".info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb;}"
        "</style></head><body>"
        "<div class='container'>"
        "<h1> ESP32-S3 Audio Streamer Control</h1>"
        "<div class='status info'>Current Stream: <strong>%s</strong></div>"
        ""
        "<div class='volume-control'>"
        "<label for='volume'> Volume Control:</label>"
        "<div class='volume-display' id='volumeDisplay'>%d%%</div>"
        "<input type='range' id='volume' min='0' max='100' value='%d' oninput='updateVolume(this.value)'>"
        "</div>"
        ""
        "<div class='equalizer-control'>"
        "<label> 10-Band Equalizer (±13dB):</label>"
        "<div class='eq-bands'>"
        "<div class='eq-band'><label>32Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(0,this.value)'></div>"
        "<div class='eq-band'><label>64Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(1,this.value)'></div>"
        "<div class='eq-band'><label>125Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(2,this.value)'></div>"
        "<div class='eq-band'><label>250Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(3,this.value)'></div>"
        "<div class='eq-band'><label>500Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(4,this.value)'></div>"
        "<div class='eq-band'><label>1kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(5,this.value)'></div>"
        "<div class='eq-band'><label>2kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(6,this.value)'></div>"
        "<div class='eq-band'><label>4kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(7,this.value)'></div>"
        "<div class='eq-band'><label>8kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(8,this.value)'></div>"
        "<div class='eq-band'><label>16kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(9,this.value)'></div>"
        "</div>"
        "<button type='button' onclick='resetEqualizer()'>Reset EQ</button>"
        "</div>"
        ""
        "<div class='equalizer-control'>"
        "<label>💡 RGB LED Control:</label>"
        "<div style='margin:10px 0;'>"
        "<label><input type='checkbox' id='ledManual' onchange='toggleLedMode(this.checked)'> Manual LED Control</label>"
        "</div>"
        "<div id='ledControls' style='display:none;'>"
        "<div style='display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin:10px 0;'>"
        "<div><label>Green (0-255):</label><input type='range' id='ledGreen' min='0' max='255' value='128' oninput='updateLedColor()'></div>"
        "<div><label>Red (0-255):</label><input type='range' id='ledRed' min='0' max='255' value='0' oninput='updateLedColor()'></div>"
        "<div><label>Blue (0-255):</label><input type='range' id='ledBlue' min='0' max='255' value='128' oninput='updateLedColor()'></div>"
        "</div>"
        "<div style='text-align:center;margin:10px 0;'>"
        "<button type='button' onclick='setLedPreset(255,0,0)'>Green</button> "
        "<button type='button' onclick='setLedPreset(0,255,0)'>Red</button> "
        "<button type='button' onclick='setLedPreset(0,0,255)'>Blue</button> "
        "<button type='button' onclick='setLedPreset(255,255,255)'>White</button> "
        "<button type='button' onclick='setLedPreset(0,0,0)'>Off</button>"
        "</div>"
        "</div>"
        "</div>"
        ""
        "<form action='/set_stream' method='get'>"
        "<div class='form-group'>"
        "<label for='url'>Stream URL:</label>"
        "<input type='text' id='url' name='url' placeholder='http://example.com/stream.mp3' required>"
        "</div>"
        "<button type='submit'>Change Stream</button>"
        "</form>"
        "<h3>Quick Presets (Auto-Detected Format):</h3>"
        "<div class='presets'>"
        "<div class='preset' onclick='setStream(\"https://stream.radioparadise.com/flac\")'> FLAC Stream<br><small>Radio Paradise (High Quality)</small></div>"
        "<div class='preset' onclick='setStream(\"https://stream.zeno.fm/vq6p5vxb4v8uv\")'> MP3 Radio<br><small>Zeno.fm Stream</small></div>"
        "<div class='preset' onclick='setStream(\"https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac\")'> AAC File<br><small>Espressif Sample</small></div>"
        "<div class='preset' onclick='setStream(\"https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.wav\")'> WAV File<br><small>Espressif Sample</small></div>"
        "</div>"
        "<div style='text-align:center;margin-top:30px;color:#666;'>"
        "<p> ESP32-S3 with 8MB PSRAM |  Auto-Decoder Selection |  WiFi Streaming</p>"
        "</div>"
        "</div>"
        "<script>"
        "function setStream(url) {"
        "  document.getElementById('url').value = url;"
        "  document.querySelector('form').submit();"
        "} "
        "function updateVolume(value) {"
        "  console.log('Frontend: Volume slider changed to ' + value + '%%');"
        "  document.getElementById('volumeDisplay').textContent = value + '%%';"
        "  fetch('/set_volume?volume=' + value)"
        "    .then(response => {"
        "      console.log('Frontend: Volume request response status:', response.status);"
        "      return response.text();"
        "    })"
        "    .then(data => {"
        "      console.log('Frontend: Volume response data:', data);"
        "      console.log('Volume updated to ' + value + '%%');"
        "    })"
        "    .catch(error => {"
        "      console.error('Frontend: Error updating volume:', error);"
        "    });"
        "} "
        "function updateEqualizer(band, value) {"
        "  console.log('Frontend: Equalizer band ' + band + ' changed to ' + value + 'dB');"
        "  fetch('/set_equalizer?band=' + band + '&gain=' + value)"
        "    .then(response => {"
        "      console.log('Frontend: Equalizer request response status:', response.status);"
        "      return response.text();"
        "    })"
        "    .then(data => {"
        "      console.log('Frontend: Equalizer response data:', data);"
        "      console.log('Equalizer band ' + band + ' set to ' + value + 'dB');"
        "    })"
        "    .catch(error => {"
        "      console.error('Frontend: Error updating equalizer:', error);"
        "    });"
        "} "
        "function resetEqualizer() {"
        "  for(let i = 0; i < 10; i++) {"
        "    fetch('/set_equalizer?band=' + i + '&gain=0');"
        "    document.querySelectorAll('.eq-band input')[i].value = 0;"
        "  }"
        "  console.log('Equalizer reset to flat response');"
        "} "
        "function toggleLedMode(manual) {"
        "  led_manual_mode = manual;"
        "  document.getElementById('ledControls').style.display = manual ? 'block' : 'none';"
        "  fetch('/set_led_mode?manual=' + (manual ? '1' : '0'))"
        "    .then(response => response.text())"
        "    .then(data => console.log('LED mode:', manual ? 'Manual' : 'Auto'))"
        "    .catch(error => console.error('Error setting LED mode:', error));"
        "} "
        "function updateLedColor() {"
        "  var red = document.getElementById('ledRed').value;"
        "  var green = document.getElementById('ledGreen').value;"
        "  var blue = document.getElementById('ledBlue').value;"
        "  fetch('/set_led_color?red=' + red + '&green=' + green + '&blue=' + blue)"
        "    .then(response => response.text())"
        "    .then(data => console.log('LED color set to R:' + red + ' G:' + green + ' B:' + blue))"
        "    .catch(error => console.error('Error setting LED color:', error));"
        "} "
        "function setLedPreset(red, green, blue) {"
        "  document.getElementById('ledRed').value = red;"
        "  document.getElementById('ledGreen').value = green;"
        "  document.getElementById('ledBlue').value = blue;"
        "  updateLedColor();"
        "} "
        "// Test function to verify JavaScript is working"
        "window.onload = function() {"
        "  console.log('Frontend: Page loaded, JavaScript is working');"
        "  console.log('Frontend: Current volume slider value:', document.getElementById('volume').value);"
        "}; "
        "</script>"
        "</body></html>";
    
    char response[4096*8];
    snprintf(response, sizeof(response), html_page, 
             strlen(current_stream_url) > 0 ? current_stream_url : "None",
             current_volume, current_volume);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t set_stream_handler(httpd_req_t *req)
{
    char url_param[512];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "url", url_param, sizeof(url_param)) == ESP_OK) {
                // URL decode
                char *decoded_url = malloc(strlen(url_param) + 1);
                if (decoded_url) {
                    // Simple URL decode for %20 -> space, etc.
                    char *src = url_param;
                    char *dst = decoded_url;
                    while (*src) {
                        if (*src == '%' && src[1] && src[2]) {
                            char hex[3] = {src[1], src[2], 0};
                            *dst++ = (char)strtol(hex, NULL, 16);
                            src += 3;
                        } else if (*src == '+') {
                            *dst++ = ' ';
                            src++;
                        } else {
                            *dst++ = *src++;
                        }
                    }
                    *dst = 0;
                    
                    // Update stream URL safely
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        strncpy(current_stream_url, decoded_url, sizeof(current_stream_url) - 1);
                        current_stream_url[sizeof(current_stream_url) - 1] = 0;
                        stream_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);
                        
                        ESP_LOGI("HTTP_SERVER", "Stream URL changed to: %s", current_stream_url);
                    }
                    free(decoded_url);
                }
            }
        }
        free(buf);
    }
    
    // Redirect back to main page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t set_volume_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP_SERVER", "🔊 Volume change request received");
    char volume_param[16];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "volume", volume_param, sizeof(volume_param)) == ESP_OK) {
                int new_volume = atoi(volume_param);
                ESP_LOGI("HTTP_SERVER", "Parsed volume: %d%%", new_volume);
                
                // Validate volume range
                if (new_volume >= 0 && new_volume <= 100) {
                    // Update volume safely
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        current_volume = new_volume;
                        volume_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);
                        
                        ESP_LOGI("HTTP_SERVER", "✅ Volume changed to: %d%% (flag set)", current_volume);
                    } else {
                        ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for volume change");
                    }
                } else {
                    ESP_LOGW("HTTP_SERVER", "⚠️ Invalid volume range: %d%%", new_volume);
                }
            } else {
                ESP_LOGW("HTTP_SERVER", "⚠️ Failed to parse volume parameter");
            }
        } else {
            ESP_LOGW("HTTP_SERVER", "⚠️ Failed to get query string");
        }
        free(buf);
    } else {
        ESP_LOGW("HTTP_SERVER", "⚠️ No query parameters received");
    }
    
    // Return simple OK response
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t set_equalizer_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP_SERVER", "🎛️ Equalizer change request received");
    char band_param[16], gain_param[16];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "band", band_param, sizeof(band_param)) == ESP_OK &&
                httpd_query_key_value(buf, "gain", gain_param, sizeof(gain_param)) == ESP_OK) {
                
                int band = atoi(band_param);
                int gain = atoi(gain_param);
                ESP_LOGI("HTTP_SERVER", "Parsed EQ: band=%d, gain=%ddB", band, gain);
                
                // Validate band and gain ranges
                if (band >= 0 && band < 10 && gain >= -13 && gain <= 13) {
                    // Update equalizer settings safely
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        equalizer_gains[band] = gain;
                        equalizer_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);
                        
                        ESP_LOGI("HTTP_SERVER", "✅ Equalizer band %d set to: %ddB (flag set)", band, gain);
                    } else {
                        ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for equalizer change");
                    }
                } else {
                    ESP_LOGW("HTTP_SERVER", "⚠️ Invalid EQ parameters: band=%d, gain=%d", band, gain);
                }
            } else {
                ESP_LOGW("HTTP_SERVER", "⚠️ Failed to parse equalizer parameters");
            }
        } else {
            ESP_LOGW("HTTP_SERVER", "⚠️ Failed to get query string");
        }
        free(buf);
    } else {
        ESP_LOGW("HTTP_SERVER", "⚠️ No query parameters received");
    }
    
    // Return simple OK response
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t set_led_mode_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP_SERVER", "💡 LED mode change request received");
    char manual_param[16];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "manual", manual_param, sizeof(manual_param)) == ESP_OK) {
                bool manual = (atoi(manual_param) == 1);
                ESP_LOGI("HTTP_SERVER", "LED mode: %s", manual ? "Manual" : "Auto");
                
                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    led_manual_mode = manual;
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("HTTP_SERVER", "✅ LED mode set to: %s", manual ? "Manual" : "Auto");
                } else {
                    ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for LED mode change");
                }
            }
        }
        free(buf);
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t set_led_color_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP_SERVER", "💡 LED color change request received");
    char red_param[16], green_param[16], blue_param[16];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "red", red_param, sizeof(red_param)) == ESP_OK &&
                httpd_query_key_value(buf, "green", green_param, sizeof(green_param)) == ESP_OK &&
                httpd_query_key_value(buf, "blue", blue_param, sizeof(blue_param)) == ESP_OK) {
                
                int red = atoi(red_param);
                int green = atoi(green_param);
                int blue = atoi(blue_param);
                ESP_LOGI("HTTP_SERVER", "LED color: R:%d G:%d B:%d", red, green, blue);
                
                // Validate color ranges
                if (red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 && blue <= 255) {
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        led_color_red = red;
                        led_color_green = green;
                        led_color_blue = blue;
                        led_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);
                        ESP_LOGI("HTTP_SERVER", "✅ LED color set to R:%d G:%d B:%d", red, green, blue);
                    } else {
                        ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for LED color change");
                    }
                } else {
                    ESP_LOGW("HTTP_SERVER", "⚠️ Invalid LED color values: R:%d G:%d B:%d", red, green, blue);
                }
            }
        }
        free(buf);
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char status_json[1024];
    
    // Get current buffer status
    int http_filled = 0, decoder_filled = 0;
    if (global_http_stream && global_decoder) {
        http_filled = audio_element_get_output_ringbuf_size(global_http_stream);
        decoder_filled = audio_element_get_output_ringbuf_size(global_decoder);
    }
    
    // Get memory status
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    snprintf(status_json, sizeof(status_json),
        "{"
        "\"current_url\":\"%s\","
        "\"decoder\":\"%s\","
        "\"volume\":%d,"
        "\"http_buffer_kb\":%d,"
        "\"decoder_buffer_kb\":%d,"
        "\"free_heap\":%zu,"
        "\"free_psram\":%zu,"
        "\"pipeline_running\":%s"
        "}",
        current_stream_url,
        current_decoder_name,
        current_volume,
        http_filled / 1024,
        decoder_filled / 1024,
        free_heap,
        free_psram,
        global_pipeline ? "true" : "false"
    );
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, status_json, strlen(status_json));
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192 * 2;
    
    ESP_LOGI("HTTP_SERVER", "Starting HTTP server on port 80");
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Root page
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
        
        // Set stream URL
        httpd_uri_t set_stream_uri = {
            .uri = "/set_stream",
            .method = HTTP_GET,
            .handler = set_stream_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &set_stream_uri);
        
        // Set volume
        httpd_uri_t set_volume_uri = {
            .uri = "/set_volume",
            .method = HTTP_GET,
            .handler = set_volume_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &set_volume_uri);
        
        // Set equalizer
        httpd_uri_t set_equalizer_uri = {
            .uri = "/set_equalizer",
            .method = HTTP_GET,
            .handler = set_equalizer_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &set_equalizer_uri);
        
        // Set LED mode
        httpd_uri_t set_led_mode_uri = {
            .uri = "/set_led_mode",
            .method = HTTP_GET,
            .handler = set_led_mode_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &set_led_mode_uri);
        
        // Set LED color
        httpd_uri_t set_led_color_uri = {
            .uri = "/set_led_color",
            .method = HTTP_GET,
            .handler = set_led_color_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &set_led_color_uri);
        
        // Status API
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);
        
        ESP_LOGI("HTTP_SERVER", "✅ HTTP server started successfully");
        return server;
    }
    
    ESP_LOGE("HTTP_SERVER", "❌ Failed to start HTTP server");
    return NULL;
}

// Start audio pipeline with given URL
static void start_audio_pipeline(const char* url)
{
    ESP_LOGI("AUDIO_PLAYER", "🎵 Starting audio pipeline with URL: %s", url);
    
    // Clear any pending events first
    if (evt) {
        audio_event_iface_msg_t msg;
        while (audio_event_iface_listen(evt, &msg, 0) == ESP_OK) {
            // Drain the queue
        }
    }
    
    // Create event interface
    if (!evt) {
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        evt_cfg.queue_set_size = 3;  // Smaller queue to prevent overflow
        evt = audio_event_iface_init(&evt_cfg);
        if (!evt) {
            ESP_LOGE("AUDIO_PLAYER", "Failed to create event interface");
            return;
        }
    }
    
    // Create pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    global_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!global_pipeline) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create audio pipeline");
        return;
    }
    
    // Create HTTP stream
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.out_rb_size = 2 * 1024 * 1024;  // 2MB buffer
    http_cfg.task_stack = 4096;  // Increase stack size for HTTP task
    http_cfg.task_prio = 5;      // Set priority
    http_cfg.task_core = 1;      // Pin to core 1
    global_http_stream = http_stream_init(&http_cfg);
    if (!global_http_stream) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create HTTP stream");
        goto cleanup_pipeline;
    }
    
    // Detect audio format from URL and create appropriate decoder
    current_audio_format = detect_audio_format_from_url(url);
    strncpy(current_decoder_name, get_decoder_name(current_audio_format), sizeof(current_decoder_name) - 1);
    current_decoder_name[sizeof(current_decoder_name) - 1] = 0;
    
    ESP_LOGI("AUDIO_PLAYER", "🔍 Detected audio format: %s from URL: %s", current_decoder_name, url);
    
    global_decoder = create_decoder_for_format(current_audio_format);
    if (!global_decoder) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create %s decoder", current_decoder_name);
        goto cleanup_http;
    }
    
    ESP_LOGI("AUDIO_PLAYER", "✅ Created %s decoder successfully", current_decoder_name);
    
    // Create equalizer for volume control (following ESP-ADF example)
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    // Create gain array - 10 bands for equalizer (ESP-ADF supports 10 bands total)
    static int eq_gain[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // Start with flat response
    eq_cfg.set_gain = eq_gain;
    eq_cfg.task_stack = 4096;  // Increase stack size
    eq_cfg.task_prio = 5;      // Set priority
    eq_cfg.task_core = 1;      // Pin to core 1
    global_equalizer = equalizer_init(&eq_cfg);
    if (!global_equalizer) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create equalizer");
        goto cleanup_decoder;
    }
    ESP_LOGI("AUDIO_PLAYER", "🎛️  Equalizer created for volume control");
    
    // Create I2S stream
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.buffer_len = 1024 * 18;
    i2s_cfg.use_alc = true;  // Enable ALC for volume control
    
    // Configure ESP32-S3 as I2S Master but WITHOUT MCLK output
    // ESP32 generates BCLK and LRCLK only, UDA1334A uses its internal PLL
    
    // Clock source options for ESP32-S3 I2S generation (does NOT affect UDA1334A):
    // I2S_CLK_SRC_DEFAULT - Use default PLL (good balance of accuracy and power)
    // I2S_CLK_SRC_PLL_160M - Use 160MHz PLL (higher accuracy, more power)
    // I2S_CLK_SRC_XTAL - Use crystal oscillator (lower power, less accurate)
    i2s_cfg.std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;  // ESP32's internal clock source
    
    // Disable MCLK output - UDA1334A will use its internal PLL instead
    i2s_cfg.std_cfg.gpio_cfg.mclk = GPIO_NUM_NC;  // No MCLK pin connection
    
    global_i2s_stream = i2s_stream_init(&i2s_cfg);
    if (!global_i2s_stream) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create I2S stream");
        goto cleanup_equalizer;
    }
    ESP_LOGI("AUDIO_PLAYER", "🔊 I2S Master mode: ESP32-S3 provides BCLK & LRCLK only (MCLK disabled - PCM1334A uses internal PLL)");
    
    // Register elements
    if (audio_pipeline_register(global_pipeline, global_http_stream, "http") != ESP_OK) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register HTTP stream");
        goto cleanup_i2s;
    }
    
    if (audio_pipeline_register(global_pipeline, global_decoder, current_decoder_name) != ESP_OK) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register %s decoder", current_decoder_name);
        goto cleanup_i2s;
    }
    
    if (audio_pipeline_register(global_pipeline, global_equalizer, "equalizer") != ESP_OK) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register equalizer");
        goto cleanup_i2s;
    }
    
    if (audio_pipeline_register(global_pipeline, global_i2s_stream, "i2s") != ESP_OK) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register I2S stream");
        goto cleanup_i2s;
    }
    
    // Link elements: http -> decoder -> equalizer -> i2s
    const char *link_tag[4] = {"http", current_decoder_name, "equalizer", "i2s"};
    if (audio_pipeline_link(global_pipeline, &link_tag[0], 4) != ESP_OK) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to link pipeline elements");
        goto cleanup_i2s;
    }
    
    // Set URI and listener
    audio_element_set_uri(global_http_stream, url);
    audio_pipeline_set_listener(global_pipeline, evt);
    
    // Start pipeline
    if (audio_pipeline_run(global_pipeline) != ESP_OK) {
        ESP_LOGE("AUDIO_PLAYER", "Failed to start pipeline");
        goto cleanup_i2s;
    }
    
    // Update LED for new stream
    update_led_for_stream(url, true);
    
    ESP_LOGI("AUDIO_PLAYER", "✅ Audio pipeline started successfully");
    return;

cleanup_i2s:
    if (global_i2s_stream) {
        audio_element_deinit(global_i2s_stream);
        global_i2s_stream = NULL;
    }
cleanup_equalizer:
    if (global_equalizer) {
        audio_element_deinit(global_equalizer);
        global_equalizer = NULL;
    }
cleanup_decoder:
    if (global_decoder) {
        audio_element_deinit(global_decoder);
        global_decoder = NULL;
    }
cleanup_http:
    if (global_http_stream) {
        audio_element_deinit(global_http_stream);
        global_http_stream = NULL;
    }
cleanup_pipeline:
    if (global_pipeline) {
        audio_pipeline_deinit(global_pipeline);
        global_pipeline = NULL;
    }
    ESP_LOGE("AUDIO_PLAYER", "❌ Failed to start audio pipeline");
}

// Update volume for the current pipeline
static void update_pipeline_volume(int volume_percent)
{
    if (global_i2s_stream) {
        ESP_LOGI("AUDIO_PLAYER", "🔊 Setting volume to %d%% using ALC (current_volume global = %d%%)", volume_percent, current_volume);
        
        // Convert percentage (0-100) to ALC volume level with better curve
        // ALC volume range is typically -96dB to 0dB
        // Use a more user-friendly logarithmic-like curve for better control
        int alc_volume;
        if (volume_percent == 0) {
            alc_volume = -96;  // Minimum volume (mute)
        } else if (volume_percent <= 10) {
            // Map 1-10% to -60dB to -40dB (very quiet but audible)
            alc_volume = -60 + ((volume_percent - 1) * 20) / 9;
        } else if (volume_percent <= 50) {
            // Map 11-50% to -40dB to -20dB (low to medium)
            alc_volume = -40 + ((volume_percent - 10) * 20) / 40;
        } else if (volume_percent <= 80) {
            // Map 51-80% to -20dB to -10dB (medium to high)
            alc_volume = -20 + ((volume_percent - 50) * 10) / 30;
        } else {
            // Map 81-100% to -10dB to 0dB (high to maximum)
            alc_volume = -10 + ((volume_percent - 80) * 10) / 20;
        }
        
        // Ensure bounds
        if (alc_volume > 0) alc_volume = 0;
        if (alc_volume < -96) alc_volume = -96;
        
        // Set ALC volume on I2S stream
        esp_err_t ret = i2s_alc_volume_set(global_i2s_stream, alc_volume);
        if (ret != ESP_OK) {
            ESP_LOGE("AUDIO_PLAYER", "Failed to set ALC volume: %s", esp_err_to_name(ret));
            return;
        }
        
        ESP_LOGI("AUDIO_PLAYER", "✅ Volume updated to %d%% (ALC: %ddB)", volume_percent, alc_volume);
    } else {
        ESP_LOGW("AUDIO_PLAYER", "⚠️  No I2S stream available for volume control");
    }
}

// Update equalizer settings for the current pipeline
static void update_pipeline_equalizer(void)
{
    if (global_equalizer) {
        ESP_LOGI("AUDIO_PLAYER", "🎛️  Updating equalizer settings");
        
        // ESP-ADF equalizer has 10 bands total (0-9), not per channel
        // Apply current equalizer gains directly to the 10 available bands
        for (int band = 0; band < 10; band++) {
            esp_err_t ret = equalizer_set_gain_info(global_equalizer, band, equalizer_gains[band], true);
            if (ret != ESP_OK) {
                ESP_LOGE("AUDIO_PLAYER", "Failed to set equalizer gain for band %d: %s", band, esp_err_to_name(ret));
                continue;
            }
        }
        
        ESP_LOGI("AUDIO_PLAYER", "✅ Equalizer settings updated");
    } else {
        ESP_LOGW("AUDIO_PLAYER", "⚠️  No equalizer available");
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("wifi_idf", ESP_LOG_INFO);
    esp_log_level_set("HTTP_SERVER", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_PLAYER", ESP_LOG_INFO);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize stream control mutex
    stream_control_mutex = xSemaphoreCreateMutex();
    if (stream_control_mutex == NULL) {
        ESP_LOGE("main", "Failed to create stream control mutex");
        return;
    }

    // Initialize WiFi
    init_wifi();
    
    // Initialize RGB LED
    init_rgb_led();
    
    // Wait for WiFi connection
    ESP_LOGI("main", "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("main", "✅ WiFi connected successfully");
        
        // Start HTTP server
        start_webserver();
        
        // Set default stream URL (first sample stream - FLAC)
        strncpy(current_stream_url, sample_streams[0], sizeof(current_stream_url) - 1);
        current_stream_url[sizeof(current_stream_url) - 1] = 0;
        
        ESP_LOGI("main", "🎵 ESP32-S3 Audio Streamer ready!");
        ESP_LOGI("main", "🌐 Web interface: http://your-esp32-ip/");
        ESP_LOGI("main", "📡 Default stream: %s", current_stream_url);
        
        // Main audio streaming loop
        while (1) {
            // Check for stream change requests - but not if we're restarting
            bool change_requested = false;
            bool volume_change = false;
            bool equalizer_change = false;
            bool led_change = false;
            char new_url[512] = {0};
            int new_volume = 0;
            int new_led_red = 0, new_led_green = 0, new_led_blue = 0;
            
            if (!pipeline_restarting && xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (stream_change_requested) {
                    change_requested = true;
                    strncpy(new_url, current_stream_url, sizeof(new_url) - 1);
                    stream_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "📡 Stream change detected");
                }
                if (volume_change_requested) {
                    volume_change = true;
                    new_volume = current_volume;
                    volume_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "🔊 Volume change detected: %d%%", new_volume);
                }
                if (equalizer_change_requested) {
                    equalizer_change = true;
                    equalizer_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "🎛️ Equalizer change detected");
                }
                if (led_change_requested) {
                    led_change = true;
                    new_led_red = led_color_red;
                    new_led_green = led_color_green;
                    new_led_blue = led_color_blue;
                    led_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "💡 LED color change detected: R:%d G:%d B:%d", new_led_red, new_led_green, new_led_blue);
                }
                xSemaphoreGive(stream_control_mutex);
            }
            
            // Handle volume changes
            if (volume_change) {
                ESP_LOGI("AUDIO_PLAYER", "🔊 Processing volume change to %d%%", new_volume);
                update_pipeline_volume(new_volume);
            }
            
            // Handle equalizer changes
            if (equalizer_change) {
                ESP_LOGI("AUDIO_PLAYER", "🎛️ Processing equalizer changes");
                update_pipeline_equalizer();
            }
            
            // Handle LED changes
            if (led_change) {
                ESP_LOGI("AUDIO_PLAYER", "💡 Processing LED color change to R:%d G:%d B:%d", new_led_red, new_led_green, new_led_blue);
                set_led_color_manual(new_led_red, new_led_green, new_led_blue);
            }
            
            if (change_requested) {
                ESP_LOGI("AUDIO_PLAYER", "🔄 Changing stream to: %s", new_url);
                pipeline_restarting = true;
                
                // Stop current pipeline if running - proper cleanup sequence
                if (global_pipeline) {
                    ESP_LOGI("AUDIO_PLAYER", "Stopping current pipeline...");
                    
                    // Stop pipeline first
                    audio_pipeline_stop(global_pipeline);
                    audio_pipeline_wait_for_stop(global_pipeline);
                    
                    // Remove listener before cleanup
                    audio_pipeline_remove_listener(global_pipeline);
                    
                    // Terminate pipeline
                    audio_pipeline_terminate(global_pipeline);
                    
                    // Unregister elements from pipeline
                    if (global_http_stream) {
                        audio_pipeline_unregister(global_pipeline, global_http_stream);
                    }
                    if (global_decoder) {
                        audio_pipeline_unregister(global_pipeline, global_decoder);
                    }
                    if (global_equalizer) {
                        audio_pipeline_unregister(global_pipeline, global_equalizer);
                    }
                    if (global_i2s_stream) {
                        audio_pipeline_unregister(global_pipeline, global_i2s_stream);
                    }
                    
                    // Deinitialize elements individually
                    if (global_http_stream) {
                        audio_element_deinit(global_http_stream);
                        global_http_stream = NULL;
                    }
                    if (global_decoder) {
                        audio_element_deinit(global_decoder);
                        global_decoder = NULL;
                    }
                    if (global_equalizer) {
                        audio_element_deinit(global_equalizer);
                        global_equalizer = NULL;
                    }
                    if (global_i2s_stream) {
                        audio_element_deinit(global_i2s_stream);
                        global_i2s_stream = NULL;
                    }
                    
                    // Finally deinitialize pipeline
                    audio_pipeline_deinit(global_pipeline);
                    global_pipeline = NULL;
                    
                    // Turn off LED when pipeline stops
                    update_led_for_stream("", false);
                    
                    ESP_LOGI("AUDIO_PLAYER", "Pipeline cleanup completed");
                    
                    // Wait a bit before starting new pipeline
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                
                // Start new stream
                start_audio_pipeline(new_url);
                pipeline_restarting = false;
            } else if (!global_pipeline && !pipeline_restarting) {
                // Start stream if we have a URL and no pipeline is running
                if (strlen(current_stream_url) > 0) {
                    start_audio_pipeline(current_stream_url);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            
            // Monitor pipeline status
            if (global_pipeline) {
                audio_event_iface_msg_t msg;
                esp_err_t ret = audio_event_iface_listen(evt, &msg, pdMS_TO_TICKS(1000));
                
                if (ret == ESP_OK) {
                    // Handle music info events for automatic I2S configuration
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT 
                        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                        
                        audio_element_info_t music_info = {0};
                        const char* source_name = "Unknown";
                        
                        if (msg.source == (void *) global_http_stream) {
                            audio_element_getinfo(global_http_stream, &music_info);
                            source_name = "HTTP Stream";
                        } else if (msg.source == (void *) global_decoder) {
                            audio_element_getinfo(global_decoder, &music_info);
                            source_name = "Decoder";
                        }
                        
                        // Only configure I2S if we have valid audio parameters
                        if (global_i2s_stream && music_info.sample_rates > 0 && 
                            music_info.channels > 0 && music_info.bits > 0) {
                            ESP_LOGI("AUDIO_PLAYER", "🎵 %s info - Sample rate: %dHz, Channels: %d, Bits: %d", 
                                    source_name, music_info.sample_rates, music_info.channels, music_info.bits);
                            ESP_LOGI("AUDIO_PLAYER", "🔧 Configuring I2S: %dHz, %d-bit, %d-channel", 
                                    music_info.sample_rates, music_info.bits, music_info.channels);
                            
                            // Configure I2S with detected parameters
                            esp_err_t i2s_ret = i2s_stream_set_clk(global_i2s_stream, music_info.sample_rates, 
                                                                  music_info.bits, music_info.channels);
                            if (i2s_ret != ESP_OK) {
                                ESP_LOGE("AUDIO_PLAYER", "Failed to configure I2S: %s", esp_err_to_name(i2s_ret));
                            }
                            
                            // Configure equalizer with audio parameters
                            if (global_equalizer) {
                                esp_err_t eq_ret = equalizer_set_info(global_equalizer, music_info.sample_rates, music_info.channels);
                                if (eq_ret != ESP_OK) {
                                    ESP_LOGE("AUDIO_PLAYER", "Failed to configure equalizer: %s", esp_err_to_name(eq_ret));
                                } else {
                                    ESP_LOGI("AUDIO_PLAYER", "🎛️  Equalizer configured for %dHz, %d channels", 
                                            music_info.sample_rates, music_info.channels);
                                    // Apply current equalizer settings
                                    update_pipeline_equalizer();
                                }
                            }
                            
                            // Set initial ALC volume (default volume, not 0%)
                            if (global_i2s_stream) {
                                update_pipeline_volume(current_volume);
                            }
                        }
                    }
                    
                    // Handle file completion messages
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT 
                        && msg.cmd == AEL_MSG_CMD_FINISH) {
                        ESP_LOGI("AUDIO_PLAYER", "🎵 File playback completed successfully");
                        // File finished normally - clean up pipeline and wait for new stream
                        pipeline_restarting = true;
                        
                        // Stop and cleanup current pipeline
                        if (global_pipeline) {
                            audio_pipeline_stop(global_pipeline);
                            audio_pipeline_wait_for_stop(global_pipeline);
                            audio_pipeline_remove_listener(global_pipeline);
                            audio_pipeline_terminate(global_pipeline);
                            
                            // Cleanup elements
                            if (global_http_stream) {
                                audio_pipeline_unregister(global_pipeline, global_http_stream);
                                audio_element_deinit(global_http_stream);
                                global_http_stream = NULL;
                            }
                            if (global_decoder) {
                                audio_pipeline_unregister(global_pipeline, global_decoder);
                                audio_element_deinit(global_decoder);
                                global_decoder = NULL;
                            }
                            if (global_equalizer) {
                                audio_pipeline_unregister(global_pipeline, global_equalizer);
                                audio_element_deinit(global_equalizer);
                                global_equalizer = NULL;
                            }
                            if (global_i2s_stream) {
                                audio_pipeline_unregister(global_pipeline, global_i2s_stream);
                                audio_element_deinit(global_i2s_stream);
                                global_i2s_stream = NULL;
                            }
                            
                            audio_pipeline_deinit(global_pipeline);
                            global_pipeline = NULL;
                        }
                        
                        // Turn off LED when file completes
                        update_led_for_stream("", false);
                        
                        pipeline_restarting = false;
                        ESP_LOGI("AUDIO_PLAYER", "✅ Ready for new stream - use web interface to start playback");
                        continue;  // Continue loop to check for new stream requests
                    }
                    
                    // Handle pipeline status changes - distinguish between completion and errors
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
                        
                        audio_element_status_t status = (audio_element_status_t)msg.data;
                        if (status == AEL_STATUS_STATE_STOPPED || status == AEL_STATUS_STATE_FINISHED) {
                            ESP_LOGW("AUDIO_PLAYER", "⚠️  Element %p stopped/finished (status: %d)", msg.source, status);
                            
                            // Check if this is the HTTP stream finishing (file completed successfully)
                            if (msg.source == (void *) global_http_stream) {
                                // Check if it's a successful completion (no error) or actual network issue
                                if (status == AEL_STATUS_STATE_FINISHED) {
                                    ESP_LOGI("AUDIO_PLAYER", "🎵 HTTP stream completed successfully");
                                    // Let the AEL_MSG_CMD_FINISH handler take care of cleanup
                                    continue;
                                } else {
                                    ESP_LOGW("AUDIO_PLAYER", "HTTP stream stopped - this might be network issue");
                                }
                            } else if (msg.source == (void *) global_decoder) {
                                if (status == AEL_STATUS_STATE_FINISHED) {
                                    ESP_LOGI("AUDIO_PLAYER", "🎵 Decoder finished successfully");
                                    // Let the AEL_MSG_CMD_FINISH handler take care of cleanup
                                    continue;
                                } else {
                                    ESP_LOGW("AUDIO_PLAYER", "Decoder stopped - this might be format issue");
                                }
                            }
                            
                            // Only restart if it's an actual error (not normal completion)
                            // and we're not already in a restart cycle
                            static TickType_t last_restart = 0;
                            TickType_t now = xTaskGetTickCount();
                            if (!pipeline_restarting && (now - last_restart) > pdMS_TO_TICKS(5000)
                                && status != AEL_STATUS_STATE_FINISHED) {  // Don't restart on normal completion
                                ESP_LOGI("AUDIO_PLAYER", "Attempting pipeline restart after cooldown...");
                                last_restart = now;
                                pipeline_restarting = true;
                                
                                // Simple restart approach
                                audio_pipeline_stop(global_pipeline);
                                audio_pipeline_wait_for_stop(global_pipeline);
                                audio_pipeline_reset_ringbuffer(global_pipeline);
                                audio_pipeline_reset_elements(global_pipeline);
                                
                                vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause
                                audio_pipeline_run(global_pipeline);
                                pipeline_restarting = false;
                            } else {
                                if (status == AEL_STATUS_STATE_FINISHED) {
                                    ESP_LOGI("AUDIO_PLAYER", "File completed - no restart needed");
                                } else {
                                    ESP_LOGW("AUDIO_PLAYER", "Restart suppressed - %s", 
                                            pipeline_restarting ? "already restarting" : "too soon after last restart");
                                }
                            }
                        }
                    }
                }
            } else {
                // No pipeline running, wait a bit before checking again
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    } else {
        ESP_LOGE("main", "❌ WiFi connection failed");
    }
}
