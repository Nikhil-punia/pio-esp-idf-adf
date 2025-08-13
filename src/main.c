/* Play an MP3, AAC or WAV file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <ctype.h>
#include <inttypes.h>
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
#include "../http_stream_streaming_patch.h"
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
typedef enum
{
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
#define RGB_LED_GPIO 38 // GPIO38 for ESP32-S3-N8R8 DevKit RGB LED (WS2812)

// WiFi event group and status
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// HTTP Server and stream control
static httpd_handle_t server = NULL;
static char current_stream_url[512] = "";
static bool stream_change_requested = false;
static bool pipeline_restarting = false;
static SemaphoreHandle_t stream_control_mutex = NULL;
static int current_volume = 70; // Default volume (0-100)
static bool volume_change_requested = false;

// WebSocket connections management
static int websocket_fd = -1;
static bool websocket_connected = false;

// Equalizer settings (10-band EQ, -13 to +13 dB)
static int equalizer_gains[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // Default flat response
static bool equalizer_change_requested = false;

// LED control settings
static int led_color_red = 128;
static int led_color_green = 0;
static int led_color_blue = 128;
static bool led_manual_mode = false;
static bool led_change_requested = false;

// Sample stream URLs for different formats
static const char *sample_streams[] = {
    "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.flac", // FLAC file (tested by ESP-ADF)
    "https://stream.zeno.fm/vq6p5vxb4v8uv",                     // MP3 radio stream
    "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac",  // AAC file (standard)
    "https://stream.radioparadise.com/aac-320",                 // Radio Paradise AAC 320kbps (reliable)
    "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.wav",  // WAV file
    "https://ice1.somafm.com/groovesalad-256-aac",              // 256kbps AAC stream
};

// Application tag for logging
static const char *TAG = "HTTP_AUTO_DECODER_EXAMPLE";

// Buffer monitoring configuration
#define BUFFER_LOG_INTERVAL_MS 10000 // Log buffer status every 10 seconds (reduced frequency)
static TimerHandle_t buffer_monitor_timer = NULL;

// Forward declarations for buffer monitoring functions
static void start_buffer_monitoring(void);

// Audio format detection based on URL extension and content-type
static audio_format_t detect_audio_format_from_url(const char *url)
{
    if (!url)
        return AUDIO_FORMAT_UNKNOWN;

    // Convert to lowercase for comparison
    char *url_lower = strdup(url);
    if (!url_lower)
        return AUDIO_FORMAT_UNKNOWN;

    for (int i = 0; url_lower[i]; i++)
    {
        url_lower[i] = tolower(url_lower[i]);
    }

    audio_format_t format = AUDIO_FORMAT_UNKNOWN;

    // Check file extension in URL
    if (strstr(url_lower, ".mp3") || strstr(url_lower, "mp3"))
    {
        format = AUDIO_FORMAT_MP3;
    }
    else if (strstr(url_lower, ".flac") || strstr(url_lower, "flac"))
    {
        format = AUDIO_FORMAT_FLAC;
    }
    else if (strstr(url_lower, ".aac") || strstr(url_lower, "aac"))
    {
        format = AUDIO_FORMAT_AAC;
    }
    else if (strstr(url_lower, ".wav") || strstr(url_lower, "wav"))
    {
        format = AUDIO_FORMAT_WAV;
    }
    else if (strstr(url_lower, ".ogg") || strstr(url_lower, "ogg"))
    {
        format = AUDIO_FORMAT_OGG;
    }
    else if (strstr(url_lower, ".amr") || strstr(url_lower, "amr"))
    {
        format = AUDIO_FORMAT_AMR;
    }
    else if (strstr(url_lower, ".m3u8") || strstr(url_lower, "m3u8") || 
             strstr(url_lower, "chunklist") || strstr(url_lower, "playlist.m3u8"))
    {
        // M3U8 live streaming format detection
        // Check for common M3U8 audio formats in URL or guess based on service
        if (strstr(url_lower, "aac") || strstr(url_lower, "mp4") || 
            strstr(url_lower, "adts") || strstr(url_lower, "m4a"))
        {
            format = AUDIO_FORMAT_AAC; // M3U8 with AAC segments (most common)
            ESP_LOGI("FORMAT_DETECT", "🎵 M3U8 Live Stream detected - using AAC decoder");
        }
        else if (strstr(url_lower, "mp3") || strstr(url_lower, "mpeg"))
        {
            format = AUDIO_FORMAT_MP3; // M3U8 with MP3 segments
            ESP_LOGI("FORMAT_DETECT", "🎵 M3U8 Live Stream detected - using MP3 decoder");
        }
        else
        {
            // Default to AAC for M3U8 streams (HLS standard)
            format = AUDIO_FORMAT_AAC;
            ESP_LOGI("FORMAT_DETECT", "🎵 M3U8 Live Stream detected - defaulting to AAC decoder");
        }
    }
    else
    {
        // Try to guess from known streaming services
        if (strstr(url_lower, "radioparadise.com"))
        {
            // Radio Paradise has both AAC and FLAC streams
            if (strstr(url_lower, "flac"))
            {
                format = AUDIO_FORMAT_FLAC; // Radio Paradise FLAC stream
            }
            else if (strstr(url_lower, "aac") || strstr(url_lower, "320"))
            {
                format = AUDIO_FORMAT_AAC; // Radio Paradise AAC streams
            }
            else
            {
                format = AUDIO_FORMAT_AAC; // Default to AAC for Radio Paradise
            }
        }
        else if (strstr(url_lower, "zeno.fm") || strstr(url_lower, "icecast") ||
                 strstr(url_lower, "shoutcast") || strstr(url_lower, "radio"))
        {
            format = AUDIO_FORMAT_MP3; // Most internet radio is MP3
        }
        else
        {
            format = AUDIO_FORMAT_MP3; // Default fallback to MP3
        }
    }

    free(url_lower);
    return format;
}

int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    // Enhanced M3U8 live streaming event handler
    if (msg->event_id == HTTP_STREAM_RESOLVE_ALL_TRACKS)
    {
        ESP_LOGI("M3U8_HANDLER", "📋 Resolved all tracks in playlist");
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_TRACK)
    {
        ESP_LOGI("M3U8_HANDLER", "🎵 Track finished - fetching next segment");
        // For M3U8 live streams: automatically fetch the next track/segment
        esp_err_t ret = http_stream_next_track(msg->el);
        if (ret != ESP_OK) {
            ESP_LOGW("M3U8_HANDLER", "⚠️ Failed to fetch next track: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    
    if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST)
    {
        ESP_LOGI("M3U8_HANDLER", "📋 Playlist finished - refetching for live stream continuation");
        // For M3U8 live streams: refetch the playlist to get new segments
        esp_err_t ret = http_stream_fetch_again(msg->el);
        if (ret != ESP_OK) {
            ESP_LOGW("M3U8_HANDLER", "⚠️ Failed to refetch playlist: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    
    // Handle M3U8-specific errors
    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        ESP_LOGI("M3U8_HANDLER", "🌐 HTTP request initiated");
    }
    
    if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
        ESP_LOGI("M3U8_HANDLER", "📡 HTTP response received");
    }
    
    return ESP_OK;
}

// Get decoder name string from format
static const char *get_decoder_name(audio_format_t format)
{
    switch (format)
    {
    case AUDIO_FORMAT_MP3:
        return "mp3";
    case AUDIO_FORMAT_AAC:
        return "aac";
    case AUDIO_FORMAT_FLAC:
        return "flac";
    case AUDIO_FORMAT_WAV:
        return "wav";
    case AUDIO_FORMAT_OGG:
        return "ogg";
    case AUDIO_FORMAT_AMR:
        return "amr";
    default:
        return "unknown";
    }
}

// Create decoder element based on detected format
static audio_element_handle_t create_decoder_for_format(audio_format_t format)
{
    audio_element_handle_t decoder = NULL;

    switch (format)
    {
    case AUDIO_FORMAT_MP3:
    {
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        mp3_cfg.out_rb_size = 512 * 1024;
        mp3_cfg.task_stack = 4096 * 3; // Increase stack size
        mp3_cfg.task_prio = 5;         // Set priority
        mp3_cfg.task_core = 1;         // Pin to core 1
        decoder = mp3_decoder_init(&mp3_cfg);
        break;
    }
    case AUDIO_FORMAT_AAC:
    {
        aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
        aac_cfg.out_rb_size = 1024 * 1024; // 1MB buffer for high-bitrate AAC
        aac_cfg.task_stack = 4096 * 6;     // Large stack for complex AAC decoding
        aac_cfg.task_prio = 6;             // Higher priority for AAC
        aac_cfg.task_core = 1;             // Pin to core 1
        decoder = aac_decoder_init(&aac_cfg);
        break;
    }
    case AUDIO_FORMAT_FLAC:
    {
        flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
        flac_cfg.out_rb_size = 1024 * 1024; // Increase to 1MB for streaming FLAC
        flac_cfg.task_stack = 1024 * 24;    // Increase stack for streaming
        flac_cfg.task_prio = 6;             // Higher priority for FLAC streaming
        flac_cfg.task_core = 1;             // Pin to core 1
        flac_cfg.stack_in_ext = true;       // Use external memory for stack
        decoder = flac_decoder_init(&flac_cfg);
        break;
    }
    case AUDIO_FORMAT_WAV:
    {
        wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
        wav_cfg.out_rb_size = 256 * 1024;
        wav_cfg.task_stack = 4096 * 3; // Increase stack size
        wav_cfg.task_prio = 5;         // Set priority
        wav_cfg.task_core = 1;         // Pin to core 1
        decoder = wav_decoder_init(&wav_cfg);
        break;
    }
    case AUDIO_FORMAT_OGG:
    {
        ogg_decoder_cfg_t ogg_cfg = DEFAULT_OGG_DECODER_CONFIG();
        ogg_cfg.out_rb_size = 512 * 1024;
        ogg_cfg.task_stack = 4096 * 3; // Increase stack size
        ogg_cfg.task_prio = 5;         // Set priority
        ogg_cfg.task_core = 1;         // Pin to core 1
        decoder = ogg_decoder_init(&ogg_cfg);
        break;
    }
    case AUDIO_FORMAT_AMR:
    {
        amr_decoder_cfg_t amr_cfg = DEFAULT_AMR_DECODER_CONFIG();
        amr_cfg.out_rb_size = 256 * 1024;
        amr_cfg.task_stack = 4096 * 3; // Increase stack size
        amr_cfg.task_prio = 5;         // Set priority
        amr_cfg.task_core = 1;         // Pin to core 1
        decoder = amr_decoder_init(&amr_cfg);
        break;
    }
    default:
        ESP_LOGW(TAG, "Unknown audio format, falling back to MP3 decoder");
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        mp3_cfg.out_rb_size = 512 * 1024;
        mp3_cfg.task_stack = 4096 * 3; // Increase stack size
        mp3_cfg.task_prio = 5;         // Set priority
        mp3_cfg.task_core = 1;         // Pin to core 1
        decoder = mp3_decoder_init(&mp3_cfg);
        break;
    }

    return decoder;
}

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI("wifi_idf", "WiFi disconnected, trying to reconnect...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
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
                .required = false},
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
        .strip_gpio_num = RGB_LED_GPIO,                              // GPIO for LED strip data pin
        .max_leds = 1,                                               // Only 1 LED on the devkit
        .led_model = LED_MODEL_WS2812,                               // WS2812 LED model
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // WS2812 uses GRB format
        .flags.invert_out = false,                                   // Don't invert output
    };

    // RMT configuration for LED strip
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,    // Use default RMT clock source
        .resolution_hz = 10 * 1000 * 1000, // 10MHz resolution for precise timing
        .flags.with_dma = false,           // DMA not needed for single LED
    };

    // Create LED strip handle
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK)
    {
        ESP_LOGE("RGB_LED", "Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Clear LED (turn off)
    ret = led_strip_clear(led_strip);
    if (ret != ESP_OK)
    {
        ESP_LOGE("RGB_LED", "Failed to clear LED: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI("RGB_LED", "✅ WS2812 RGB LED initialized successfully with RMT");
}

// Set LED color based on stream type and status using official led_strip component
static void update_led_for_stream(const char *url, bool is_playing)
{
    if (!led_strip)
    {
        return; // LED strip not initialized
    }

    // If in manual mode, don't auto-update LED
    if (led_manual_mode)
    {
        return;
    }

    if (!is_playing)
    {
        // Off when not playing
        led_strip_clear(led_strip);
        led_strip_refresh(led_strip);
        return;
    }

    // Determine color based on stream content
    if (strstr(url, ".flac") || strstr(url, "flac"))
    {
        // Purple for FLAC (high quality)
        led_strip_set_pixel(led_strip, 0, 128, 0, 128);
    }
    else if (strstr(url, ".mp3") || strstr(url, "mp3"))
    {
        // Blue for MP3
        led_strip_set_pixel(led_strip, 0, 0, 0, 255);
    }
    else if (strstr(url, ".wav") || strstr(url, "wav"))
    {
        // Green for WAV
        led_strip_set_pixel(led_strip, 0, 0, 255, 0);
    }
    else if (strstr(url, ".aac") || strstr(url, "aac"))
    {
        // Orange for AAC
        led_strip_set_pixel(led_strip, 0, 255, 165, 0);
    }
    else if (strstr(url, "zeno.fm") || strstr(url, "radio"))
    {
        // Red for radio streams
        led_strip_set_pixel(led_strip, 0, 255, 0, 0);
    }
    else
    {
        // White for unknown formats
        led_strip_set_pixel(led_strip, 0, 255, 255, 255);
    }

    // Refresh the LED to show the new color
    led_strip_refresh(led_strip);
}

// Set LED color manually
static void set_led_color_manual(int red, int green, int blue)
{
    if (!led_strip)
    {
        return;
    }

    led_strip_set_pixel(led_strip, 0, red, green, blue);
    led_strip_refresh(led_strip);
    ESP_LOGI("RGB_LED", "💡 LED color set to R:%d G:%d B:%d", red, green, blue);
}

// WebSocket message broadcasting (lightweight version)
static void broadcast_status_update(void)
{
    if (!websocket_connected || websocket_fd < 0)
    {
        return;
    }

    // Create a smaller status JSON message to reduce stack usage
    char json_buffer[1024]; // Increased buffer size for longer URLs

    // Get basic status info
    int http_filled = 0, decoder_filled = 0;
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (global_http_stream && global_decoder)
    {
        http_filled = audio_element_get_output_ringbuf_size(global_http_stream);
        decoder_filled = audio_element_get_output_ringbuf_size(global_decoder);
    }

    // Create simple JSON status message
    // Truncate URL if too long to prevent buffer overflow
    char safe_url[256];
    strncpy(safe_url, current_stream_url, sizeof(safe_url) - 1);
    safe_url[sizeof(safe_url) - 1] = 0;

    snprintf(json_buffer, sizeof(json_buffer),
             "{"
             "\"type\":\"status_update\","
             "\"data\":{"
             "\"current_url\":\"%.200s\","
             "\"decoder\":\"%.15s\","
             "\"volume\":%d,"
             "\"pipeline_running\":%s,"
             "\"http_buffer_kb\":%d,"
             "\"decoder_buffer_kb\":%d,"
             "\"free_heap_kb\":%zu,"
             "\"free_psram_kb\":%zu"
             "}"
             "}",
             safe_url,
             current_decoder_name,
             current_volume,
             global_pipeline ? "true" : "false",
             http_filled / 1024,
             decoder_filled / 1024,
             free_heap / 1024,
             free_psram / 1024);

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)json_buffer;
    ws_pkt.len = strlen(json_buffer);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_send_frame_async(server, websocket_fd, &ws_pkt);
    if (ret != ESP_OK)
    {
        ESP_LOGW("WEBSOCKET", "Failed to send status update: %s", esp_err_to_name(ret));
        // If send fails, disconnect the WebSocket
        websocket_connected = false;
        websocket_fd = -1;
    }
}

// Handle incoming WebSocket messages (lightweight version)
static void handle_websocket_message(const char *message)
{
    ESP_LOGI("WEBSOCKET", "Received message: %.100s", message); // Limit log length

    // Simple string parsing instead of cJSON to reduce stack usage
    if (strstr(message, "\"type\":\"set_stream\"") && strstr(message, "\"url\":\""))
    {
        char *url_start = strstr(message, "\"url\":\"");
        if (url_start)
        {
            url_start += 7; // Skip "url":"
            char *url_end = strchr(url_start, '"');
            if (url_end && (url_end - url_start) < sizeof(current_stream_url) - 1)
            {
                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    strncpy(current_stream_url, url_start, url_end - url_start);
                    current_stream_url[url_end - url_start] = 0;
                    stream_change_requested = true;
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("WEBSOCKET", "Stream changed to: %s", current_stream_url);
                    // Send status update after stream change
                    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to allow processing
                    broadcast_status_update();
                }
            }
        }
    }
    else if (strstr(message, "\"type\":\"set_volume\"") && strstr(message, "\"volume\":"))
    {
        char *vol_start = strstr(message, "\"volume\":");
        if (vol_start)
        {
            vol_start += 9; // Skip "volume":
            int vol = atoi(vol_start);
            if (vol >= 0 && vol <= 100)
            {
                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    current_volume = vol;
                    volume_change_requested = true;
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("WEBSOCKET", "Volume changed to: %d%%", vol);
                    // Send status update after volume change
                    broadcast_status_update();
                }
            }
        }
    }
    else if (strstr(message, "\"type\":\"set_equalizer\""))
    {
        char *band_start = strstr(message, "\"band\":");
        char *gain_start = strstr(message, "\"gain\":");
        if (band_start && gain_start)
        {
            band_start += 7; // Skip "band":
            gain_start += 7; // Skip "gain":
            int band_idx = atoi(band_start);
            int gain_val = atoi(gain_start);
            if (band_idx >= 0 && band_idx < 10 && gain_val >= -13 && gain_val <= 13)
            {
                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    equalizer_gains[band_idx] = gain_val;
                    equalizer_change_requested = true;
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("WEBSOCKET", "Equalizer band %d set to %ddB", band_idx, gain_val);
                    // Send status update after equalizer change
                    broadcast_status_update();
                }
            }
        }
    }
    else if (strstr(message, "\"type\":\"reset_equalizer\""))
    {
        if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            for (int i = 0; i < 10; i++)
            {
                equalizer_gains[i] = 0;
            }
            equalizer_change_requested = true;
            xSemaphoreGive(stream_control_mutex);
            ESP_LOGI("WEBSOCKET", "Equalizer reset to flat response");
            // Send status update after equalizer reset
            broadcast_status_update();
        }
    }
    else if (strstr(message, "\"type\":\"set_led_mode\""))
    {
        bool manual = strstr(message, "\"manual\":true") != NULL;
        if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            led_manual_mode = manual;
            xSemaphoreGive(stream_control_mutex);
            ESP_LOGI("WEBSOCKET", "LED mode set to: %s", manual ? "Manual" : "Auto");
        }
    }
    else if (strstr(message, "\"type\":\"set_led_color\""))
    {
        char *red_start = strstr(message, "\"red\":");
        char *green_start = strstr(message, "\"green\":");
        char *blue_start = strstr(message, "\"blue\":");
        if (red_start && green_start && blue_start)
        {
            red_start += 6;   // Skip "red":
            green_start += 8; // Skip "green":
            blue_start += 7;  // Skip "blue":
            int r = atoi(red_start);
            int g = atoi(green_start);
            int b = atoi(blue_start);
            if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255)
            {
                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    led_color_red = r;
                    led_color_green = g;
                    led_color_blue = b;
                    led_change_requested = true;
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("WEBSOCKET", "LED color set to R:%d G:%d B:%d", r, g, b);
                }
            }
        }
    }
    else if (strstr(message, "\"type\":\"get_status\""))
    {
        // Send current status immediately
        broadcast_status_update();
    }
}

// WebSocket handler
static esp_err_t websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI("WEBSOCKET", "WebSocket connection handshake");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Set max_len = 0 to get the frame len
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE("WEBSOCKET", "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    if (ws_pkt.len)
    {
        // Allocate buffer for the message
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL)
        {
            ESP_LOGE("WEBSOCKET", "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;

        // Receive the frame payload
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK)
        {
            ESP_LOGE("WEBSOCKET", "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }

        // Store WebSocket connection info
        websocket_fd = httpd_req_to_sockfd(req);
        websocket_connected = true;

        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
        {
            // Handle the message
            handle_websocket_message((char *)ws_pkt.payload);
        }
        else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE)
        {
            ESP_LOGI("WEBSOCKET", "WebSocket connection closed");
            websocket_connected = false;
            websocket_fd = -1;
        }

        free(buf);
    }

    return ESP_OK;
}

// HTTP Server handlers
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html_page =
        "<!DOCTYPE html>"
        "<html><head><title>ESP32-S3 Audio Streamer</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;}"
        ".container{max-width:800px;margin:auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}"
        "h1{color:#333;text-align:center;}"
        ".current{background:#e8f5e8;padding:15px;border-radius:5px;margin:10px 0;}"
        ".connection-status{padding:10px;border-radius:5px;margin:10px 0;text-align:center;font-weight:bold;}"
        ".connected{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}"
        ".disconnected{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;}"
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
        "button:disabled{background:#6c757d;cursor:not-allowed;}"
        ".presets{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px;margin:20px 0;}"
        ".preset{background:#f8f9fa;padding:10px;border-radius:5px;border:1px solid #dee2e6;cursor:pointer;text-align:center;}"
        ".preset:hover{background:#e9ecef;}"
        ".preset:disabled{background:#e9ecef;cursor:not-allowed;opacity:0.6;}"
        ".status{text-align:center;margin:20px 0;padding:10px;border-radius:5px;}"
        ".info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb;}"
        ".buffer-info{background:#fff3cd;color:#856404;border:1px solid #ffeaa7;margin:10px 0;padding:10px;border-radius:5px;font-size:14px;}"
        "</style></head><body>"
        "<div class='container'>"
        "<h1>🎵 ESP32-S3 Audio Streamer Control (WebSocket)</h1>"
        "<div id='connectionStatus' class='connection-status disconnected'>WebSocket: Disconnected</div>"
        "<div class='status info'>Current Stream: <strong id='currentStream'>None</strong></div>"
        "<div id='bufferInfo' class='buffer-info' style='display:none;'></div>"
        ""
        "<div class='volume-control'>"
        "<label for='volume'>🔊 Volume Control:</label>"
        "<div class='volume-display' id='volumeDisplay'>70%</div>"
        "<input type='range' id='volume' min='0' max='100' value='70' oninput='updateVolume(this.value)'>"
        "</div>"
        ""
        "<div class='equalizer-control'>"
        "<label>🎛️ 10-Band Equalizer (±13dB):</label>"
        "<div class='eq-bands'>"
        "<div class='eq-band'><label>32Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(0,this.value)' data-band='0'></div>"
        "<div class='eq-band'><label>64Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(1,this.value)' data-band='1'></div>"
        "<div class='eq-band'><label>125Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(2,this.value)' data-band='2'></div>"
        "<div class='eq-band'><label>250Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(3,this.value)' data-band='3'></div>"
        "<div class='eq-band'><label>500Hz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(4,this.value)' data-band='4'></div>"
        "<div class='eq-band'><label>1kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(5,this.value)' data-band='5'></div>"
        "<div class='eq-band'><label>2kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(6,this.value)' data-band='6'></div>"
        "<div class='eq-band'><label>4kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(7,this.value)' data-band='7'></div>"
        "<div class='eq-band'><label>8kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(8,this.value)' data-band='8'></div>"
        "<div class='eq-band'><label>16kHz</label><input type='range' min='-13' max='13' value='0' oninput='updateEqualizer(9,this.value)' data-band='9'></div>"
        "</div>"
        "<button type='button' onclick='resetEqualizer()' id='resetEqBtn'>Reset EQ</button>"
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
        "<div class='form-group'>"
        "<label for='url'>🌐 Stream URL:</label>"
        "<input type='text' id='url' placeholder='http://example.com/stream.mp3' value=''>"
        "<button type='button' onclick='setCustomStream()' id='setStreamBtn' style='margin-top:10px;'>Change Stream</button>"
        "</div>"
        ""
        "<h3>Quick Presets (Auto-Detected Format):</h3>"
        "<div class='presets'>"
        "<div class='preset' onclick='setStream(\"https://stream.radioparadise.com/aac-320\")'>🎵 Radio Paradise AAC<br><small>320kbps High Quality</small></div>"
        "<div class='preset' onclick='setStream(\"https://stream.zeno.fm/vq6p5vxb4v8uv\")'>🎵 MP3 Radio<br><small>Zeno.fm Stream</small></div>"
        "<div class='preset' onclick='setStream(\"https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac\")'>🎵 AAC File<br><small>Espressif Sample</small></div>"
        "<div class='preset' onclick='setStream(\"https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.wav\")'>🎵 WAV File<br><small>Espressif Sample</small></div>"
        "<div class='preset' onclick='setStream(\"https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.flac\")'>🎵 FLAC File<br><small>Espressif Sample</small></div>"
        "<div class='preset' onclick='setStream(\"https://stream.radioparadise.com/flac\")'>🎵 Radio Paradise FLAC<br><small>High Quality Stream</small></div>"
        "<div class='preset' onclick='setStream(\"https://ice1.somafm.com/groovesalad-256-aac\")'>🎵 SomaFM AAC<br><small>256kbps Ambient</small></div>"
        "<div class='preset' onclick='setStream(\"https://stream.rcs.revma.com/ypqt40u0x1zuv\")'>🎵 High Bitrate AAC<br><small>Test Stream</small></div>"
        "</div>"
        "<div style='text-align:center;margin-top:30px;color:#666;'>"
        "<p>⚡ ESP32-S3 with 8MB PSRAM | 🎛️ Auto-Decoder Selection | 📡 WebSocket Real-time Control</p>"
        "</div>"
        "</div>"
        "<script>\n"
        "let ws = null;\n"
        "let wsConnected = false;\n"
        "let reconnectInterval = null;\n"
        "\n"
        "function connectWebSocket() {\n"
        "  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';\n"
        "  const wsUrl = protocol + '//' + window.location.host + '/ws';\n"
        "  \n"
        "  console.log('Connecting to WebSocket:', wsUrl);\n"
        "  ws = new WebSocket(wsUrl);\n"
        "  \n"
        "  ws.onopen = function() {\n"
        "    console.log('WebSocket connected');\n"
        "    wsConnected = true;\n"
        "    updateConnectionStatus(true);\n"
        "    enableControls(true);\n"
        "    sendWebSocketMessage('get_status', {});\n"
        "    if (reconnectInterval) {\n"
        "      clearInterval(reconnectInterval);\n"
        "      reconnectInterval = null;\n"
        "    }\n"
        "  };\n"
        "  \n"
        "  ws.onmessage = function(event) {\n"
        "    try {\n"
        "      const message = JSON.parse(event.data);\n"
        "      handleWebSocketMessage(message);\n"
        "    } catch (e) {\n"
        "      console.error('Error parsing WebSocket message:', e);\n"
        "    }\n"
        "  };\n"
        "  \n"
        "  ws.onclose = function() {\n"
        "    console.log('WebSocket disconnected');\n"
        "    wsConnected = false;\n"
        "    updateConnectionStatus(false);\n"
        "    enableControls(false);\n"
        "    if (!reconnectInterval) {\n"
        "      reconnectInterval = setInterval(connectWebSocket, 3000);\n"
        "    }\n"
        "  };\n"
        "  \n"
        "  ws.onerror = function(error) {\n"
        "    console.error('WebSocket error:', error);\n"
        "  };\n"
        "}\n"
        "\n"
        "function sendWebSocketMessage(type, data) {\n"
        "  if (ws && wsConnected) {\n"
        "    const message = { type: type, data: data };\n"
        "    ws.send(JSON.stringify(message));\n"
        "  } else {\n"
        "    console.warn('WebSocket not connected, cannot send message');\n"
        "  }\n"
        "}\n"
        "\n"
        "function handleWebSocketMessage(message) {\n"
        "  console.log('Received WebSocket message:', message);\n"
        "  if (message.type === 'status_update' && message.data) {\n"
        "    const data = message.data;\n"
        "    document.getElementById('currentStream').textContent = data.current_url || 'None';\n"
        "    if (data.volume !== undefined) {\n"
        "      document.getElementById('volume').value = data.volume;\n"
        "      document.getElementById('volumeDisplay').textContent = data.volume + '%';\n"
        "    }\n"
        "    if (data.equalizer_gains && Array.isArray(data.equalizer_gains)) {\n"
        "      data.equalizer_gains.forEach((gain, index) => {\n"
        "        const slider = document.querySelector('input[data-band=\"' + index + '\"]');\n"
        "        if (slider) slider.value = gain;\n"
        "      });\n"
        "    }\n"
        "    const bufferInfo = document.getElementById('bufferInfo');\n"
        "    if (data.pipeline_running && data.http_buffer_kb !== undefined) {\n"
        "      bufferInfo.innerHTML = '📊 Buffers: HTTP ' + data.http_buffer_kb + 'KB | Decoder ' + data.decoder_buffer_kb + 'KB | 💾 Memory: ' + data.free_heap_kb + 'KB heap, ' + data.free_psram_kb + 'KB PSRAM';\n"
        "      bufferInfo.style.display = 'block';\n"
        "    } else {\n"
        "      bufferInfo.style.display = 'none';\n"
        "    }\n"
        "  }\n"
        "}\n"
        "\n"
        "function updateConnectionStatus(connected) {\n"
        "  const statusDiv = document.getElementById('connectionStatus');\n"
        "  if (connected) {\n"
        "    statusDiv.textContent = 'WebSocket: Connected ✅';\n"
        "    statusDiv.className = 'connection-status connected';\n"
        "  } else {\n"
        "    statusDiv.textContent = 'WebSocket: Disconnected ❌ (Reconnecting...)';\n"
        "    statusDiv.className = 'connection-status disconnected';\n"
        "  }\n"
        "}\n"
        "\n"
        "function enableControls(enabled) {\n"
        "  const controls = ['volume', 'setStreamBtn', 'resetEqBtn', 'ledManual'];\n"
        "  controls.forEach(id => {\n"
        "    const element = document.getElementById(id);\n"
        "    if (element) element.disabled = !enabled;\n"
        "  });\n"
        "  document.querySelectorAll('.eq-band input[type=range]').forEach(slider => {\n"
        "    slider.disabled = !enabled;\n"
        "  });\n"
        "  document.querySelectorAll('.preset').forEach(preset => {\n"
        "    if (enabled) {\n"
        "      preset.style.pointerEvents = 'auto';\n"
        "      preset.style.opacity = '1';\n"
        "    } else {\n"
        "      preset.style.pointerEvents = 'none';\n"
        "      preset.style.opacity = '0.6';\n"
        "    }\n"
        "  });\n"
        "}\n"
        "\n"
        "function setStream(url) {\n"
        "  document.getElementById('url').value = url;\n"
        "  sendWebSocketMessage('set_stream', { url: url });\n"
        "}\n"
        "\n"
        "function setCustomStream() {\n"
        "  const url = document.getElementById('url').value.trim();\n"
        "  if (url) {\n"
        "    sendWebSocketMessage('set_stream', { url: url });\n"
        "  } else {\n"
        "    alert('Please enter a valid stream URL');\n"
        "  }\n"
        "}\n"
        "\n"
        "function updateVolume(value) {\n"
        "  document.getElementById('volumeDisplay').textContent = value + '%';\n"
        "  sendWebSocketMessage('set_volume', { volume: parseInt(value) });\n"
        "}\n"
        "\n"
        "function updateEqualizer(band, value) {\n"
        "  sendWebSocketMessage('set_equalizer', { band: band, gain: parseInt(value) });\n"
        "}\n"
        "\n"
        "function resetEqualizer() {\n"
        "  sendWebSocketMessage('reset_equalizer', {});\n"
        "  document.querySelectorAll('.eq-band input[type=range]').forEach(slider => {\n"
        "    slider.value = 0;\n"
        "  });\n"
        "}\n"
        "\n"
        "function toggleLedMode(manual) {\n"
        "  document.getElementById('ledControls').style.display = manual ? 'block' : 'none';\n"
        "  sendWebSocketMessage('set_led_mode', { manual: manual });\n"
        "}\n"
        "\n"
        "function updateLedColor() {\n"
        "  const red = parseInt(document.getElementById('ledRed').value);\n"
        "  const green = parseInt(document.getElementById('ledGreen').value);\n"
        "  const blue = parseInt(document.getElementById('ledBlue').value);\n"
        "  sendWebSocketMessage('set_led_color', { red: red, green: green, blue: blue });\n"
        "}\n"
        "\n"
        "function setLedPreset(red, green, blue) {\n"
        "  document.getElementById('ledRed').value = red;\n"
        "  document.getElementById('ledGreen').value = green;\n"
        "  document.getElementById('ledBlue').value = blue;\n"
        "  updateLedColor();\n"
        "}\n"
        "\n"
        "document.addEventListener('DOMContentLoaded', function() {\n"
        "  document.getElementById('url').addEventListener('keypress', function(e) {\n"
        "    if (e.key === 'Enter') {\n"
        "      setCustomStream();\n"
        "    }\n"
        "  });\n"
        "  connectWebSocket();\n"
        "  enableControls(false);\n"
        "});\n"
        "</script>\n"
        "</body></html>";

    char response[16384]; // Increased buffer size for larger HTML
    snprintf(response, sizeof(response), "%s", html_page);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t set_stream_handler(httpd_req_t *req)
{
    char url_param[512];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;

    if (buf_len > 1)
    {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            if (httpd_query_key_value(buf, "url", url_param, sizeof(url_param)) == ESP_OK)
            {
                // URL decode
                char *decoded_url = malloc(strlen(url_param) + 1);
                if (decoded_url)
                {
                    // Simple URL decode for %20 -> space, etc.
                    char *src = url_param;
                    char *dst = decoded_url;
                    while (*src)
                    {
                        if (*src == '%' && src[1] && src[2])
                        {
                            char hex[3] = {src[1], src[2], 0};
                            *dst++ = (char)strtol(hex, NULL, 16);
                            src += 3;
                        }
                        else if (*src == '+')
                        {
                            *dst++ = ' ';
                            src++;
                        }
                        else
                        {
                            *dst++ = *src++;
                        }
                    }
                    *dst = 0;

                    // Update stream URL safely
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                    {
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

    if (buf_len > 1)
    {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "volume", volume_param, sizeof(volume_param)) == ESP_OK)
            {
                int new_volume = atoi(volume_param);
                ESP_LOGI("HTTP_SERVER", "Parsed volume: %d%%", new_volume);

                // Validate volume range
                if (new_volume >= 0 && new_volume <= 100)
                {
                    // Update volume safely
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                    {
                        current_volume = new_volume;
                        volume_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);

                        ESP_LOGI("HTTP_SERVER", "✅ Volume changed to: %d%% (flag set)", current_volume);
                    }
                    else
                    {
                        ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for volume change");
                    }
                }
                else
                {
                    ESP_LOGW("HTTP_SERVER", "⚠️ Invalid volume range: %d%%", new_volume);
                }
            }
            else
            {
                ESP_LOGW("HTTP_SERVER", "⚠️ Failed to parse volume parameter");
            }
        }
        else
        {
            ESP_LOGW("HTTP_SERVER", "⚠️ Failed to get query string");
        }
        free(buf);
    }
    else
    {
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

    if (buf_len > 1)
    {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "band", band_param, sizeof(band_param)) == ESP_OK &&
                httpd_query_key_value(buf, "gain", gain_param, sizeof(gain_param)) == ESP_OK)
            {

                int band = atoi(band_param);
                int gain = atoi(gain_param);
                ESP_LOGI("HTTP_SERVER", "Parsed EQ: band=%d, gain=%ddB", band, gain);

                // Validate band and gain ranges
                if (band >= 0 && band < 10 && gain >= -13 && gain <= 13)
                {
                    // Update equalizer settings safely
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                    {
                        equalizer_gains[band] = gain;
                        equalizer_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);

                        ESP_LOGI("HTTP_SERVER", "✅ Equalizer band %d set to: %ddB (flag set)", band, gain);
                    }
                    else
                    {
                        ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for equalizer change");
                    }
                }
                else
                {
                    ESP_LOGW("HTTP_SERVER", "⚠️ Invalid EQ parameters: band=%d, gain=%d", band, gain);
                }
            }
            else
            {
                ESP_LOGW("HTTP_SERVER", "⚠️ Failed to parse equalizer parameters");
            }
        }
        else
        {
            ESP_LOGW("HTTP_SERVER", "⚠️ Failed to get query string");
        }
        free(buf);
    }
    else
    {
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

    if (buf_len > 1)
    {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "manual", manual_param, sizeof(manual_param)) == ESP_OK)
            {
                bool manual = (atoi(manual_param) == 1);
                ESP_LOGI("HTTP_SERVER", "LED mode: %s", manual ? "Manual" : "Auto");

                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    led_manual_mode = manual;
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("HTTP_SERVER", "✅ LED mode set to: %s", manual ? "Manual" : "Auto");
                }
                else
                {
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

    if (buf_len > 1)
    {
        char *buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI("HTTP_SERVER", "Query string: %s", buf);
            if (httpd_query_key_value(buf, "red", red_param, sizeof(red_param)) == ESP_OK &&
                httpd_query_key_value(buf, "green", green_param, sizeof(green_param)) == ESP_OK &&
                httpd_query_key_value(buf, "blue", blue_param, sizeof(blue_param)) == ESP_OK)
            {

                int red = atoi(red_param);
                int green = atoi(green_param);
                int blue = atoi(blue_param);
                ESP_LOGI("HTTP_SERVER", "LED color: R:%d G:%d B:%d", red, green, blue);

                // Validate color ranges
                if (red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 && blue <= 255)
                {
                    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                    {
                        led_color_red = red;
                        led_color_green = green;
                        led_color_blue = blue;
                        led_change_requested = true;
                        xSemaphoreGive(stream_control_mutex);
                        ESP_LOGI("HTTP_SERVER", "✅ LED color set to R:%d G:%d B:%d", red, green, blue);
                    }
                    else
                    {
                        ESP_LOGE("HTTP_SERVER", "❌ Failed to acquire mutex for LED color change");
                    }
                }
                else
                {
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
    char status_json[2048]; // Increased size for more detailed buffer info

    // Get current buffer status with detailed information
    int http_filled = 0, decoder_filled = 0, http_total = 0, decoder_total = 0;
    int eq_filled = 0, i2s_filled = 0;

    if (global_http_stream && global_decoder)
    {
        http_filled = audio_element_get_output_ringbuf_size(global_http_stream);
        decoder_filled = audio_element_get_output_ringbuf_size(global_decoder);

        // Get total buffer sizes for percentage calculation
        audio_element_info_t http_info = {0};
        audio_element_info_t decoder_info = {0};
        audio_element_getinfo(global_http_stream, &http_info);
        audio_element_getinfo(global_decoder, &decoder_info);
        http_total = 2 * 1024 * 1024; // Our configured 2MB HTTP buffer for FLAC

        // Set decoder total based on format
        switch (current_audio_format)
        {
        case AUDIO_FORMAT_AAC:
            decoder_total = 1024 * 1024;
            break; // 2MB
        case AUDIO_FORMAT_FLAC:
            decoder_total = 1024 * 1024;
            break; // 1MB
        case AUDIO_FORMAT_MP3:
            decoder_total = 512 * 1024;
            break; // 512KB
        case AUDIO_FORMAT_WAV:
            decoder_total = 256 * 1024;
            break; // 256KB
        case AUDIO_FORMAT_OGG:
            decoder_total = 512 * 1024;
            break; // 512KB
        default:
            decoder_total = 512 * 1024;
            break;
        }

        // Get equalizer and I2S buffer status
        if (global_equalizer)
        {
            eq_filled = audio_element_get_output_ringbuf_size(global_equalizer);
        }
        if (global_i2s_stream)
        {
            i2s_filled = audio_element_get_output_ringbuf_size(global_i2s_stream);
        }
    }

    // Get memory status
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_free_heap = esp_get_minimum_free_heap_size();

    // Calculate buffer fill percentages
    int http_percent = (http_total > 0) ? (http_filled * 100 / http_total) : 0;
    int decoder_percent = (decoder_total > 0) ? (decoder_filled * 100 / decoder_total) : 0;

    // Determine buffer health status
    const char *buffer_health = "healthy";
    if (http_percent < 10 || http_percent > 95 || decoder_percent > 90 || free_heap < 50 * 1024)
    {
        buffer_health = "warning";
    }
    if (free_heap < 25 * 1024)
    {
        buffer_health = "critical";
    }

    snprintf(status_json, sizeof(status_json),
             "{"
             "\"current_url\":\"%s\","
             "\"decoder\":\"%s\","
             "\"volume\":%d,"
             "\"http_buffer_kb\":%d,"
             "\"http_buffer_total_kb\":%d,"
             "\"http_buffer_percent\":%d,"
             "\"decoder_buffer_kb\":%d,"
             "\"decoder_buffer_total_kb\":%d,"
             "\"decoder_buffer_percent\":%d,"
             "\"equalizer_buffer_kb\":%d,"
             "\"i2s_buffer_kb\":%d,"
             "\"free_heap_kb\":%zu,"
             "\"free_psram_kb\":%zu,"
             "\"min_free_heap_kb\":%zu,"
             "\"buffer_health\":\"%s\","
             "\"pipeline_running\":%s"
             "}",
             current_stream_url,
             current_decoder_name,
             current_volume,
             http_filled / 1024,
             http_total / 1024,
             http_percent,
             decoder_filled / 1024,
             decoder_total / 1024,
             decoder_percent,
             eq_filled / 1024,
             i2s_filled / 1024,
             free_heap / 1024,
             free_psram / 1024,
             min_free_heap / 1024,
             buffer_health,
             global_pipeline ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, status_json, strlen(status_json));
    return ESP_OK;
}

static esp_err_t buffer_status_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP_SERVER", "📊 Manual buffer status request received");

    // Log detailed buffer status only when manually requested (not in timer)
    // This avoids stack overflow in timer service task
    if (!global_pipeline || !global_http_stream || !global_decoder)
    {
        httpd_resp_send(req, "Pipeline not initialized", 24);
        return ESP_OK;
    }

    // Get buffer information
    int http_filled = audio_element_get_output_ringbuf_size(global_http_stream);
    int decoder_filled = audio_element_get_output_ringbuf_size(global_decoder);
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // Calculate percentages
    int http_percent = (http_filled * 100) / (1.5 * 1024 * 1024); // 2MB buffer
    int decoder_total = 0;
    switch (current_audio_format)
    {
    case AUDIO_FORMAT_AAC:
        decoder_total = 1024;
        break;
    case AUDIO_FORMAT_FLAC:
        decoder_total = 1024;
        break;
    default:
        decoder_total = 512;
        break;
    }
    int decoder_percent = (decoder_filled * 100) / (decoder_total * 1024);

    ESP_LOGI("BUFFER_MONITOR", "=== DETAILED BUFFER STATUS ===");
    ESP_LOGI("BUFFER_MONITOR", "Stream: %s | Format: %s", current_stream_url, current_decoder_name);
    ESP_LOGI("BUFFER_MONITOR", "HTTP: %d KB (%d%%) | Decoder: %d KB (%d%%)",
             http_filled / 1024, http_percent, decoder_filled / 1024, decoder_percent);
    ESP_LOGI("BUFFER_MONITOR", "Memory: Heap=%zu KB | PSRAM=%zu KB", free_heap / 1024, free_psram / 1024);
    ESP_LOGI("BUFFER_MONITOR", "===============================");

    // Return simple response
    httpd_resp_send(req, "Detailed buffer status logged to console", 41);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192 * 4; // Increase to 32KB for WebSocket and large HTML
    config.max_uri_handlers = 16; // Increase handler limit

    ESP_LOGI("HTTP_SERVER", "Starting HTTP server on port 80 with 32KB stack");

    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Root page
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &root_uri);

        // Set stream URL
        httpd_uri_t set_stream_uri = {
            .uri = "/set_stream",
            .method = HTTP_GET,
            .handler = set_stream_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &set_stream_uri);

        // Set volume
        httpd_uri_t set_volume_uri = {
            .uri = "/set_volume",
            .method = HTTP_GET,
            .handler = set_volume_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &set_volume_uri);

        // Set equalizer
        httpd_uri_t set_equalizer_uri = {
            .uri = "/set_equalizer",
            .method = HTTP_GET,
            .handler = set_equalizer_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &set_equalizer_uri);

        // Set LED mode
        httpd_uri_t set_led_mode_uri = {
            .uri = "/set_led_mode",
            .method = HTTP_GET,
            .handler = set_led_mode_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &set_led_mode_uri);

        // Set LED color
        httpd_uri_t set_led_color_uri = {
            .uri = "/set_led_color",
            .method = HTTP_GET,
            .handler = set_led_color_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &set_led_color_uri);

        // Status API
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &status_uri);

        // Buffer Status API
        httpd_uri_t buffer_status_uri = {
            .uri = "/buffer_status",
            .method = HTTP_GET,
            .handler = buffer_status_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &buffer_status_uri);

        // WebSocket endpoint
        httpd_uri_t websocket_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = websocket_handler,
            .user_ctx = NULL,
            .is_websocket = true};
        httpd_register_uri_handler(server, &websocket_uri);

        ESP_LOGI("HTTP_SERVER", "✅ HTTP server started successfully with WebSocket support");
        return server;
    }

    ESP_LOGE("HTTP_SERVER", "❌ Failed to start HTTP server");
    return NULL;
}

// Start audio pipeline with given URL
static void start_audio_pipeline(const char *url)
{
    ESP_LOGI("AUDIO_PLAYER", "🎵 Starting audio pipeline with URL: %s", url);

    // Create event interface
    if (!evt)
    {
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        evt_cfg.queue_set_size = 3; // Smaller queue to prevent overflow
        evt = audio_event_iface_init(&evt_cfg);
        if (!evt)
        {
            ESP_LOGE("AUDIO_PLAYER", "Failed to create event interface");
            return;
        }
    }

    // Create pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    global_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!global_pipeline)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create audio pipeline");
        return;
    }

    // Create HTTP stream optimized for high-resolution streaming
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.event_handle = _http_stream_event_handle;
    http_cfg.out_rb_size = 1.5 * 1024 * 1024; // 2MB buffer for FLAC streaming
    http_cfg.task_stack = 4096 * 12;          // Larger stack for high-bandwidth streams
    http_cfg.task_prio = 12;                  // High priority for streaming
    http_cfg.task_core = 1;                   // Pin to core 1
    http_cfg.enable_playlist_parser = true;   // Enable playlist parser
    http_cfg.request_size = 0;
    http_cfg.request_range_size = 0;
    global_http_stream = http_stream_init(&http_cfg);
    if (!global_http_stream)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create HTTP stream");
        goto cleanup_pipeline;
    }

    // Detect audio format from URL and create appropriate decoder
    current_audio_format = detect_audio_format_from_url(url);
    strncpy(current_decoder_name, get_decoder_name(current_audio_format), sizeof(current_decoder_name) - 1);
    current_decoder_name[sizeof(current_decoder_name) - 1] = 0;

    ESP_LOGI("AUDIO_PLAYER", "🔍 Detected audio format: %s from URL: %s", current_decoder_name, url);

    global_decoder = create_decoder_for_format(current_audio_format);
    if (!global_decoder)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create %s decoder", current_decoder_name);
        goto cleanup_http;
    }

    ESP_LOGI("AUDIO_PLAYER", "✅ Created %s decoder successfully", current_decoder_name);

    // Create equalizer for volume control (following ESP-ADF example)
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    // Create gain array - 10 bands for equalizer (ESP-ADF supports 10 bands total)
    static int eq_gain[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // Start with flat response
    eq_cfg.set_gain = eq_gain;
    eq_cfg.task_stack = 4096 * 6; // Increase stack size
    eq_cfg.task_prio = 5;         // Set priority
    eq_cfg.task_core = 1;         // Pin to core 1
    global_equalizer = equalizer_init(&eq_cfg);
    if (!global_equalizer)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create equalizer");
        goto cleanup_decoder;
    }
    ESP_LOGI("AUDIO_PLAYER", "🎛️  Equalizer created for volume control");

    // Create I2S stream optimized for high-resolution audio
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.buffer_len = 1024 * 36; // Larger buffer for high-res audio (128KB)
    i2s_cfg.use_alc = true;         // Enable ALC for volume control
    i2s_cfg.task_stack = 4096 * 4;  // Larger stack for I2S stream
    // High-resolution I2S configuration for FLAC and other lossless formats
    // Support up to 192kHz/32-bit audio
    i2s_cfg.chan_cfg.dma_desc_num = 8;     // More DMA buffers for smoother playback
    i2s_cfg.chan_cfg.dma_frame_num = 1023; // Larger DMA buffer length

    // Configure ESP32-S3 as I2S Master but WITHOUT MCLK output
    // ESP32 generates BCLK and LRCLK only, UDA1334A uses its internal PLL

    // Clock source options for ESP32-S3 I2S generation (does NOT affect UDA1334A):
    // I2S_CLK_SRC_DEFAULT - Use default PLL (good balance of accuracy and power)
    // I2S_CLK_SRC_PLL_160M - Use 160MHz PLL (higher accuracy, more power)
    // I2S_CLK_SRC_XTAL - Use crystal oscillator (lower power, less accurate)
    i2s_cfg.std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT; // ESP32's internal clock source

    // Disable MCLK output - UDA1334A will use its internal PLL instead
    i2s_cfg.std_cfg.gpio_cfg.mclk = GPIO_NUM_NC; // No MCLK pin connection

    global_i2s_stream = i2s_stream_init(&i2s_cfg);
    if (!global_i2s_stream)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create I2S stream");
        goto cleanup_equalizer;
    }
    ESP_LOGI("AUDIO_PLAYER", "🔊 I2S Master mode: ESP32-S3 provides BCLK & LRCLK only (MCLK disabled - PCM1334A uses internal PLL)");

    // Register elements
    if (audio_pipeline_register(global_pipeline, global_http_stream, "http") != ESP_OK)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register HTTP stream");
        goto cleanup_i2s;
    }

    if (audio_pipeline_register(global_pipeline, global_decoder, current_decoder_name) != ESP_OK)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register %s decoder", current_decoder_name);
        goto cleanup_i2s;
    }

    if (audio_pipeline_register(global_pipeline, global_equalizer, "equalizer") != ESP_OK)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register equalizer");
        goto cleanup_i2s;
    }

    if (audio_pipeline_register(global_pipeline, global_i2s_stream, "i2s") != ESP_OK)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to register I2S stream");
        goto cleanup_i2s;
    }

    // Link elements: http -> decoder -> equalizer -> i2s
    const char *link_tag[4] = {"http", current_decoder_name, "equalizer", "i2s"};
    if (audio_pipeline_link(global_pipeline, &link_tag[0], 4) != ESP_OK)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to link pipeline elements");
        goto cleanup_i2s;
    }

    // Set URI and listener
    audio_element_set_uri(global_http_stream, url);
    audio_pipeline_set_listener(global_pipeline, evt);

    // Start pipeline
    if (audio_pipeline_run(global_pipeline) != ESP_OK)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to start pipeline");
        goto cleanup_i2s;
    }

    // Update LED for new stream
    update_led_for_stream(url, true);

    // Start buffer monitoring for this stream and WebSocket updates
    start_buffer_monitoring();

    ESP_LOGI("AUDIO_PLAYER", "✅ Audio pipeline started successfully");
    ESP_LOGI("AUDIO_PLAYER", "💡 Use /buffer_status endpoint for manual buffer monitoring");

    // Broadcast status update via WebSocket
    broadcast_status_update();

    return;

cleanup_i2s:
    if (global_i2s_stream)
    {
        audio_element_deinit(global_i2s_stream);
        global_i2s_stream = NULL;
    }
cleanup_equalizer:
    if (global_equalizer)
    {
        audio_element_deinit(global_equalizer);
        global_equalizer = NULL;
    }
cleanup_decoder:
    if (global_decoder)
    {
        audio_element_deinit(global_decoder);
        global_decoder = NULL;
    }
cleanup_http:
    if (global_http_stream)
    {
        audio_element_deinit(global_http_stream);
        global_http_stream = NULL;
    }
cleanup_pipeline:
    if (global_pipeline)
    {
        audio_pipeline_deinit(global_pipeline);
        global_pipeline = NULL;
    }
    ESP_LOGE("AUDIO_PLAYER", "❌ Failed to start audio pipeline");
}

// Update volume for the current pipeline
static void update_pipeline_volume(int volume_percent)
{
    if (global_i2s_stream)
    {
        ESP_LOGI("AUDIO_PLAYER", "🔊 Setting volume to %d%% using ALC (current_volume global = %d%%)", volume_percent, current_volume);

        // Convert percentage (0-100) to ALC volume level with better curve
        // ALC volume range is typically -96dB to 0dB
        // Use a more user-friendly logarithmic-like curve for better control
        int alc_volume;
        if (volume_percent == 0)
        {
            alc_volume = -96; // Minimum volume (mute)
        }
        else if (volume_percent <= 10)
        {
            // Map 1-10% to -60dB to -40dB (very quiet but audible)
            alc_volume = -60 + ((volume_percent - 1) * 20) / 9;
        }
        else if (volume_percent <= 50)
        {
            // Map 11-50% to -40dB to -20dB (low to medium)
            alc_volume = -40 + ((volume_percent - 10) * 20) / 40;
        }
        else if (volume_percent <= 80)
        {
            // Map 51-80% to -20dB to -10dB (medium to high)
            alc_volume = -20 + ((volume_percent - 50) * 10) / 30;
        }
        else
        {
            // Map 81-100% to -10dB to 0dB (high to maximum)
            alc_volume = -10 + ((volume_percent - 80) * 10) / 20;
        }

        // Ensure bounds
        if (alc_volume > 0)
            alc_volume = 0;
        if (alc_volume < -96)
            alc_volume = -96;

        // Set ALC volume on I2S stream
        esp_err_t ret = i2s_alc_volume_set(global_i2s_stream, alc_volume);
        if (ret != ESP_OK)
        {
            ESP_LOGE("AUDIO_PLAYER", "Failed to set ALC volume: %s", esp_err_to_name(ret));
            return;
        }

        ESP_LOGI("AUDIO_PLAYER", "✅ Volume updated to %d%% (ALC: %ddB)", volume_percent, alc_volume);
    }
    else
    {
        ESP_LOGW("AUDIO_PLAYER", "⚠️  No I2S stream available for volume control");
    }
}

// Update equalizer settings for the current pipeline
static void update_pipeline_equalizer(void)
{
    if (global_equalizer)
    {
        ESP_LOGI("AUDIO_PLAYER", "🎛️  Updating equalizer settings");

        // ESP-ADF equalizer has 10 bands total (0-9), not per channel
        // Apply current equalizer gains directly to the 10 available bands
        for (int band = 0; band < 10; band++)
        {
            esp_err_t ret = equalizer_set_gain_info(global_equalizer, band, equalizer_gains[band], true);
            if (ret != ESP_OK)
            {
                ESP_LOGE("AUDIO_PLAYER", "Failed to set equalizer gain for band %d: %s", band, esp_err_to_name(ret));
                continue;
            }
        }

        ESP_LOGI("AUDIO_PLAYER", "✅ Equalizer settings updated");
    }
    else
    {
        ESP_LOGW("AUDIO_PLAYER", "⚠️  No equalizer available");
    }
}

// Timer callback for periodic buffer monitoring (lightweight version)
static void buffer_monitor_timer_callback(TimerHandle_t xTimer)
{
    // Use minimal stack - just basic buffer status
    if (!global_pipeline || !global_http_stream || !global_decoder)
    {
        return;
    }

    // Simple buffer check with minimal stack usage
    int http_filled = audio_element_get_output_ringbuf_size(global_http_stream);
    int decoder_filled = audio_element_get_output_ringbuf_size(global_decoder);
    size_t free_heap = esp_get_free_heap_size();

    // Simple logging to avoid stack overflow
    ESP_LOGI("BUFFER", "HTTP:%dKB Decoder:%dKB Heap:%zuKB",
             http_filled / 1024, decoder_filled / 1024, free_heap / 1024);

    // Only warn on critical issues
    if (free_heap < 30 * 1024)
    {
        ESP_LOGW("BUFFER", "Low heap: %zu KB", free_heap / 1024);
    }

    // DO NOT call broadcast_status_update() from timer - causes stack overflow
    // WebSocket updates will be sent on user actions instead
}

// Start buffer monitoring timer
static void start_buffer_monitoring(void)
{
    if (buffer_monitor_timer == NULL)
    {
        buffer_monitor_timer = xTimerCreate(
            "buffer_monitor",                      // Timer name
            pdMS_TO_TICKS(BUFFER_LOG_INTERVAL_MS), // Timer period (5 seconds)
            pdTRUE,                                // Auto-reload timer
            NULL,                                  // Timer ID (not used)
            buffer_monitor_timer_callback          // Callback function
        );

        if (buffer_monitor_timer != NULL)
        {
            if (xTimerStart(buffer_monitor_timer, 0) == pdPASS)
            {
                ESP_LOGI("BUFFER_MONITOR", "✅ Buffer monitoring started (interval: %d ms)", BUFFER_LOG_INTERVAL_MS);
            }
            else
            {
                ESP_LOGE("BUFFER_MONITOR", "❌ Failed to start buffer monitoring timer");
            }
        }
        else
        {
            ESP_LOGE("BUFFER_MONITOR", "❌ Failed to create buffer monitoring timer");
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("wifi_idf", ESP_LOG_INFO);
    esp_log_level_set("HTTP_SERVER", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_PLAYER", ESP_LOG_INFO);
    esp_log_level_set("BUFFER_MONITOR", ESP_LOG_INFO); // Enable buffer monitoring logs

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize stream control mutex
    stream_control_mutex = xSemaphoreCreateMutex();
    if (stream_control_mutex == NULL)
    {
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

    if (bits & WIFI_CONNECTED_BIT)
    {
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
        while (1)
        {
            // Check for stream change requests - but not if we're restarting
            bool change_requested = false;
            bool volume_change = false;
            bool equalizer_change = false;
            bool led_change = false;
            char new_url[512] = {0};
            int new_volume = 0;
            int new_led_red = 0, new_led_green = 0, new_led_blue = 0;

            if (!pipeline_restarting && xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                if (stream_change_requested)
                {
                    change_requested = true;
                    strncpy(new_url, current_stream_url, sizeof(new_url) - 1);
                    stream_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "📡 Stream change detected");
                }
                if (volume_change_requested)
                {
                    volume_change = true;
                    new_volume = current_volume;
                    volume_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "🔊 Volume change detected: %d%%", new_volume);
                }
                if (equalizer_change_requested)
                {
                    equalizer_change = true;
                    equalizer_change_requested = false;
                    ESP_LOGI("AUDIO_PLAYER", "🎛️ Equalizer change detected");
                }
                if (led_change_requested)
                {
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
            if (volume_change)
            {
                ESP_LOGI("AUDIO_PLAYER", "🔊 Processing volume change to %d%%", new_volume);
                update_pipeline_volume(new_volume);
                broadcast_status_update();
            }

            // Handle equalizer changes
            if (equalizer_change)
            {
                ESP_LOGI("AUDIO_PLAYER", "🎛️ Processing equalizer changes");
                update_pipeline_equalizer();
                broadcast_status_update();
            }

            // Handle LED changes
            if (led_change)
            {
                ESP_LOGI("AUDIO_PLAYER", "💡 Processing LED color change to R:%d G:%d B:%d", new_led_red, new_led_green, new_led_blue);
                set_led_color_manual(new_led_red, new_led_green, new_led_blue);
            }

            if (change_requested)
            {
                ESP_LOGI("AUDIO_PLAYER", "🔄 Changing stream to: %s", new_url);
                pipeline_restarting = true;

                // Stop buffer monitoring during pipeline restart (disabled to prevent stack overflow)
                // stop_buffer_monitoring();

                // Stop current pipeline if running - proper cleanup sequence
                if (global_pipeline)
                {
                    ESP_LOGI("AUDIO_PLAYER", "Stopping current pipeline...");

                    // Stop pipeline first
                    audio_pipeline_stop(global_pipeline);
                    audio_pipeline_wait_for_stop(global_pipeline);

                    // Remove listener before cleanup
                    audio_pipeline_remove_listener(global_pipeline);

                    // Terminate pipeline
                    audio_pipeline_terminate(global_pipeline);

                    // Unregister elements from pipeline
                    if (global_http_stream)
                    {
                        audio_pipeline_unregister(global_pipeline, global_http_stream);
                    }
                    if (global_decoder)
                    {
                        audio_pipeline_unregister(global_pipeline, global_decoder);
                    }
                    if (global_equalizer)
                    {
                        audio_pipeline_unregister(global_pipeline, global_equalizer);
                    }
                    if (global_i2s_stream)
                    {
                        audio_pipeline_unregister(global_pipeline, global_i2s_stream);
                    }

                    // Deinitialize elements individually
                    if (global_http_stream)
                    {
                        audio_element_deinit(global_http_stream);
                        global_http_stream = NULL;
                    }
                    if (global_decoder)
                    {
                        audio_element_deinit(global_decoder);
                        global_decoder = NULL;
                    }
                    if (global_equalizer)
                    {
                        audio_element_deinit(global_equalizer);
                        global_equalizer = NULL;
                    }
                    if (global_i2s_stream)
                    {
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
            }
            else if (!global_pipeline && !pipeline_restarting)
            {
                // Start stream if we have a URL and no pipeline is running
                if (strlen(current_stream_url) > 0)
                {
                    start_audio_pipeline(current_stream_url);
                }
                else
                {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }

            // Monitor pipeline status
            if (global_pipeline)
            {
                audio_event_iface_msg_t msg;
                esp_err_t ret = audio_event_iface_listen(evt, &msg, pdMS_TO_TICKS(100));

                if (ret == ESP_OK)
                {
                    // Log all important events for debugging
                    ESP_LOGI("AUDIO_EVENT", "Event: src=%p cmd=%d data=%d source_type=%d",
                             msg.source, (int)msg.cmd, (int)msg.data, (int)msg.source_type);

                    // Handle music info events for automatic I2S configuration
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
                    {

                        audio_element_info_t music_info = {0};
                        const char *source_name = "Unknown";

                        if (msg.source == (void *)global_http_stream)
                        {
                            audio_element_getinfo(global_http_stream, &music_info);
                            source_name = "HTTP Stream";
                        }
                        else if (msg.source == (void *)global_decoder)
                        {
                            audio_element_getinfo(global_decoder, &music_info);
                            source_name = "Decoder";
                        }

                        // Only configure I2S if we have valid audio parameters
                        if (global_i2s_stream && music_info.sample_rates > 0 &&
                            music_info.channels > 0 && music_info.bits > 0)
                        {
                            ESP_LOGI("AUDIO_PLAYER", "🎵 %s info - Sample rate: %dHz, Channels: %d, Bits: %d",
                                     source_name, music_info.sample_rates, music_info.channels, music_info.bits);
                            ESP_LOGI("AUDIO_PLAYER", "🔧 Configuring I2S: %dHz, %d-bit, %d-channel",
                                     music_info.sample_rates, music_info.bits, music_info.channels);

                            // Configure I2S with detected parameters
                            esp_err_t i2s_ret = i2s_stream_set_clk(global_i2s_stream, music_info.sample_rates,
                                                                   music_info.bits, music_info.channels);
                            if (i2s_ret != ESP_OK)
                            {
                                ESP_LOGE("AUDIO_PLAYER", "Failed to configure I2S: %s", esp_err_to_name(i2s_ret));
                            }

                            // Configure equalizer with audio parameters
                            if (global_equalizer)
                            {
                                esp_err_t eq_ret = equalizer_set_info(global_equalizer, music_info.sample_rates, music_info.channels);
                                if (eq_ret != ESP_OK)
                                {
                                    ESP_LOGE("AUDIO_PLAYER", "Failed to configure equalizer: %s", esp_err_to_name(eq_ret));
                                }
                                else
                                {
                                    ESP_LOGI("AUDIO_PLAYER", "🎛️  Equalizer configured for %dHz, %d channels",
                                             music_info.sample_rates, music_info.channels);
                                    // Apply current equalizer settings
                                    update_pipeline_equalizer();
                                }
                            }

                            // Set initial ALC volume (default volume, not 0%)
                            if (global_i2s_stream)
                            {
                                update_pipeline_volume(current_volume);
                            }
                        }
                    }

                    // Handle file completion messages
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.cmd == AEL_MSG_CMD_FINISH)
                    {
                        ESP_LOGI("AUDIO_PLAYER", "🎵 File playback completed successfully");
                        // File finished normally - clean up pipeline and wait for new stream
                        pipeline_restarting = true;

                        // Stop and cleanup current pipeline
                        if (global_pipeline)
                        {
                            audio_pipeline_stop(global_pipeline);
                            audio_pipeline_wait_for_stop(global_pipeline);
                            audio_pipeline_remove_listener(global_pipeline);
                            audio_pipeline_terminate(global_pipeline);

                            // Cleanup elements
                            if (global_http_stream)
                            {
                                audio_pipeline_unregister(global_pipeline, global_http_stream);
                                audio_element_deinit(global_http_stream);
                                global_http_stream = NULL;
                            }
                            if (global_decoder)
                            {
                                audio_pipeline_unregister(global_pipeline, global_decoder);
                                audio_element_deinit(global_decoder);
                                global_decoder = NULL;
                            }
                            if (global_equalizer)
                            {
                                audio_pipeline_unregister(global_pipeline, global_equalizer);
                                audio_element_deinit(global_equalizer);
                                global_equalizer = NULL;
                            }
                            if (global_i2s_stream)
                            {
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
                        continue; // Continue loop to check for new stream requests
                    }

                    // Handle pipeline status changes - distinguish between completion and errors
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
                    {

                        audio_element_status_t status = (audio_element_status_t)msg.data;
                        if (status == AEL_STATUS_STATE_STOPPED || status == AEL_STATUS_STATE_FINISHED)
                        {
                            ESP_LOGW("AUDIO_PLAYER", "⚠️  Element %p stopped/finished (status: %d)", msg.source, status);

                            // Check if this is the HTTP stream finishing (file completed successfully)
                            if (msg.source == (void *)global_http_stream)
                            {
                                // Check if it's a successful completion (no error) or actual network issue
                                if (status == AEL_STATUS_STATE_FINISHED)
                                {
                                    ESP_LOGI("AUDIO_PLAYER", "🎵 HTTP stream completed successfully");
                                    
                                }
                                else
                                {
                                    ESP_LOGW("AUDIO_PLAYER", "HTTP stream stopped - this might be network issue");
                                }
                            }
                            else if (msg.source == (void *)global_decoder)
                            {
                                if (status == AEL_STATUS_STATE_FINISHED)
                                {
                                    ESP_LOGI("AUDIO_PLAYER", "🎵 Decoder finished successfully");
                                    // Let the AEL_MSG_CMD_FINISH handler take care of cleanup
                                    continue;
                                }
                                else
                                {
                                    ESP_LOGW("AUDIO_PLAYER", "Decoder stopped - this might be format issue");
                                }
                            }

                            // Only restart if it's an actual error (not normal completion)
                            // and we're not already in a restart cycle
                            static TickType_t last_restart = 0;
                            TickType_t now = xTaskGetTickCount();
                            if (!pipeline_restarting && (now - last_restart) > pdMS_TO_TICKS(5000) && status != AEL_STATUS_STATE_FINISHED)
                            { // Don't restart on normal completion
                                ESP_LOGI("AUDIO_PLAYER", "Attempting pipeline restart after cooldown...");
                                last_restart = now;
                                pipeline_restarting = true;

                                // Simple restart approach
                                audio_pipeline_stop(global_pipeline);
                                audio_pipeline_wait_for_stop(global_pipeline);
                                audio_pipeline_reset_ringbuffer(global_pipeline);
                                audio_pipeline_reset_elements(global_pipeline);

                                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause
                                audio_pipeline_run(global_pipeline);
                                pipeline_restarting = false;
                            }
                            else
                            {
                                if (status == AEL_STATUS_STATE_FINISHED)
                                {
                                    ESP_LOGI("AUDIO_PLAYER", "File completed - no restart needed");
                                }
                                else
                                {
                                    ESP_LOGW("AUDIO_PLAYER", "Restart suppressed - %s",
                                             pipeline_restarting ? "already restarting" : "too soon after last restart");
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                // No pipeline running, wait a bit before checking again
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
    else
    {
        ESP_LOGE("main", "❌ WiFi connection failed");
    }
}