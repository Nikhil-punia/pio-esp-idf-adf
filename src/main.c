/* Play an MP3, AAC or WAV file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
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
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"

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

// SD Card support
#include "fatfs_stream.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include <dirent.h>
#include <sys/stat.h>

// SD Card SPI support
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

// JSON support for API
#include "cJSON.h"

// TCP Socket support for high-speed file uploads
#ifdef CONFIG_LWIP_IPV4
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#endif

// Custom memory allocation for cJSON to use PSRAM for large allocations
static void* cjson_malloc_psram(size_t size) {
    if (size > 32768) { // Use PSRAM for allocations > 32KB
        void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (ptr) {
            ESP_LOGI("CJSON", "Allocated %d bytes in PSRAM", size);
            return ptr;
        }
        ESP_LOGW("CJSON", "PSRAM allocation failed, falling back to internal RAM");
    }
    return malloc(size);
}

static void cjson_free_psram(void* ptr) {
    free(ptr); // free() works for both internal RAM and PSRAM
}

// MIN macro if not defined
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

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

// SD Card SPI pins for ESP32-S3-N8R8 DevKit
#define ESP_SD_PIN_CLK   GPIO_NUM_12  // SPI CLK
#define ESP_SD_PIN_CMD   GPIO_NUM_11  // SPI MOSI
#define ESP_SD_PIN_D0    GPIO_NUM_13  // SPI MISO
#define ESP_SD_PIN_D3    GPIO_NUM_10  // SPI CS

// Ensure all SD pin definitions exist (some may be referenced even in SPI mode)
#ifndef ESP_SD_PIN_D1
#define ESP_SD_PIN_D1    GPIO_NUM_NC
#endif
#ifndef ESP_SD_PIN_D2
#define ESP_SD_PIN_D2    GPIO_NUM_NC
#endif
#ifndef ESP_SD_PIN_CD
#define ESP_SD_PIN_CD    GPIO_NUM_NC
#endif
#ifndef ESP_SD_PIN_WP
#define ESP_SD_PIN_WP    GPIO_NUM_NC
#endif

// TCP File Upload Server Configuration
#define TCP_UPLOAD_PORT     8080
#define TCP_MAX_CONNECTIONS 2
#define TCP_UPLOAD_BUFFER_SIZE (256 * 1024)  // 256KB buffer for fast transfers
#define TCP_UPLOAD_HEADER_SIZE 1024

// TCP Upload Protocol Header
typedef struct {
    uint32_t magic;           // Magic number 0xCAFEBABE
    uint32_t file_size;       // Total file size
    uint32_t chunk_size;      // Size of this chunk
    uint32_t chunk_index;     // Chunk index (0-based)
    uint32_t filename_len;    // Length of filename
    char filename[256];       // Filename (null-terminated)
    uint32_t crc32;          // CRC32 checksum of chunk data
    uint8_t reserved[244];   // Reserved for future use (total header = 1024 bytes)
} __attribute__((packed)) tcp_upload_header_t;

#define TCP_UPLOAD_MAGIC 0xCAFEBABE

// TCP Upload Session
typedef struct {
    int socket;
    FILE* file;
    char filepath[512];
    uint32_t total_size;
    uint32_t received_size;
    uint32_t expected_chunks;
    uint32_t received_chunks;
    bool active;
    TickType_t last_activity;
} tcp_upload_session_t;

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

// SD Card support variables
// static esp_periph_set_handle_t set = NULL; // Currently unused - commented out to avoid warning
static audio_element_handle_t fatfs_stream_reader = NULL;
static char current_sd_file[256] = "";
static bool is_playing_from_sd = false;
static bool volume_change_requested = false;

// WebSocket connections management
static int websocket_fd = -1;
static bool websocket_connected = false;
static SemaphoreHandle_t websocket_mutex = NULL;

// File upload session management
typedef struct {
    char session_id[32];
    char filename[128];
    char filepath[256];
    size_t file_size;
    size_t uploaded_bytes;
    FILE* file_handle;
    bool active;
    uint32_t last_activity;
} upload_session_t;

static upload_session_t upload_session = {0};
static SemaphoreHandle_t upload_mutex = NULL;

// TCP File Upload Server
static int tcp_server_socket = -1;
static TaskHandle_t tcp_server_task_handle = NULL;
static tcp_upload_session_t tcp_upload_sessions[TCP_MAX_CONNECTIONS] = {0};
static SemaphoreHandle_t tcp_upload_mutex = NULL;

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

// Forward declaration for SD card functions
static void start_audio_pipeline_from_sd(const char *file_path);
static esp_err_t get_sd_files_json(char *json_buffer, size_t buffer_size);

// Forward declaration for WebSocket functions
static void broadcast_message(const char *message);

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
             "\"free_psram_kb\":%zu,"
             "\"playing_from_sd\":%s,"
             "\"current_sd_file\":\"%.100s\""
             "}"
             "}",
             safe_url,
             current_decoder_name,
             current_volume,
             global_pipeline ? "true" : "false",
             http_filled / 1024,
             decoder_filled / 1024,
             free_heap / 1024,
             free_psram / 1024,
             is_playing_from_sd ? "true" : "false",
             current_sd_file);

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

// WebSocket message broadcasting (generic message)
static void broadcast_message(const char *message)
{
    if (!message || !websocket_mutex) {
        return;
    }

    if (xSemaphoreTake(websocket_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!websocket_connected || websocket_fd < 0) {
            xSemaphoreGive(websocket_mutex);
            return;
        }

        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t *)message;
        ws_pkt.len = strlen(message);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        esp_err_t ret = httpd_ws_send_frame_async(server, websocket_fd, &ws_pkt);
        if (ret != ESP_OK)
        {
            ESP_LOGW("WEBSOCKET", "Failed to send message: %s", esp_err_to_name(ret));
            // If send fails, disconnect the WebSocket
            websocket_connected = false;
            websocket_fd = -1;
        }
        xSemaphoreGive(websocket_mutex);
    }
}

// Upload session management functions
static void init_upload_session() {
    if (!upload_mutex) {
        upload_mutex = xSemaphoreCreateMutex();
    }
    if (!websocket_mutex) {
        websocket_mutex = xSemaphoreCreateMutex();
    }
}

static bool start_upload_session(const char* session_id, const char* filename, size_t file_size) {
    if (xSemaphoreTake(upload_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // Clean up any existing session
        if (upload_session.active && upload_session.file_handle) {
            fclose(upload_session.file_handle);
        }
        
        // Initialize new session
        memset(&upload_session, 0, sizeof(upload_session));
        strncpy(upload_session.session_id, session_id, sizeof(upload_session.session_id) - 1);
        strncpy(upload_session.filename, filename, sizeof(upload_session.filename) - 1);
        snprintf(upload_session.filepath, sizeof(upload_session.filepath), "/sdcard/%s", filename);
        upload_session.file_size = file_size;
        upload_session.uploaded_bytes = 0;
        upload_session.active = true;
        upload_session.last_activity = xTaskGetTickCount();
        
        // File handle will be managed by binary upload chunks
        upload_session.file_handle = NULL;
        
        ESP_LOGI("UPLOAD", "Started upload session: %s, file: %s, size: %zu bytes", 
                session_id, filename, file_size);
        
        xSemaphoreGive(upload_mutex);
        return true;
    }
    return false;
}

static bool write_upload_chunk(const char* session_id, const uint8_t* data, size_t data_size) {
    if (xSemaphoreTake(upload_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (!upload_session.active || strcmp(upload_session.session_id, session_id) != 0) {
            xSemaphoreGive(upload_mutex);
            return false;
        }
        
        if (!upload_session.file_handle) {
            xSemaphoreGive(upload_mutex);
            return false;
        }
        
        size_t written = fwrite(data, 1, data_size, upload_session.file_handle);
        if (written != data_size) {
            ESP_LOGE("UPLOAD", "Failed to write chunk: expected %zu, written %zu", data_size, written);
            xSemaphoreGive(upload_mutex);
            return false;
        }
        
        upload_session.uploaded_bytes += written;
        upload_session.last_activity = xTaskGetTickCount();
        
        // Flush every 64KB or if near completion
        if ((upload_session.uploaded_bytes % (64 * 1024)) == 0 || 
            upload_session.uploaded_bytes >= upload_session.file_size) {
            fflush(upload_session.file_handle);
        }
        
        xSemaphoreGive(upload_mutex);
        return true;
    }
    return false;
}

static bool complete_upload_session(const char* session_id) {
    ESP_LOGI("UPLOAD", "DEBUG: complete_upload_session called with session_id: %s", session_id ? session_id : "NULL");
    
    if (xSemaphoreTake(upload_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ESP_LOGI("UPLOAD", "DEBUG: Mutex acquired, session active: %s, stored session_id: %s", 
                 upload_session.active ? "true" : "false", 
                 upload_session.session_id ? upload_session.session_id : "NULL");
        
        if (!upload_session.active || strcmp(upload_session.session_id, session_id) != 0) {
            ESP_LOGE("UPLOAD", "DEBUG: Session mismatch or inactive. Active: %s, ID match: %s", 
                     upload_session.active ? "true" : "false",
                     (upload_session.session_id && strcmp(upload_session.session_id, session_id) == 0) ? "true" : "false");
            xSemaphoreGive(upload_mutex);
            return false;
        }
        
        // File handle is managed by binary upload chunks, not by session
        if (upload_session.file_handle) {
            fflush(upload_session.file_handle);
            fclose(upload_session.file_handle);
            upload_session.file_handle = NULL;
        }
        
        // Get actual file size from disk for final verification
        struct stat file_stat;
        size_t actual_file_size = 0;
        if (stat(upload_session.filepath, &file_stat) == 0) {
            actual_file_size = file_stat.st_size;
            ESP_LOGI("UPLOAD", "DEBUG: File stat successful: filepath=%s, tracked=%zu, disk=%zu, expected=%zu", 
                     upload_session.filepath, upload_session.uploaded_bytes, actual_file_size, upload_session.file_size);
            upload_session.uploaded_bytes = actual_file_size; // Update to actual size
        } else {
            actual_file_size = upload_session.uploaded_bytes; // Use tracked size if stat fails
            ESP_LOGI("UPLOAD", "DEBUG: File stat failed for %s, using tracked size: %zu, expected=%zu", 
                     upload_session.filepath, actual_file_size, upload_session.file_size);
        }
        
        bool success = (actual_file_size == upload_session.file_size);
        if (success) {
            ESP_LOGI("UPLOAD", "Upload completed successfully: %s (%zu bytes)", 
                    upload_session.filename, actual_file_size);
        } else {
            ESP_LOGE("UPLOAD", "Upload incomplete: %s (%zu/%zu bytes), difference: %d", 
                    upload_session.filename, actual_file_size, upload_session.file_size,
                    (int)(upload_session.file_size - actual_file_size));
            // Delete incomplete file
            unlink(upload_session.filepath);
        }
        
        upload_session.active = false;
        xSemaphoreGive(upload_mutex);
        return success;
    }
    return false;
}

// Binary upload protocol structure
typedef struct {
    uint32_t session_id;
    uint32_t chunk_index;
    uint32_t chunk_size;
    uint32_t total_size;
    char filename[64];
    // Data follows immediately after this header
} binary_upload_header_t;

// Handle binary upload data (much faster than JSON+base64)
static void handle_binary_upload(httpd_req_t *req, uint8_t *data, size_t len)
{
    if (len < sizeof(binary_upload_header_t)) {
        ESP_LOGE("WEBSOCKET", "Binary upload packet too small: %d bytes", len);
        return;
    }

    binary_upload_header_t *header = (binary_upload_header_t *)data;
    uint8_t *chunk_data = data + sizeof(binary_upload_header_t);
    size_t data_len = len - sizeof(binary_upload_header_t);

    // Log more frequently for debugging, especially for last chunks
    if (header->chunk_index % 5 == 0 || data_len != header->chunk_size || header->chunk_index > 15) {
        ESP_LOGI("WEBSOCKET", "Binary upload: session_id=%" PRIu32 ", chunk_index=%" PRIu32 ", expected_size=%" PRIu32 ", actual_size=%d", 
                 header->session_id, header->chunk_index, header->chunk_size, data_len);
    }

    // Use actual data length - but warn if there's a significant mismatch
    size_t actual_chunk_size = data_len;
    
    if (data_len != header->chunk_size) {
        ESP_LOGW("WEBSOCKET", "⚠️ CHUNK SIZE MISMATCH: chunk %" PRIu32 " expected %u bytes, got %zu bytes (diff: %d)", 
                 header->chunk_index, header->chunk_size, data_len, (int)(header->chunk_size - data_len));
    }
    
    if (actual_chunk_size == 0) {
        ESP_LOGW("WEBSOCKET", "Received empty chunk for session %" PRIu32 ", index %" PRIu32, 
                 header->session_id, header->chunk_index);
        return;
    }

    // Write chunk directly to file and update upload session
    char file_path[100];
    snprintf(file_path, sizeof(file_path), "/sdcard/%s", header->filename);
    
    // Convert session_id to string for tracking
    char session_id_str[16];
    snprintf(session_id_str, sizeof(session_id_str), "%" PRIu32, header->session_id);
    
    if (header->chunk_index == 0) {
        // First chunk - create new file and start tracking session
        FILE *f = fopen(file_path, "wb");
        if (f) {
            size_t written = fwrite(chunk_data, 1, actual_chunk_size, f);
            fflush(f); // Ensure data is written to disk
            fclose(f);
            
            if (written == actual_chunk_size) {
                ESP_LOGI("WEBSOCKET", "Created file %s with first chunk (%zu bytes)", header->filename, written);
                
                // Start upload session tracking
                if (!start_upload_session(session_id_str, header->filename, header->total_size)) {
                    ESP_LOGE("WEBSOCKET", "Failed to start upload session tracking");
                } else {
                    // Update uploaded bytes manually since we're bypassing add_upload_chunk
                    if (xSemaphoreTake(upload_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        upload_session.uploaded_bytes = written;
                        xSemaphoreGive(upload_mutex);
                    }
                }
            } else {
                ESP_LOGE("WEBSOCKET", "Failed to write first chunk: expected %zu, written %zu", actual_chunk_size, written);
                return;
            }
        } else {
            ESP_LOGE("WEBSOCKET", "Failed to create file %s", header->filename);
            return;
        }
    } else {
        // Append chunk to existing file and update tracking
        FILE *f = fopen(file_path, "ab");
        if (f) {
            size_t written = fwrite(chunk_data, 1, actual_chunk_size, f);
            fflush(f); // Ensure data is written to disk
            fclose(f);
            
            if (written == actual_chunk_size) {
                // Update uploaded bytes tracking with simple increment
                if (xSemaphoreTake(upload_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    upload_session.uploaded_bytes += written;
                    upload_session.last_activity = xTaskGetTickCount();
                    
                    size_t current_uploaded = upload_session.uploaded_bytes;
                    size_t total_size = upload_session.file_size;
                    size_t remaining = total_size - current_uploaded;
                    
                    ESP_LOGI("WEBSOCKET", "Chunk %" PRIu32 " written %zu bytes, total now: %zu/%zu, remaining: %zu", 
                            header->chunk_index, written, current_uploaded, total_size, remaining);
                    
                    // Special logging for potentially final chunks
                    if (remaining == 0) {
                        ESP_LOGI("WEBSOCKET", "🎉 FINAL CHUNK COMPLETED! Chunk %" PRIu32 " - Upload should be complete!", header->chunk_index);
                    } else if (remaining < 65536) {
                        ESP_LOGI("WEBSOCKET", "⚠️ NEAR COMPLETION: Chunk %" PRIu32 " - Only %zu bytes remaining", header->chunk_index, remaining);
                    }
                    
                    xSemaphoreGive(upload_mutex);
                    
                    // Log progress every 64KB (outside mutex to reduce contention)
                    if ((current_uploaded % (64 * 1024)) == 0) {
                        ESP_LOGI("WEBSOCKET", "Upload progress: %s (%zu/%zu bytes = %.1f%%)", 
                                header->filename, current_uploaded, total_size, 
                                (float)current_uploaded / total_size * 100.0f);
                    }
                } else {
                    ESP_LOGE("WEBSOCKET", "Failed to acquire mutex for tracking update");
                }
                
                // Only log every 10th chunk to reduce overhead
                if (header->chunk_index % 10 == 0) {
                    ESP_LOGI("WEBSOCKET", "Appended chunk %" PRIu32 " to %s (%zu bytes)", header->chunk_index, header->filename, written);
                }
            } else {
                ESP_LOGE("WEBSOCKET", "Failed to write chunk: expected %zu, written %zu", actual_chunk_size, written);
                return;
            }
        } else {
            ESP_LOGE("WEBSOCKET", "Failed to append to file %s", header->filename);
            return;
        }
    }

    // Send binary acknowledgment (much smaller than JSON)
    uint8_t ack_data[16];
    uint32_t *ack_session = (uint32_t *)&ack_data[0];
    uint32_t *ack_chunk = (uint32_t *)&ack_data[4];
    uint32_t *ack_status = (uint32_t *)&ack_data[8];
    uint32_t *ack_size = (uint32_t *)&ack_data[12];
    
    *ack_session = header->session_id;
    *ack_chunk = header->chunk_index;
    *ack_status = 1; // Success
    *ack_size = actual_chunk_size;
    
    // Log this specific chunk for debugging
    ESP_LOGI("WEBSOCKET", "Sending ack for chunk %" PRIu32 " (size: %zu bytes)", header->chunk_index, actual_chunk_size);
    
    // Send binary ack immediately
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = ack_data;
    ws_pkt.len = 16;
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;
    
    esp_err_t ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE("WEBSOCKET", "Failed to send binary ack for chunk %" PRIu32 ": %s", header->chunk_index, esp_err_to_name(ret));
    }
    
    // Yield to other tasks to prevent WebSocket from becoming unresponsive
    if (header->chunk_index % 5 == 0) {
        vTaskDelay(pdMS_TO_TICKS(1)); // Small yield every 5 chunks
    }
}

// Handle incoming WebSocket messages (lightweight version)
static void handle_websocket_message(const char *message)
{
    ESP_LOGI("WEBSOCKET", "Received message type and length: %.50s... (total length: %d)", message, strlen(message)); // Limit log length

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
    else if (strstr(message, "\"type\":\"play_sd_file\"") && strstr(message, "\"file_path\":\""))
    {
        char *file_start = strstr(message, "\"file_path\":\"");
        if (file_start)
        {
            file_start += 13; // Skip "file_path":"
            char *file_end = strchr(file_start, '"');
            if (file_end && (file_end - file_start) < sizeof(current_sd_file) - 1)
            {
                if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    char file_path[256];
                    strncpy(file_path, file_start, file_end - file_start);
                    file_path[file_end - file_start] = 0;
                    
                    // Start playing SD file
                    start_audio_pipeline_from_sd(file_path);
                    xSemaphoreGive(stream_control_mutex);
                    ESP_LOGI("WEBSOCKET", "Playing SD file: %s", file_path);
                }
            }
        }
    }
    else if (strstr(message, "\"type\":\"get_status\""))
    {
        // Send current status immediately
        broadcast_status_update();
    }
    else if (strstr(message, "\"type\":\"get_sd_files\""))
    {
        // Send SD card files list via WebSocket
        char *json_buffer = malloc(8192);
        if (json_buffer)
        {
            if (get_sd_files_json(json_buffer, 8192) == ESP_OK)
            {
                // Parse the JSON to send it as a WebSocket message
                cJSON *json = cJSON_Parse(json_buffer);
                if (json)
                {
                    cJSON *response = cJSON_CreateObject();
                    cJSON *type = cJSON_CreateString("sd_files_loaded");
                    cJSON *data = cJSON_CreateObject();
                    cJSON *files = cJSON_GetObjectItem(json, "files");
                    
                    if (files)
                    {
                        cJSON_AddItemToObject(response, "type", type);
                        cJSON_AddItemToObject(data, "files", cJSON_Duplicate(files, 1));
                        cJSON_AddItemToObject(response, "data", data);
                        
                        char *ws_response = cJSON_PrintUnformatted(response);
                        if (ws_response)
                        {
                            broadcast_message(ws_response);
                            free(ws_response);
                        }
                    }
                    cJSON_Delete(response);
                    cJSON_Delete(json);
                }
            }
            free(json_buffer);
        }
    }
    else if (strstr(message, "\"type\":\"upload_start\""))
    {
        // Handle upload session start
        cJSON *json = cJSON_Parse(message);
        if (json) {
            cJSON *data = cJSON_GetObjectItem(json, "data");
            if (data) {
                cJSON *session_id = cJSON_GetObjectItem(data, "session_id");
                cJSON *filename = cJSON_GetObjectItem(data, "filename");
                cJSON *file_size = cJSON_GetObjectItem(data, "file_size");
                
                if (session_id && filename && file_size && 
                    cJSON_IsString(session_id) && cJSON_IsString(filename) && cJSON_IsNumber(file_size)) {
                    
                    bool success = start_upload_session(session_id->valuestring, 
                                                      filename->valuestring, 
                                                      (size_t)file_size->valuedouble);
                    
                    // Send response
                    char response[256];
                    if (success) {
                        snprintf(response, sizeof(response),
                            "{\"type\":\"upload_ready\",\"data\":{\"session_id\":\"%s\",\"chunk_size\":65536,\"binary_mode\":true}}",
                            session_id->valuestring);
                        ESP_LOGI("WEBSOCKET", "Upload session ready: %s", filename->valuestring);
                    } else {
                        snprintf(response, sizeof(response),
                            "{\"type\":\"upload_error\",\"data\":{\"session_id\":\"%s\",\"error\":\"Failed to start upload session\"}}",
                            session_id->valuestring);
                        ESP_LOGE("WEBSOCKET", "Failed to start upload session: %s", filename->valuestring);
                    }
                    broadcast_message(response);
                }
            }
            cJSON_Delete(json);
        }
    }
    else if (strstr(message, "\"type\":\"upload_chunk\""))
    {
        ESP_LOGI("WEBSOCKET", "Processing upload_chunk message, length: %d", strlen(message));
        
        // Use PSRAM for large JSON parsing if message is large
        cJSON *json = NULL;
        bool use_psram = strlen(message) > 100000; // Use PSRAM for messages > 100KB
        
        if (use_psram) {
            ESP_LOGI("WEBSOCKET", "Using PSRAM for large JSON parsing");
            // For large messages, use PSRAM and increase parsing limits
            json = cJSON_ParseWithLength(message, strlen(message));
        } else {
            json = cJSON_Parse(message);
        }
        
        if (json) {
            ESP_LOGI("WEBSOCKET", "JSON parsed successfully");
            cJSON *data = cJSON_GetObjectItem(json, "data");
            if (data) {
                ESP_LOGI("WEBSOCKET", "Data object found");
                cJSON *session_id = cJSON_GetObjectItem(data, "session_id");
                cJSON *chunk_data = cJSON_GetObjectItem(data, "chunk_data");
                cJSON *chunk_size = cJSON_GetObjectItem(data, "chunk_size");
                
                if (session_id && chunk_data && chunk_size &&
                    cJSON_IsString(session_id) && cJSON_IsString(chunk_data) && cJSON_IsNumber(chunk_size)) {
                    
                    ESP_LOGI("WEBSOCKET", "Upload chunk: session=%s, chunk_size=%d, base64_len=%d", 
                            session_id->valuestring, (int)chunk_size->valuedouble, strlen(chunk_data->valuestring));
                    
                    // Debug: check first and last few characters of base64 data
                    const char *base64_str = chunk_data->valuestring;
                    size_t base64_len = strlen(base64_str);
                    ESP_LOGI("WEBSOCKET", "Base64 data preview - first 20 chars: %.20s", base64_str);
                    ESP_LOGI("WEBSOCKET", "Base64 data preview - last 20 chars: %s", base64_str + (base64_len > 20 ? base64_len - 20 : 0));
                    
                    // Decode base64 data - use PSRAM for large chunks (up to 1MB)
                    size_t decoded_len = 0;
                    size_t buffer_size = (size_t)chunk_size->valuedouble + 4;
                    
                    // Use PSRAM allocation for chunks larger than 32KB
                    uint8_t *decoded_data = NULL;
                    if (buffer_size > 32768) {
                        // Allocate from PSRAM for large chunks
                        decoded_data = (uint8_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
                        if (!decoded_data) {
                            ESP_LOGW("WEBSOCKET", "PSRAM allocation failed, trying regular heap");
                            decoded_data = (uint8_t*)malloc(buffer_size);
                        }
                    } else {
                        decoded_data = (uint8_t*)malloc(buffer_size);
                    }
                    
                    if (decoded_data) {
                        ESP_LOGI("WEBSOCKET", "Allocated %zu bytes for decoding", buffer_size);
                        
                        // Validate base64 string before decoding
                        const char *base64_str = chunk_data->valuestring;
                        size_t input_len = strlen(base64_str);
                        bool valid_base64 = true;
                        
                        ESP_LOGI("WEBSOCKET", "Received base64 string length: %zu", input_len);
                        ESP_LOGI("WEBSOCKET", "First 50 chars: %.50s", base64_str);
                        if (input_len > 50) {
                            ESP_LOGI("WEBSOCKET", "Last 50 chars: %s", base64_str + input_len - 50);
                        }
                        
                        // Check for valid base64 characters
                        for (size_t i = 0; i < input_len; i++) {
                            char c = base64_str[i];
                            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                                  (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                                ESP_LOGE("WEBSOCKET", "Invalid base64 character at position %zu: 0x%02X ('%c')", i, (unsigned char)c, c);
                                valid_base64 = false;
                                break;
                            }
                        }
                        
                        if (!valid_base64) {
                            ESP_LOGE("WEBSOCKET", "Invalid base64 data detected");
                            char response[256];
                            snprintf(response, sizeof(response),
                                "{\"type\":\"upload_error\",\"data\":{\"session_id\":\"%s\",\"error\":\"Invalid base64 data\"}}",
                                session_id->valuestring);
                            broadcast_message(response);
                            free(decoded_data);
                            cJSON_Delete(json);
                            return;
                        }
                        
                        // Log base64 data for debugging
                        ESP_LOGI("WEBSOCKET", "Base64 string length: %zu, expected decoded size: %d", 
                                input_len, (int)chunk_size->valuedouble);
                        
                        // Log first and last 20 characters of base64 data
                        if (input_len > 40) {
                            char first_chars[21] = {0};
                            char last_chars[21] = {0};
                            strncpy(first_chars, base64_str, 20);
                            strncpy(last_chars, base64_str + input_len - 20, 20);
                            ESP_LOGI("WEBSOCKET", "Base64 starts: '%s' ends: '%s'", first_chars, last_chars);
                        }
                        
                        // Check for invalid characters
                        int invalid_count = 0;
                        for (size_t i = 0; i < input_len && invalid_count < 10; i++) {
                            char c = base64_str[i];
                            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                                  (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                                ESP_LOGW("WEBSOCKET", "Invalid base64 char at pos %zu: '%c' (0x%02x)", i, c, (unsigned char)c);
                                invalid_count++;
                            }
                        }
                        if (invalid_count > 0) {
                            ESP_LOGE("WEBSOCKET", "Found %d+ invalid base64 characters", invalid_count);
                        }
                        
                        // Use mbedtls base64 decode
                        int ret = mbedtls_base64_decode(decoded_data, buffer_size, 
                                                       &decoded_len, (const unsigned char*)base64_str, input_len);
                        
                        ESP_LOGI("WEBSOCKET", "Base64 decode result: ret=%d, decoded_len=%zu, expected=%d", 
                                ret, decoded_len, (int)chunk_size->valuedouble);
                        
                        if (ret == 0 && decoded_len == (size_t)chunk_size->valuedouble) {
                            ESP_LOGI("WEBSOCKET", "Base64 decode successful, writing chunk");
                            bool success = write_upload_chunk(session_id->valuestring, decoded_data, decoded_len);
                            
                            // Send response
                            char response[256];
                            if (success) {
                                snprintf(response, sizeof(response),
                                    "{\"type\":\"upload_chunk_received\",\"data\":{\"session_id\":\"%s\",\"chunk_size\":%d}}",
                                    session_id->valuestring, (int)decoded_len);
                                ESP_LOGI("WEBSOCKET", "Chunk written successfully, sending confirmation");
                            } else {
                                snprintf(response, sizeof(response),
                                    "{\"type\":\"upload_error\",\"data\":{\"session_id\":\"%s\",\"error\":\"Failed to write chunk\"}}",
                                    session_id->valuestring);
                                ESP_LOGE("WEBSOCKET", "Failed to write chunk");
                            }
                            broadcast_message(response);
                        } else {
                            ESP_LOGE("WEBSOCKET", "Base64 decode failed: expected %d, got %zu, ret=%d", 
                                    (int)chunk_size->valuedouble, decoded_len, ret);
                            char response[256];
                            snprintf(response, sizeof(response),
                                "{\"type\":\"upload_error\",\"data\":{\"session_id\":\"%s\",\"error\":\"Base64 decode failed\"}}",
                                session_id->valuestring);
                            broadcast_message(response);
                        }
                        free(decoded_data);
                    } else {
                        ESP_LOGE("WEBSOCKET", "Failed to allocate %zu bytes for upload chunk", buffer_size);
                        char response[256];
                        snprintf(response, sizeof(response),
                            "{\"type\":\"upload_error\",\"data\":{\"session_id\":\"%s\",\"error\":\"Memory allocation failed\"}}",
                            session_id->valuestring);
                        broadcast_message(response);
                    }
                } else {
                    ESP_LOGE("WEBSOCKET", "Invalid upload_chunk message format");
                }
            } else {
                ESP_LOGE("WEBSOCKET", "No data object in upload_chunk message");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE("WEBSOCKET", "Failed to parse JSON for upload_chunk");
        }
    }    
    else if (strstr(message, "\"type\":\"upload_complete\""))
    {
        // Handle upload completion with safety checks
        ESP_LOGI("WEBSOCKET", "Processing upload_complete message");
        
        cJSON *json = cJSON_Parse(message);
        if (json) {
            cJSON *data = cJSON_GetObjectItem(json, "data");
            if (data) {
                cJSON *session_id = cJSON_GetObjectItem(data, "session_id");
                
                if (session_id && cJSON_IsString(session_id)) {
                    ESP_LOGI("WEBSOCKET", "Completing upload session: %s", session_id->valuestring);
                    
                    bool success = false;
                    
                    // Add a longer delay to ensure all binary chunks are processed
                    ESP_LOGI("WEBSOCKET", "Waiting for all chunks to complete...");
                    vTaskDelay(pdMS_TO_TICKS(1000)); // Increased to 1 second
                    
                    // Force file sync to ensure all data is written
                    if (xSemaphoreTake(upload_mutex, pdMS_TO_TICKS(1000))) {
                        if (upload_session.file_handle) {
                            fflush(upload_session.file_handle);
                            fsync(fileno(upload_session.file_handle));
                        }
                        xSemaphoreGive(upload_mutex);
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(100)); // Additional delay after sync
                    
                    success = complete_upload_session(session_id->valuestring);
                    
                    // Send response with additional safety checks
                    char response[256];
                    if (success) {
                        snprintf(response, sizeof(response),
                            "{\"type\":\"upload_complete\",\"data\":{\"session_id\":\"%s\",\"success\":true}}",
                            session_id->valuestring);
                        ESP_LOGI("WEBSOCKET", "Upload completed successfully: %s", session_id->valuestring);
                    } else {
                        snprintf(response, sizeof(response),
                            "{\"type\":\"upload_complete\",\"data\":{\"session_id\":\"%s\",\"success\":false,\"error\":\"Upload verification failed\"}}",
                            session_id->valuestring);
                        ESP_LOGE("WEBSOCKET", "Upload completion failed: %s", session_id->valuestring);
                    }
                    
                    // Add delay before sending response to prevent race conditions
                    vTaskDelay(pdMS_TO_TICKS(50));
                    broadcast_message(response);
                } else {
                    ESP_LOGE("WEBSOCKET", "Invalid session_id in upload_complete message");
                }
            } else {
                ESP_LOGE("WEBSOCKET", "No data object in upload_complete message");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE("WEBSOCKET", "Failed to parse JSON for upload_complete");
        }
    }
}

// Get list of audio files from SD card
static esp_err_t get_sd_files_json(char *json_buffer, size_t buffer_size)
{
    DIR *dir;
    struct dirent *entry;
    struct stat file_stat;
    char full_path[300];
    
    ESP_LOGI("SDCARD", "🗂️ Starting SD card file enumeration...");
    
    if (!json_buffer || buffer_size < 100) {
        ESP_LOGE("SDCARD", "Invalid arguments to get_sd_files_json");
        return ESP_ERR_INVALID_ARG;
    }
    
    strcpy(json_buffer, "{\"files\":[");
    size_t current_len = strlen(json_buffer);
    bool first_file = true;
    
    ESP_LOGI("SDCARD", "📁 Attempting to open /sdcard directory...");
    dir = opendir("/sdcard");
    if (dir == NULL) {
        ESP_LOGE("SDCARD", "❌ Failed to open /sdcard directory");
        strcat(json_buffer, "]}");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI("SDCARD", "✅ Successfully opened /sdcard directory");
    int file_count = 0;
    int audio_file_count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        file_count++;
        ESP_LOGI("SDCARD", "📄 Found file: %s", entry->d_name);
        
        // Skip directories and hidden files
        if (entry->d_name[0] == '.') {
            ESP_LOGI("SDCARD", "⏭️ Skipping hidden file: %s", entry->d_name);
            continue;
        }
        
        // Create full path
        snprintf(full_path, sizeof(full_path), "/sdcard/%s", entry->d_name);
        
        // Get file stats
        if (stat(full_path, &file_stat) != 0) {
            ESP_LOGW("SDCARD", "⚠️ Could not stat file: %s", full_path);
            continue;
        }
        
        // Skip if it's a directory
        if (S_ISDIR(file_stat.st_mode)) {
            ESP_LOGI("SDCARD", "📁 Skipping directory: %s", entry->d_name);
            continue;
        }
        
        ESP_LOGI("SDCARD", "📄 File size: %ld bytes", file_stat.st_size);
        
        // Check if it's an audio file
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) {
            ESP_LOGI("SDCARD", "⏭️ No extension found for: %s", entry->d_name);
            continue;
        }
        ext++;
        
        ESP_LOGI("SDCARD", "🔍 File extension: %s", ext);
        
        bool is_audio = false;
        if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 || 
            strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "fla") == 0 ||  // Handle 8.3 filename: .flac -> .fla
            strcasecmp(ext, "aac") == 0 || strcasecmp(ext, "m4a") == 0 ||
            strcasecmp(ext, "ogg") == 0) {
            is_audio = true;
            ESP_LOGI("SDCARD", "🎵 Detected audio file: %s", entry->d_name);
        }
        
        if (!is_audio) {
            ESP_LOGI("SDCARD", "⏭️ Not an audio file: %s", entry->d_name);
            continue;
        }
        
        audio_file_count++;
        
        // Add comma separator if not first file
        if (!first_file) {
            if (current_len + 1 >= buffer_size - 50) break; // Reserve space for closing
            strcat(json_buffer, ",");
            current_len++;
        }
        
        // Add file entry with larger buffer to prevent truncation
        char file_entry[800]; // Increased buffer size to handle long paths
        
        // Create a display name - for now, just use the original name to avoid crashes
        char display_name[256];
        strncpy(display_name, entry->d_name, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = 0;
        
        // Simple transformation: if it has ~, just show it's truncated
        if (strstr(display_name, "~")) {
            // Just add a note that it's truncated, don't manipulate the string
            size_t len = strlen(display_name);
            if (len < sizeof(display_name) - 12) {  // Reserve space for " (truncated)"
                strcat(display_name, " (8.3)");
            }
        }
        
        int entry_len = snprintf(file_entry, sizeof(file_entry), 
                "{\"name\": \"%s\", \"display_name\": \"%s\", \"size\": %ld, \"path\": \"%s\", \"type\": \"audio\"}", 
                entry->d_name, display_name, file_stat.st_size, full_path);
        
        // Check if the entry was truncated or if adding it would overflow the main buffer
        if (entry_len >= sizeof(file_entry) || current_len + entry_len >= buffer_size - 10) {
            ESP_LOGW("SDCARD", "Skipping file entry due to size limits: %s", entry->d_name);
            break; // Skip this entry to prevent overflow
        }
        
        strcat(json_buffer, file_entry);
        current_len += entry_len;
        first_file = false;
        ESP_LOGI("SDCARD", "✅ Added audio file to JSON: %s", entry->d_name);
    }
    
    closedir(dir);
    strcat(json_buffer, "]}");
    
    ESP_LOGI("SDCARD", "📊 File enumeration complete:");
    ESP_LOGI("SDCARD", "   Total files found: %d", file_count);
    ESP_LOGI("SDCARD", "   Audio files found: %d", audio_file_count);
    ESP_LOGI("SDCARD", "   JSON response length: %d", strlen(json_buffer));
    
    return ESP_OK;
}

// HTTP handler for SD card file list
static esp_err_t sd_files_handler(httpd_req_t *req)
{
    char *json_buffer = malloc(4096); // 4KB buffer for file list
    if (!json_buffer) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = get_sd_files_json(json_buffer, 4096);
    if (ret != ESP_OK) {
        free(json_buffer);
        const char *error_json = "{\"error\": \"Failed to read SD card\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_json, strlen(error_json));
        return ESP_OK;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buffer, strlen(json_buffer));
    free(json_buffer);
    return ESP_OK;
}

// HTTP handler for playing SD card file
static esp_err_t play_sd_file_handler(httpd_req_t *req)
{
    char content[256];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = 0;
    
    // Parse JSON to get file path
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        const char *error_resp = "{\"error\": \"Invalid JSON\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_resp, strlen(error_resp));
        return ESP_OK;
    }
    
    cJSON *file_path = cJSON_GetObjectItem(json, "file_path");
    if (!cJSON_IsString(file_path)) {
        cJSON_Delete(json);
        const char *error_resp = "{\"error\": \"Missing file_path\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_resp, strlen(error_resp));
        return ESP_OK;
    }
    
    // Start playing the file
    if (xSemaphoreTake(stream_control_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        start_audio_pipeline_from_sd(file_path->valuestring);
        xSemaphoreGive(stream_control_mutex);
        
        ESP_LOGI("HTTP_SERVER", "Starting playback of SD file: %s", file_path->valuestring);
        
        const char *success_resp = "{\"status\": \"success\", \"message\": \"Started SD playback\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, success_resp, strlen(success_resp));
    } else {
        const char *error_resp = "{\"error\": \"System busy\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_resp, strlen(error_resp));
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

// TCP File Upload Server Functions

// Find available TCP upload session
static tcp_upload_session_t* find_available_tcp_session(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!tcp_upload_sessions[i].active) {
            return &tcp_upload_sessions[i];
        }
    }
    return NULL;
}

// Initialize TCP upload session
static bool init_tcp_upload_session(tcp_upload_session_t* session, int socket, const tcp_upload_header_t* header) {
    if (!session || !header) return false;
    
    // Validate magic number
    if (header->magic != TCP_UPLOAD_MAGIC) {
        ESP_LOGE("TCP_UPLOAD", "Invalid magic number: 0x%08lx", (unsigned long)header->magic);
        return false;
    }
    
    // Debug: Log header contents
    ESP_LOGI("TCP_UPLOAD", "Header received: magic=0x%08lx, file_size=%" PRIu32 ", chunk_size=%" PRIu32 ", filename_len=%" PRIu32 ", filename='%s'", 
             (unsigned long)header->magic, header->file_size, header->chunk_size, header->filename_len, header->filename);
    
    // Validate filename
    if (header->filename_len == 0 || header->filename[0] == '\0') {
        ESP_LOGE("TCP_UPLOAD", "Empty or invalid filename received");
        return false;
    }
    
    // Create filepath
    snprintf(session->filepath, sizeof(session->filepath), "/sdcard/%s", header->filename);
    
    // Open file for writing
    session->file = fopen(session->filepath, "wb");
    if (!session->file) {
        ESP_LOGE("TCP_UPLOAD", "Failed to create file: %s", session->filepath);
        return false;
    }
    
    // Initialize session
    session->socket = socket;
    session->total_size = header->file_size;
    session->received_size = 0;
    session->expected_chunks = (header->file_size + header->chunk_size - 1) / header->chunk_size;
    session->received_chunks = 0;
    session->active = true;
    session->last_activity = xTaskGetTickCount();
    
    ESP_LOGI("TCP_UPLOAD", "Session initialized: file=%s, size=%" PRIu32 ", chunks=%" PRIu32, 
             header->filename, header->file_size, session->expected_chunks);
    
    return true;
}

// Process TCP upload chunk
static bool process_tcp_upload_chunk(tcp_upload_session_t* session, const tcp_upload_header_t* header, const uint8_t* data) {
    if (!session || !header || !data || !session->file) return false;
    
    // Write chunk to file
    size_t written = fwrite(data, 1, header->chunk_size, session->file);
    if (written != header->chunk_size) {
        ESP_LOGE("TCP_UPLOAD", "Write failed: wrote %d, expected %" PRIu32, written, header->chunk_size);
        return false;
    }
    
    // Update session stats
    session->received_size += header->chunk_size;
    session->received_chunks++;
    session->last_activity = xTaskGetTickCount();
    
    // Log progress every 100 chunks
    if (session->received_chunks % 100 == 0 || session->received_chunks == session->expected_chunks) {
        float progress = (float)session->received_size / session->total_size * 100.0f;
        ESP_LOGI("TCP_UPLOAD", "Progress: %.1f%% (%" PRIu32 "/%" PRIu32 " chunks, %" PRIu32 "/%" PRIu32 " bytes)", 
                 progress, session->received_chunks, session->expected_chunks,
                 session->received_size, session->total_size);
    }
    
    // Check if upload is complete
    if (session->received_chunks >= session->expected_chunks) {
        fclose(session->file);
        session->file = NULL;
        session->active = false;
        ESP_LOGI("TCP_UPLOAD", "Upload completed: %s (%" PRIu32 " bytes)", session->filepath, session->received_size);
        return true;
    }
    
    return true;
}

// Handle TCP client connection
static void handle_tcp_client(void* pvParameters) {
    int client_sock = (int)(intptr_t)pvParameters;
    tcp_upload_session_t* session = NULL;
    tcp_upload_header_t header;
    uint8_t* buffer = NULL;
    
    ESP_LOGI("TCP_UPLOAD", "New client connected, socket: %d", client_sock);
    
    // Allocate buffer for data transfer
    buffer = heap_caps_malloc(TCP_UPLOAD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        buffer = malloc(TCP_UPLOAD_BUFFER_SIZE);
        if (!buffer) {
            ESP_LOGE("TCP_UPLOAD", "Failed to allocate buffer");
            close(client_sock);
            return;
        }
    }
    
    while (1) {
        // Read header
        int received = recv(client_sock, &header, sizeof(tcp_upload_header_t), MSG_WAITALL);
        if (received <= 0) {
            if (received == 0) {
                ESP_LOGI("TCP_UPLOAD", "Client disconnected");
            } else {
                ESP_LOGE("TCP_UPLOAD", "Header receive error: %d", errno);
            }
            break;
        }
        
        if (received != sizeof(tcp_upload_header_t)) {
            ESP_LOGE("TCP_UPLOAD", "Incomplete header received: %d/%d bytes", received, sizeof(tcp_upload_header_t));
            break;
        }
        
        // Validate header
        if (header.magic != TCP_UPLOAD_MAGIC) {
            ESP_LOGE("TCP_UPLOAD", "Invalid magic: 0x%" PRIx32, header.magic);
            break;
        }
        
        if (header.chunk_size > TCP_UPLOAD_BUFFER_SIZE) {
            ESP_LOGE("TCP_UPLOAD", "Chunk too large: %" PRIu32 " > %u", header.chunk_size, TCP_UPLOAD_BUFFER_SIZE);
            break;
        }
        
        // Initialize session for first chunk
        if (header.chunk_index == 0) {
            if (session) {
                // Clean up previous session
                if (session->file) fclose(session->file);
                session->active = false;
            }
            
            session = find_available_tcp_session();
            if (!session) {
                ESP_LOGE("TCP_UPLOAD", "No available upload sessions");
                break;
            }
            
            if (!init_tcp_upload_session(session, client_sock, &header)) {
                ESP_LOGE("TCP_UPLOAD", "Failed to initialize upload session");
                break;
            }
        }
        
        if (!session || !session->active) {
            ESP_LOGE("TCP_UPLOAD", "No active session for chunk %" PRIu32, header.chunk_index);
            break;
        }
        
        // Read chunk data
        uint32_t remaining = header.chunk_size;
        uint32_t total_received = 0;
        
        while (remaining > 0) {
            uint32_t to_read = MIN(remaining, TCP_UPLOAD_BUFFER_SIZE);
            int chunk_received = recv(client_sock, buffer + total_received, to_read, MSG_WAITALL);
            
            if (chunk_received <= 0) {
                ESP_LOGE("TCP_UPLOAD", "Data receive error: %d", errno);
                goto cleanup;
            }
            
            total_received += chunk_received;
            remaining -= chunk_received;
        }
        
        // Process chunk
        if (!process_tcp_upload_chunk(session, &header, buffer)) {
            ESP_LOGE("TCP_UPLOAD", "Failed to process chunk %" PRIu32, header.chunk_index);
            break;
        }
        
        // Send acknowledgment
        uint32_t ack = header.chunk_index;
        if (send(client_sock, &ack, sizeof(ack), 0) != sizeof(ack)) {
            ESP_LOGE("TCP_UPLOAD", "Failed to send acknowledgment");
            break;
        }
        
        // Check if upload is complete
        if (!session->active) {
            ESP_LOGI("TCP_UPLOAD", "Upload completed successfully");
            break;
        }
    }
    
cleanup:
    if (session) {
        if (session->file) {
            fclose(session->file);
            session->file = NULL;
        }
        session->active = false;
    }
    
    if (buffer) free(buffer);
    close(client_sock);
    ESP_LOGI("TCP_UPLOAD", "Client connection closed");
}

// TCP Upload Server Task
static void tcp_upload_server_task(void* pvParameters) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_sock;
    
    ESP_LOGI("TCP_UPLOAD", "Starting TCP upload server on port %d", TCP_UPLOAD_PORT);
    
    // Create socket
    tcp_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_server_socket < 0) {
        ESP_LOGE("TCP_UPLOAD", "Failed to create socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(tcp_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Set receive buffer size
    int recv_buf_size = TCP_UPLOAD_BUFFER_SIZE;
    setsockopt(tcp_server_socket, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size));
    
    // Bind socket
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TCP_UPLOAD_PORT);
    
    if (bind(tcp_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE("TCP_UPLOAD", "Failed to bind socket: %d", errno);
        close(tcp_server_socket);
        vTaskDelete(NULL);
        return;
    }
    
    // Listen for connections
    if (listen(tcp_server_socket, TCP_MAX_CONNECTIONS) < 0) {
        ESP_LOGE("TCP_UPLOAD", "Failed to listen: %d", errno);
        close(tcp_server_socket);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("TCP_UPLOAD", "TCP upload server listening on port %d", TCP_UPLOAD_PORT);
    
    while (1) {
        // Accept client connection
        client_sock = accept(tcp_server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            ESP_LOGE("TCP_UPLOAD", "Accept failed: %d", errno);
            continue;
        }
        
        // Create task to handle client
        char task_name[32];
        snprintf(task_name, sizeof(task_name), "tcp_client_%d", client_sock);
        
        xTaskCreatePinnedToCore(
            handle_tcp_client,
            task_name,
            8192 * 2,  // 16KB stack
            (void*)(intptr_t)client_sock,
            5,  // Priority
            NULL,
            1   // Core 1
        );
    }
    
    close(tcp_server_socket);
    vTaskDelete(NULL);
}

// Start TCP Upload Server
static esp_err_t start_tcp_upload_server(void) {
    // Create mutex
    tcp_upload_mutex = xSemaphoreCreateMutex();
    if (!tcp_upload_mutex) {
        ESP_LOGE("TCP_UPLOAD", "Failed to create mutex");
        return ESP_FAIL;
    }
    
    // Initialize sessions
    memset(tcp_upload_sessions, 0, sizeof(tcp_upload_sessions));
    
    // Create server task
    BaseType_t result = xTaskCreatePinnedToCore(
        tcp_upload_server_task,
        "tcp_upload_server",
        8192 * 3,  // 24KB stack
        NULL,
        6,  // Priority
        &tcp_server_task_handle,
        1   // Core 1
    );
    
    if (result != pdPASS) {
        ESP_LOGE("TCP_UPLOAD", "Failed to create server task");
        vSemaphoreDelete(tcp_upload_mutex);
        return ESP_FAIL;
    }
    
    ESP_LOGI("TCP_UPLOAD", "TCP upload server started successfully");
    return ESP_OK;
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

        // Store WebSocket connection info with proper synchronization
        if (websocket_mutex && xSemaphoreTake(websocket_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            websocket_fd = httpd_req_to_sockfd(req);
            websocket_connected = true;
            xSemaphoreGive(websocket_mutex);
        } else {
            ESP_LOGE("WEBSOCKET", "Failed to acquire websocket mutex");
            free(buf);
            return ESP_ERR_TIMEOUT;
        }

        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
        {
            // Handle the message
            handle_websocket_message((char *)ws_pkt.payload);
        }
        else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY)
        {
            // Handle binary upload data (faster than JSON+base64)
            handle_binary_upload(req, ws_pkt.payload, ws_pkt.len);
        }
        else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE)
        {
            ESP_LOGI("WEBSOCKET", "WebSocket connection closed");
            if (websocket_mutex && xSemaphoreTake(websocket_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                websocket_connected = false;
                websocket_fd = -1;
                xSemaphoreGive(websocket_mutex);
            }
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
        ".sd-file-item{padding:12px;border-bottom:1px solid #eee;display:flex;justify-content:space-between;align-items:center;}"
        ".sd-file-item:hover{background:#f8f9fa;}"
        ".sd-file-info{flex:1;}"
        ".sd-file-name{font-weight:bold;color:#333;margin-bottom:4px;}"
        ".sd-file-details{font-size:12px;color:#666;}"
        ".sd-file-actions{display:flex;gap:8px;}"
        ".sd-file-btn{padding:6px 12px;font-size:12px;border:none;border-radius:3px;cursor:pointer;}"
        ".sd-play-btn{background:#28a745;color:white;}"
        ".sd-play-btn:hover{background:#218838;}"
        ".sd-info-btn{background:#17a2b8;color:white;}"
        ".sd-info-btn:hover{background:#138496;}"
        ".sd-delete-btn{background:#dc3545;color:white;}"
        ".sd-delete-btn:hover{background:#c82333;}"
        ".currently-playing{background:#e8f5e8;border-left:4px solid #28a745;}"
        ".upload-progress{background:#e8f5e8;color:#155724;border:1px solid #c3e6cb;}"
        ".upload-error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;}"
        ".upload-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}"
        "@keyframes progress-stripes{0%{background-position:0 0;}100%{background-position:20px 0;}}"
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
        ""
        "<div class='equalizer-control' style='margin-top:30px;'>"
        "<h3>🗂️ SD Card File Browser</h3>"
        "<div style='margin:15px 0;'>"
        "<button type='button' onclick='loadSDFiles()' id='loadSDBtn'>📁 Browse SD Card Files</button> "
        "<button type='button' onclick='refreshSDFiles()' id='refreshSDBtn' style='display:none;'>🔄 Refresh</button>"
        "</div>"
        "<div style='margin:15px 0; border-top:1px solid #ddd; padding-top:15px;'>"
        "<h4>📤 Upload Audio File to SD Card</h4>"
        "<div style='margin:10px 0;'>"
        "<input type='file' id='fileUpload' accept='.mp3,.flac,.wav,.aac,.ogg,.amr' style='margin-right:10px;'/>"
        "<button type='button' onclick='uploadFileTCP()' id='uploadBtnTCP'>⚡ Upload via TCP (Fastest)</button>"
        "<button type='button' onclick='uploadFileWS()' id='uploadBtn' style='margin-left:10px;'>🚀 Upload via WebSocket</button>"
        "<button type='button' onclick='uploadFile()' id='uploadBtnHTTP' style='margin-left:10px;'>📤 Upload via HTTP</button>"
        "</div>"
        "<div id='uploadStatus' style='margin:10px 0; padding:8px; border-radius:4px; display:none;'></div>"
        "<div id='uploadProgress' style='margin:10px 0; display:none;'>"
        "<div style='background:#e9ecef; border-radius:8px; overflow:hidden; border:1px solid #dee2e6;'>"
        "<div id='uploadProgressBar' style='background:linear-gradient(90deg, #28a745, #20c997); height:24px; width:0%; transition:width 0.2s ease-out; position:relative; overflow:hidden;'>"
        "<div style='position:absolute; top:0; left:0; right:0; bottom:0; background:linear-gradient(45deg, rgba(255,255,255,0.1) 25%, transparent 25%, transparent 50%, rgba(255,255,255,0.1) 50%, rgba(255,255,255,0.1) 75%, transparent 75%, transparent); background-size:20px 20px; animation:progress-stripes 1s linear infinite;'></div>"
        "</div>"
        "</div>"
        "<div id='uploadProgressText' style='text-align:center; margin-top:8px; font-size:13px; font-weight:500; color:#495057;'></div>"
        "</div>"
        "</div>"
        "<div id='sdCardStatus' class='status info' style='display:none;'></div>"
        "<div id='sdFileList' style='display:none;'>"
        "<div style='max-height:300px;overflow-y:auto;border:1px solid #ddd;border-radius:5px;'>"
        "<div id='fileListContainer'></div>"
        "</div>"
        "</div>"
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
        "      if (event.data instanceof ArrayBuffer || event.data instanceof Blob) {\n"
        "        console.log('Received binary upload ack');\n"
        "        handleBinaryUploadAck(event.data);\n"
        "        return;\n"
        "      }\n"
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
        "    const messageStr = JSON.stringify(message);\n"
        "    console.log('Sending WebSocket message:', type, 'size:', messageStr.length, 'bytes');\n"
        "    try {\n"
        "      ws.send(messageStr);\n"
        "    } catch (error) {\n"
        "      console.error('Error sending WebSocket message:', error);\n"
        "    }\n"
        "  } else {\n"
        "    console.warn('WebSocket not connected, cannot send message');\n"
        "  }\n"
        "}\n"
        "\n"
        "function handleBinaryUploadAck(data) {\n"
        "  console.log('Processing binary upload acknowledgment');\n"
        "  \n"
        "  // Parse binary ack: session_id (4 bytes) + chunk_index (4 bytes) + status (4 bytes)\n"
        "  const reader = new FileReader();\n"
        "  reader.onload = function() {\n"
        "    const buffer = reader.result;\n"
        "    const view = new DataView(buffer);\n"
        "    \n"
        "    const sessionId = view.getUint32(0, true);\n"
        "    const chunkIndex = view.getUint32(4, true);\n"
        "    const status = view.getUint32(8, true);\n"
        "    \n"
        "    console.log('Binary ack: session', sessionId, 'chunk', chunkIndex, 'status', status);\n"
        "    console.log('Current upload session:', currentUpload ? currentUpload.sessionId : 'none');\n"
        "    \n"
        "    if (currentUpload && sessionId.toString() === currentUpload.sessionId) {\n"
        "      console.log('Session ID matches, processing ack...');\n"
        "      if (status === 1) {\n"
        "        // Success - mark chunk as complete\n"
        "        if (!currentUpload.completedChunks) currentUpload.completedChunks = new Set();\n"
        "        currentUpload.completedChunks.add(chunkIndex);\n"
        "        const chunkSize = Math.min(uploadChunkSize, currentUpload.fileSize - (chunkIndex * uploadChunkSize));\n"
        "        currentUpload.uploadedBytes += chunkSize;\n"
        "        console.log('Chunk', chunkIndex, 'completed. Total uploaded:', currentUpload.uploadedBytes, 'of', currentUpload.fileSize);\n"
        "        showUploadProgress(currentUpload.uploadedBytes, currentUpload.fileSize);\n"
        "        \n"
        "        // Continue with parallel uploads\n"
        "        uploadNextParallelChunks();\n"
        "      } else {\n"
        "        console.error('Upload chunk failed:', chunkIndex);\n"
        "        showUploadStatus('Upload failed at chunk ' + chunkIndex, 'error');\n"
        "        resetUpload();\n"
        "      }\n"
        "    } else {\n"
        "      console.log('Session ID mismatch or no current upload');\n"
        "    }\n"
        "  };\n"
        "  \n"
        "  if (data instanceof Blob) {\n"
        "    reader.readAsArrayBuffer(data);\n"
        "  } else {\n"
        "    reader.readAsArrayBuffer(new Blob([data]));\n"
        "  }\n"
        "}\n"
        "\n"
        "function handleWebSocketMessage(message) {\n"
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
        "  else if (message.type === 'sd_files_loaded' && message.data && message.data.files) {\n"
        "    displaySDFiles(message.data.files);\n"
        "    document.getElementById('sdCardStatus').innerHTML = '✅ Found ' + message.data.files.length + ' audio files';\n"
        "    document.getElementById('loadSDBtn').style.display = 'none';\n"
        "    document.getElementById('refreshSDBtn').style.display = 'inline-block';\n"
        "  }\n"
        "  else if (message.type === 'sd_file_playing' && message.data) {\n"
        "    updateCurrentlyPlaying(message.data.file_path);\n"
        "    document.getElementById('currentStream').textContent = 'SD: ' + (message.data.file_name || 'Unknown');\n"
        "  }\n"
        "  else if (message.type === 'upload_ready' && message.data) {\n"
        "    if (currentUpload && currentUpload.sessionId === message.data.session_id) {\n"
        "        "      // Update chunk size from server\n"
        "      if (message.data.chunk_size) {\n"
        "        uploadChunkSize = message.data.chunk_size;\n"
        "        console.log('Updated upload chunk size to:', uploadChunkSize);\n"
        "      }\n"
        "      window.uploadStartTime = Date.now(); // Reset timer for progress calculation\n"
        "      showUploadStatus('Starting upload: ' + currentUpload.fileName, 'progress');\n"
        "      \n"
        "      // Initialize parallel upload tracking\n"
        "      currentUpload.totalChunks = Math.ceil(currentUpload.fileSize / uploadChunkSize);\n"
        "      currentUpload.sentChunks = new Set();\n"
        "      currentUpload.completedChunks = new Set();\n"
        "      currentUpload.uploadedBytes = 0;\n"
        "      \n"
        "      // Start parallel upload with initial batch\n"
        "      uploadNextParallelChunks();\n"
        "    }\n"
        "  }\n"
        "  else if (message.type === 'upload_chunk_received' && message.data) {\n"
        "    if (currentUpload && currentUpload.sessionId === message.data.session_id) {\n"
        "      currentUpload.uploadedBytes += message.data.chunk_size;\n"
        "      \n"
        "      // Update progress on every chunk for real-time feedback\n"
        "      showUploadProgress(currentUpload.uploadedBytes, currentUpload.fileSize);\n"
        "      \n"
        "      if (currentUpload.uploadedBytes < currentUpload.fileSize) {\n"
        "        // Upload next chunk immediately for maximum speed\n"
        "        uploadNextChunk();\n"
        "      } else {\n"
        "        // Upload complete, send finalize message\n"
        "        sendWebSocketMessage('upload_complete', {\n"
        "          session_id: currentUpload.sessionId\n"
        "        });\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  else if (message.type === 'upload_complete' && message.data) {\n"
        "    if (currentUpload && currentUpload.sessionId === message.data.session_id) {\n"
        "      if (message.data.success) {\n"
        "        showUploadStatus('✅ Upload completed successfully: ' + currentUpload.fileName, 'success');\n"
        "        document.getElementById('fileUpload').value = '';\n"
        "        if (document.getElementById('sdFileList').style.display !== 'none') {\n"
        "          setTimeout(() => refreshSDFiles(), 1000);\n"
        "        }\n"
        "      } else {\n"
        "        showUploadStatus('❌ Upload failed: ' + (message.data.error || 'Unknown error'), 'error');\n"
        "      }\n"
        "      resetUpload();\n"
        "    }\n"
        "  }\n"
        "  else if (message.type === 'upload_error' && message.data) {\n"
        "    if (currentUpload && currentUpload.sessionId === message.data.session_id) {\n"
        "      showUploadStatus('❌ Upload error: ' + (message.data.error || 'Unknown error'), 'error');\n"
        "      resetUpload();\n"
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
        "  const controls = ['volume', 'setStreamBtn', 'resetEqBtn', 'ledManual', 'loadSDBtn', 'refreshSDBtn', 'uploadBtnTCP', 'uploadBtn', 'uploadBtnHTTP'];\n"
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
        "  document.querySelectorAll('.sd-file-btn').forEach(btn => {\n"
        "    btn.disabled = !enabled;\n"
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
        "// SD Card functionality\n"
        "function loadSDFiles() {\n"
        "  document.getElementById('sdCardStatus').style.display = 'block';\n"
        "  document.getElementById('sdCardStatus').innerHTML = '🔄 Loading SD card files...';\n"
        "  \n"
        "  // First try WebSocket for faster loading\n"
        "  if (wsConnected) {\n"
        "    sendWebSocketMessage('get_sd_files', {});\n"
        "  } else {\n"
        "    // Fallback to HTTP if WebSocket not connected\n"
        "    fetch('/sd_files')\n"
        "      .then(response => response.json())\n"
        "      .then(data => {\n"
        "        if (data && data.files) {\n"
        "          displaySDFiles(data.files);\n"
        "          document.getElementById('sdCardStatus').innerHTML = '✅ Found ' + data.files.length + ' audio files';\n"
        "          document.getElementById('loadSDBtn').style.display = 'none';\n"
        "          document.getElementById('refreshSDBtn').style.display = 'inline-block';\n"
        "        } else {\n"
        "          document.getElementById('sdCardStatus').innerHTML = '❌ No files found or SD card error';\n"
        "        }\n"
        "      })\n"
        "      .catch(error => {\n"
        "        console.error('Error loading SD files:', error);\n"
        "        document.getElementById('sdCardStatus').innerHTML = '❌ Error loading SD card files';\n"
        "      });\n"
        "  }\n"
        "}\n"
        "\n"
        "function refreshSDFiles() {\n"
        "  loadSDFiles();\n"
        "}\n"
        "\n"
        "function displaySDFiles(files) {\n"
        "  const container = document.getElementById('fileListContainer');\n"
        "  const fileList = document.getElementById('sdFileList');\n"
        "  \n"
        "  container.innerHTML = '';\n"
        "  \n"
        "  if (files.length === 0) {\n"
        "    container.innerHTML = '<div style=\"padding:20px;text-align:center;color:#666;\">No audio files found</div>';\n"
        "    fileList.style.display = 'block';\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  files.forEach(file => {\n"
        "    const fileItem = document.createElement('div');\n"
        "    fileItem.className = 'sd-file-item';\n"
        "    fileItem.dataset.filePath = file.path;\n"
        "    \n"
        "    const fileInfo = document.createElement('div');\n"
        "    fileInfo.className = 'sd-file-info';\n"
        "    \n"
        "    const fileName = document.createElement('div');\n"
        "    fileName.className = 'sd-file-name';\n"
        "    fileName.textContent = file.display_name || file.name;\n"
        "    \n"
        "    const fileDetails = document.createElement('div');\n"
        "    fileDetails.className = 'sd-file-details';\n"
        "    fileDetails.textContent = formatFileSize(file.size) + ' • ' + (file.type || 'Unknown');\n"
        "    \n"
        "    fileInfo.appendChild(fileName);\n"
        "    fileInfo.appendChild(fileDetails);\n"
        "    \n"
        "    const actions = document.createElement('div');\n"
        "    actions.className = 'sd-file-actions';\n"
        "    \n"
        "    const playBtn = document.createElement('button');\n"
        "    playBtn.className = 'sd-file-btn sd-play-btn';\n"
        "    playBtn.textContent = '▶️ Play';\n"
        "    playBtn.onclick = () => playSDFile(file.path, file.name);\n"
        "    \n"
        "    const infoBtn = document.createElement('button');\n"
        "    infoBtn.className = 'sd-file-btn sd-info-btn';\n"
        "    infoBtn.textContent = 'ℹ️ Info';\n"
        "    infoBtn.onclick = () => showFileInfo(file);\n"
        "    \n"
        "    const deleteBtn = document.createElement('button');\n"
        "    deleteBtn.className = 'sd-file-btn sd-delete-btn';\n"
        "    deleteBtn.textContent = '🗑️ Delete';\n"
        "    deleteBtn.onclick = () => deleteFile(file.path, file.name);\n"
        "    \n"
        "    actions.appendChild(playBtn);\n"
        "    actions.appendChild(infoBtn);\n"
        "    actions.appendChild(deleteBtn);\n"
        "    \n"
        "    fileItem.appendChild(fileInfo);\n"
        "    fileItem.appendChild(actions);\n"
        "    \n"
        "    container.appendChild(fileItem);\n"
        "  });\n"
        "  \n"
        "  fileList.style.display = 'block';\n"
        "}\n"
        "\n"
        "function playSDFile(filePath, fileName) {\n"
        "  console.log('Playing SD file:', filePath);\n"
        "  sendWebSocketMessage('play_sd_file', { file_path: filePath });\n"
        "  document.getElementById('currentStream').textContent = 'SD: ' + fileName;\n"
        "  updateCurrentlyPlaying(filePath);\n"
        "}\n"
        "\n"
        "function updateCurrentlyPlaying(currentPath) {\n"
        "  document.querySelectorAll('.sd-file-item').forEach(item => {\n"
        "    item.classList.remove('currently-playing');\n"
        "    if (item.dataset.filePath === currentPath) {\n"
        "      item.classList.add('currently-playing');\n"
        "    }\n"
        "  });\n"
        "}\n"
        "\n"
        "function showFileInfo(file) {\n"
        "  const info = [\n"
        "    'File: ' + (file.display_name || file.name),\n"
        "    'Size: ' + formatFileSize(file.size),\n"
        "    'Type: ' + (file.type || 'Unknown'),\n"
        "    'Path: ' + file.path\n"
        "  ].join('\\n');\n"
        "  \n"
        "  alert(info);\n"
        "}\n"
        "\n"
        "function deleteFile(filePath, fileName) {\n"
        "  if (confirm('Are you sure you want to delete \"' + fileName + '\"?')) {\n"
        "    fetch('/delete_file', {\n"
        "      method: 'POST',\n"
        "      headers: {\n"
        "        'Content-Type': 'application/json'\n"
        "      },\n"
        "      body: JSON.stringify({ file_path: filePath })\n"
        "    })\n"
        "    .then(response => response.json())\n"
        "    .then(data => {\n"
        "      if (data.success) {\n"
        "        alert('File deleted successfully!');\n"
        "        refreshSDFiles();\n"
        "      } else {\n"
        "        alert('Failed to delete file: ' + (data.error || 'Unknown error'));\n"
        "      }\n"
        "    })\n"
        "    .catch(error => {\n"
        "      console.error('Delete error:', error);\n"
        "      alert('Failed to delete file: Network error');\n"
        "    });\n"
        "  }\n"
        "}\n"
        "\n"
        "function formatFileSize(bytes) {\n"
        "  if (bytes === 0) return '0 B';\n"
        "  const k = 1024;\n"
        "  const sizes = ['B', 'KB', 'MB', 'GB'];\n"
        "  const i = Math.floor(Math.log(bytes) / Math.log(k));\n"
        "  return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];\n"
        "}\n"
        "\n"
        "// File upload functionality\n"
        "let uploadChunkSize = 64 * 1024; // 64KB chunks with increased WebSocket buffer\n"
        "let fallbackChunkSize = 32 * 1024; // 32KB fallback chunks if large chunks fail\n"
        "let wsUploadTimeout = 30000; // 30 second timeout for large chunks\n"
        "let currentUpload = null;\n"
        "let activeUploads = new Map(); // Track multiple concurrent chunk uploads\n"
        "const maxConcurrentChunks = 6; // Number of parallel uploads\n"
        "let pendingChunks = []; // Queue of chunks waiting to be uploaded\n"
        "\n"
        "function uploadFileWS() {\n"
        "  const fileInput = document.getElementById('fileUpload');\n"
        "  const uploadStatus = document.getElementById('uploadStatus');\n"
        "  const uploadBtn = document.getElementById('uploadBtn');\n"
        "  const uploadBtnHTTP = document.getElementById('uploadBtnHTTP');\n"
        "  \n"
        "  if (!wsConnected) {\n"
        "    showUploadStatus('WebSocket not connected. Please wait for connection.', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  if (!fileInput.files || fileInput.files.length === 0) {\n"
        "    showUploadStatus('Please select a file to upload', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  const file = fileInput.files[0];\n"
        "  const maxSize = 200 * 1024 * 1024; // 200MB limit for WebSocket\n"
        "  \n"
        "  if (file.size > maxSize) {\n"
        "    showUploadStatus('File too large. Maximum size is 200MB', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  uploadBtn.disabled = true;\n"
        "  uploadBtnHTTP.disabled = true;\n"
        "  showUploadStatus('Preparing upload: ' + file.name + ' (' + formatFileSize(file.size) + ')', 'progress');\n"
        "  showUploadProgress(0, file.size);\n"
        "  \n"
        "  // Initialize upload session\n"
        "  currentUpload = {\n"
        "    file: file,\n"
        "    fileName: file.name,\n"
        "    fileSize: file.size,\n"
        "    uploadedBytes: 0,\n"
        "    sessionId: Math.floor(Math.random() * 0xFFFFFFFF).toString(),\n"
        "    reader: new FileReader()\n"
        "  };\n"
        "  \n"
        "  // Start upload session\n"
        "  sendWebSocketMessage('upload_start', {\n"
        "    session_id: currentUpload.sessionId,\n"
        "    filename: currentUpload.fileName,\n"
        "    file_size: currentUpload.fileSize,\n"
        "    chunk_size: uploadChunkSize\n"
        "  });\n"
        "}\n"
        "\n"
        "function uploadNextParallelChunks() {\n"
        "  if (!currentUpload || !wsConnected) {\n"
        "    showUploadStatus('Upload interrupted - connection lost', 'error');\n"
        "    resetUpload();\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  // Check if upload is complete\n"
        "  if (currentUpload.completedChunks && currentUpload.completedChunks.size >= currentUpload.totalChunks) {\n"
        "    console.log('All chunks completed, sending completion message');\n"
        "    sendWebSocketMessage('upload_complete', {\n"
        "      session_id: currentUpload.sessionId,\n"
        "      filename: currentUpload.fileName\n"
        "    });\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  // Send multiple chunks in parallel (up to 8 simultaneous for 128KB chunks)\n"
        "  const maxParallel = 8;\n"
        "  let sent = 0;\n"
        "  \n"
        "  for (let chunkIndex = 0; chunkIndex < currentUpload.totalChunks && sent < maxParallel; chunkIndex++) {\n"
        "    // Skip chunks already sent or completed\n"
        "    if (currentUpload.sentChunks.has(chunkIndex) || \n"
        "        (currentUpload.completedChunks && currentUpload.completedChunks.has(chunkIndex))) {\n"
        "      continue;\n"
        "    }\n"
        "    \n"
        "    // Send this chunk\n"
        "    uploadChunkByIndex(chunkIndex);\n"
        "    currentUpload.sentChunks.add(chunkIndex);\n"
        "    sent++;\n"
        "  }\n"
        "  \n"
        "  console.log('Sent', sent, 'parallel chunks');\n"
        "}\n"
        "\n"
        "function uploadChunkByIndex(chunkIndex) {\n"
        "  const startByte = chunkIndex * uploadChunkSize;\n"
        "  const endByte = Math.min(startByte + uploadChunkSize, currentUpload.fileSize);\n"
        "  const chunkSize = endByte - startByte;\n"
        "  const chunk = currentUpload.file.slice(startByte, endByte);\n"
        "  \n"
        "  console.log('Uploading chunk', chunkIndex, ':', startByte, 'to', endByte, 'size:', chunkSize);\n"
        "  \n"
        "  const reader = new FileReader();\n"
        "  reader.onload = function(e) {\n"
        "    try {\n"
        "      const arrayBuffer = e.target.result;\n"
        "      const uint8Array = new Uint8Array(arrayBuffer);\n"
        "      \n"
        "      // Create binary packet\n"
        "      const filenameBytes = new TextEncoder().encode(currentUpload.fileName);\n"
        "      const headerSize = 80;\n"
        "      const header = new ArrayBuffer(headerSize);\n"
        "      const headerView = new DataView(header);\n"
        "      headerView.setUint32(0, parseInt(currentUpload.sessionId), true);\n"
        "      headerView.setUint32(4, chunkIndex, true);\n"
        "      headerView.setUint32(8, uint8Array.length, true); // Use actual chunk data length\n"
        "      headerView.setUint32(12, currentUpload.fileSize, true);\n"
        "      const headerBytes = new Uint8Array(header);\n"
        "      \n"
        "      // Write filename\n"
        "      const maxFilenameLen = Math.min(filenameBytes.length, 63);\n"
        "      headerBytes.set(filenameBytes.subarray(0, maxFilenameLen), 16);\n"
        "      headerBytes[16 + maxFilenameLen] = 0;\n"
        "      \n"
        "      // Create and send packet\n"
        "      const packet = new Uint8Array(headerSize + uint8Array.length);\n"
        "      packet.set(headerBytes, 0);\n"
        "      packet.set(uint8Array, headerSize);\n"
        "      \n"
        "      console.log('Sending chunk', chunkIndex, 'size:', packet.length);\n"
        "      ws.send(packet.buffer);\n"
        "    } catch (error) {\n"
        "      console.error('Error processing chunk', chunkIndex, ':', error);\n"
        "    }\n"
        "  };\n"
        "  \n"
        "  reader.readAsArrayBuffer(chunk);\n"
        "}\n"
        "\n"
        "function uploadNextChunk() {\n"
        "  console.log('uploadNextChunk called, currentUpload:', currentUpload, 'wsConnected:', wsConnected);\n"
        "  \n"
        "  if (!currentUpload || !wsConnected) {\n"
        "    showUploadStatus('Upload interrupted - connection lost', 'error');\n"
        "    resetUpload();\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  if (currentUpload.uploadedBytes >= currentUpload.fileSize) {\n"
        "    console.log('Upload complete, sending completion message');\n"
        "    sendWebSocketMessage('upload_complete', {\n"
        "      session_id: currentUpload.sessionId,\n"
        "      filename: currentUpload.fileName\n"
        "    });\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  const startByte = currentUpload.uploadedBytes;\n"
        "  const endByte = Math.min(startByte + uploadChunkSize, currentUpload.fileSize);\n"
        "  const chunk = currentUpload.file.slice(startByte, endByte);\n"
        "  \n"
        "  console.log('Processing chunk:', startByte, 'to', endByte, 'size:', endByte - startByte);\n"
        "  \n"
        "  currentUpload.reader.onload = function(e) {\n"
        "    try {\n"
        "      console.log('FileReader onload called');\n"
        "      const arrayBuffer = e.target.result;\n"
        "      const uint8Array = new Uint8Array(arrayBuffer);\n"
        "      \n"
        "      console.log('Converting', uint8Array.length, 'bytes to BINARY (no base64)');\n"
        "      \n"
        "      // Use binary protocol instead of base64\n"
        "      const chunkIndex = Math.floor(startByte / uploadChunkSize);\n"
        "      console.log('Creating binary packet for chunk', chunkIndex);\n"
        "      \n"
        "      console.log('Binary packet created, size:', uint8Array.length);\n"
        "      \n"
        "      const message = {\n"
        "        session_id: currentUpload.sessionId,\n"
        "        chunk_index: Math.floor(startByte / uploadChunkSize),\n"
        "        chunk_data: 'binary',\n"
        "        chunk_size: uint8Array.length\n"
        "      };\n"
        "      \n"
        "      console.log('Sending upload_chunk message');\n"
        "      // Send binary data directly (no JSON)\n"
        "      const filenameBytes = new TextEncoder().encode(currentUpload.fileName);\n"
        "      const headerSize = 80; // Fixed size: 4 uint32_t + 64-byte filename\n"
        "      const header = new ArrayBuffer(headerSize);\n"
        "      const headerView = new DataView(header);\n"
        "      headerView.setUint32(0, parseInt(currentUpload.sessionId), true);\n"
        "      console.log('Sending session ID:', currentUpload.sessionId, 'as uint32:', parseInt(currentUpload.sessionId));\n"
        "      headerView.setUint32(4, chunkIndex, true);\n"
        "      headerView.setUint32(8, uint8Array.length, true);\n"
        "      headerView.setUint32(12, currentUpload.fileSize, true); // total_size\n"
        "      const headerBytes = new Uint8Array(header);\n"
        "      \n"
        "      // Write filename to fixed 64-byte field\n"
        "      const maxFilenameLen = Math.min(filenameBytes.length, 63);\n"
        "      headerBytes.set(filenameBytes.subarray(0, maxFilenameLen), 16);\n"
        "      headerBytes[16 + maxFilenameLen] = 0;\n"
        "      const packet = new Uint8Array(headerSize + uint8Array.length);\n"
        "      packet.set(headerBytes, 0);\n"
        "      packet.set(uint8Array, headerSize);\n"
        "      console.log('Packet created: header', headerSize, 'data', uint8Array.length, 'total', packet.length);\n"
        "      console.log('Sending packet buffer length:', packet.buffer.byteLength);\n"
        "      ws.send(packet.buffer);\n"
        "    } catch (error) {\n"
        "      console.error('Error processing upload chunk:', error);\n"
        "      showUploadStatus('Error processing file chunk: ' + error.message, 'error');\n"
        "      resetUpload();\n"
        "    }\n"
        "  };\n"
        "  \n"
        "  currentUpload.reader.onerror = function(error) {\n"
        "    console.error('FileReader error:', error);\n"
        "    showUploadStatus('Error reading file chunk', 'error');\n"
        "    resetUpload();\n"
        "  };\n"
        "  \n"
        "  currentUpload.reader.readAsArrayBuffer(chunk);\n"
        "}\n"
        "\n"
        "function resetUpload() {\n"
        "  currentUpload = null;\n"
        "  document.getElementById('uploadBtnTCP').disabled = false;\n"
        "  document.getElementById('uploadBtn').disabled = false;\n"
        "  document.getElementById('uploadBtnHTTP').disabled = false;\n"
        "  hideUploadProgress();\n"
        "}\n"
        "\n"
        "function uploadFile() {\n"
        "  const fileInput = document.getElementById('fileUpload');\n"
        "  const uploadStatus = document.getElementById('uploadStatus');\n"
        "  const uploadBtnTCP = document.getElementById('uploadBtnTCP');\n"
        "  const uploadBtn = document.getElementById('uploadBtn');\n"
        "  const uploadBtnHTTP = document.getElementById('uploadBtnHTTP');\n"
        "  \n"
        "  if (!fileInput.files || fileInput.files.length === 0) {\n"
        "    showUploadStatus('Please select a file to upload', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  const file = fileInput.files[0];\n"
        "  const maxSize = 50 * 1024 * 1024; // 50MB limit for HTTP\n"
        "  \n"
        "  if (file.size > maxSize) {\n"
        "    showUploadStatus('File too large for HTTP upload. Use WebSocket upload for files larger than 50MB', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  const formData = new FormData();\n"
        "  formData.append('file', file);\n"
        "  \n"
        "  uploadBtn.disabled = true;\n"
        "  uploadBtnHTTP.disabled = true;\n"
        "  showUploadStatus('Uploading ' + file.name + ' via HTTP (slower)...', 'progress');\n"
        "  \n"
        "  fetch('/upload_file', {\n"
        "    method: 'POST',\n"
        "    body: formData\n"
        "  })\n"
        "  .then(response => response.json())\n"
        "  .then(data => {\n"
        "    if (data.success) {\n"
        "      showUploadStatus('File uploaded successfully via HTTP!', 'success');\n"
        "      document.getElementById('fileUpload').value = '';\n"
        "      if (document.getElementById('sdFileList').style.display !== 'none') {\n"
        "        setTimeout(() => refreshSDFiles(), 1000);\n"
        "      }\n"
        "    } else {\n"
        "      showUploadStatus('HTTP upload failed: ' + (data.error || 'Unknown error'), 'error');\n"
        "    }\n"
        "  })\n"
        "  .catch(error => {\n"
        "    console.error('Upload error:', error);\n"
        "    showUploadStatus('HTTP upload failed: Network error', 'error');\n"
        "  })\n"
        "  .finally(() => {\n"
        "    uploadBtn.disabled = false;\n"
        "    uploadBtnHTTP.disabled = false;\n"
        "  });\n"
        "}\n"
        "\n"
        "function showUploadProgress(uploadedBytes, totalBytes) {\n"
        "  const progressDiv = document.getElementById('uploadProgress');\n"
        "  const progressBar = document.getElementById('uploadProgressBar');\n"
        "  const progressText = document.getElementById('uploadProgressText');\n"
        "  \n"
        "  const percentage = Math.round((uploadedBytes / totalBytes) * 100);\n"
        "  \n"
        "  // Calculate upload speed and ETA\n"
        "  if (!window.uploadStartTime) {\n"
        "    window.uploadStartTime = Date.now();\n"
        "  }\n"
        "  \n"
        "  const elapsedTime = (Date.now() - window.uploadStartTime) / 1000; // seconds\n"
        "  const uploadSpeed = uploadedBytes / elapsedTime; // bytes per second\n"
        "  const remainingBytes = totalBytes - uploadedBytes;\n"
        "  const eta = remainingBytes / uploadSpeed; // seconds\n"
        "  \n"
        "  let speedText = '';\n"
        "  let etaText = '';\n"
        "  \n"
        "  if (elapsedTime > 1 && uploadedBytes > 0) {\n"
        "    speedText = ' - ' + formatFileSize(uploadSpeed) + '/s';\n"
        "    if (eta > 0 && eta < 3600) {\n"
        "      const etaMinutes = Math.floor(eta / 60);\n"
        "      const etaSeconds = Math.floor(eta % 60);\n"
        "      etaText = ' - ETA: ' + etaMinutes + 'm ' + etaSeconds + 's';\n"
        "    }\n"
        "  }\n"
        "  \n"
        "  progressDiv.style.display = 'block';\n"
        "  progressBar.style.width = percentage + '%';\n"
        "  progressText.textContent = formatFileSize(uploadedBytes) + ' / ' + formatFileSize(totalBytes) + ' (' + percentage + '%)' + speedText + etaText;\n"
        "}\n"
        "\n"
        "function hideUploadProgress() {\n"
        "  document.getElementById('uploadProgress').style.display = 'none';\n"
        "}\n"
        "\n"
        "function showUploadStatus(message, type) {\n"
        "  const uploadStatus = document.getElementById('uploadStatus');\n"
        "  uploadStatus.textContent = message;\n"
        "  uploadStatus.className = 'upload-' + type;\n"
        "  uploadStatus.style.display = 'block';\n"
        "  \n"
        "  if (type === 'success' || type === 'error') {\n"
        "    setTimeout(() => {\n"
        "      uploadStatus.style.display = 'none';\n"
        "    }, 5000);\n"
        "  }\n"
        "}\n"
        "\n"
        "// TCP Upload Function - Direct socket connection for maximum speed\n"
        "function uploadFileTCP() {\n"
        "  const fileInput = document.getElementById('fileUpload');\n"
        "  const uploadStatus = document.getElementById('uploadStatus');\n"
        "  const uploadBtnTCP = document.getElementById('uploadBtnTCP');\n"
        "  const uploadBtn = document.getElementById('uploadBtn');\n"
        "  const uploadBtnHTTP = document.getElementById('uploadBtnHTTP');\n"
        "  \n"
        "  if (!fileInput.files || fileInput.files.length === 0) {\n"
        "    showUploadStatus('Please select a file to upload', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  const file = fileInput.files[0];\n"
        "  const maxSize = 500 * 1024 * 1024; // 500MB limit for TCP\n"
        "  \n"
        "  if (file.size > maxSize) {\n"
        "    showUploadStatus('File too large for TCP upload (max 500MB)', 'error');\n"
        "    return;\n"
        "  }\n"
        "  \n"
        "  uploadBtnTCP.disabled = true;\n"
        "  uploadBtn.disabled = true;\n"
        "  uploadBtnHTTP.disabled = true;\n"
        "  showUploadStatus('Uploading ' + file.name + ' via TCP (fastest)...', 'progress');\n"
        "  \n"
        "  // Start TCP-like upload using HTTP chunked transfer\n"
        "  uploadViaTCP(file);\n"
        "}\n"
        "\n"
        "function uploadViaTCP(file) {\n"
        "  const CHUNK_SIZE = 32 * 1024; // 32KB chunks for reliable performance\n"
        "  \n"
        "  showUploadProgress(0, file.size);\n"
        "  window.uploadStartTime = Date.now();\n"
        "  \n"
        "  let uploadedBytes = 0;\n"
        "  let chunkIndex = 0;\n"
        "  const totalChunks = Math.ceil(file.size / CHUNK_SIZE);\n"
        "  \n"
        "  function uploadNextChunk() {\n"
        "    if (uploadedBytes >= file.size) {\n"
        "      showUploadStatus('TCP upload completed: ' + file.name, 'success');\n"
        "      hideUploadProgress();\n"
        "      document.getElementById('uploadBtnTCP').disabled = false;\n"
        "      document.getElementById('uploadBtn').disabled = false;\n"
        "      document.getElementById('uploadBtnHTTP').disabled = false;\n"
        "      refreshSDFiles();\n"
        "      return;\n"
        "    }\n"
        "    \n"
        "    const start = chunkIndex * CHUNK_SIZE;\n"
        "    const end = Math.min(start + CHUNK_SIZE, file.size);\n"
        "    const chunk = file.slice(start, end);\n"
        "    \n"
        "    // Use new streaming approach with metadata in headers\n"
        "    fetch('/tcp_upload_chunk', {\n"
        "      method: 'POST',\n"
        "      headers: {\n"
        "        'Content-Type': 'application/octet-stream',\n"
        "        'X-Filename': file.name,\n"
        "        'X-Chunk-Index': chunkIndex.toString(),\n"
        "        'X-Total-Size': file.size.toString(),\n"
        "        'X-Total-Chunks': totalChunks.toString()\n"
        "      },\n"
        "      body: chunk\n"
        "    })\n"
        "    .then(response => response.json())\n"
        "    .then(data => {\n"
        "      if (data.success) {\n"
        "        uploadedBytes += chunk.size;\n"
        "        chunkIndex++;\n"
        "        showUploadProgress(uploadedBytes, file.size);\n"
        "        \n"
        "        // Continue with next chunk immediately for maximum speed\n"
        "        setTimeout(uploadNextChunk, 5); // Minimal delay\n"
        "      } else {\n"
        "        showUploadStatus('TCP upload failed: ' + (data.error || 'Unknown error'), 'error');\n"
        "        document.getElementById('uploadBtnTCP').disabled = false;\n"
        "        document.getElementById('uploadBtn').disabled = false;\n"
        "        document.getElementById('uploadBtnHTTP').disabled = false;\n"
        "      }\n"
        "    })\n"
        "    .catch(error => {\n"
        "      console.error('TCP upload error:', error);\n"
        "      showUploadStatus('TCP upload failed: Network error', 'error');\n"
        "      document.getElementById('uploadBtnTCP').disabled = false;\n"
        "      document.getElementById('uploadBtn').disabled = false;\n"
        "      document.getElementById('uploadBtnHTTP').disabled = false;\n"
        "    });\n"
        "  }\n"
        "  \n"
        "  // Start upload\n"
        "  uploadNextChunk();\n"
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

    // Send HTML directly without intermediate buffer to avoid truncation
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
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
             "\"pipeline_running\":%s,"
             "\"playing_from_sd\":%s,"
             "\"current_sd_file\":\"%s\""
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
             global_pipeline ? "true" : "false",
             is_playing_from_sd ? "true" : "false",
             current_sd_file);

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

// HTTP handler for file upload to SD card
static esp_err_t upload_file_handler(httpd_req_t *req)
{
    char filepath[256];
    FILE *fd = NULL;
    int remaining = req->content_len;
    int received;
    char *buf = NULL;
    char filename[128] = {0};
    bool file_opened = false;
    
    ESP_LOGI("HTTP_SERVER", "File upload started, content length: %d", remaining);

    // Check if content length is reasonable (max 50MB)
    if (remaining > 50 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 50MB)");
        return ESP_FAIL;
    }

    // Allocate buffer for reading chunks
    buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Set response type
    httpd_resp_set_type(req, "application/json");
    
    // Read first chunk to parse headers
    received = httpd_req_recv(req, buf, 4096);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    
    ESP_LOGI("HTTP_SERVER", "Received first chunk: %d bytes", received);
    
    // Parse multipart form data to extract filename
    char *filename_start = strstr(buf, "filename=\"");
    if (filename_start && filename_start < buf + received) {
        filename_start += 10; // Skip 'filename="'
        char *filename_end = strchr(filename_start, '"');
        if (filename_end && filename_end < buf + received) {
            int len = MIN(filename_end - filename_start, sizeof(filename) - 1);
            strncpy(filename, filename_start, len);
            filename[len] = 0;
            ESP_LOGI("HTTP_SERVER", "Extracted filename: %s", filename);
            
            // Find the start of file content (after \r\n\r\n)
            char *content_start = strstr(filename_end, "\r\n\r\n");
            if (content_start && content_start < buf + received) {
                content_start += 4;
                
                // Create file path
                snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
                
                // Open file for writing
                fd = fopen(filepath, "wb");
                if (fd) {
                    file_opened = true;
                    ESP_LOGI("HTTP_SERVER", "Opened file for writing: %s", filepath);
                    
                    // Write the file content from first chunk
                    int content_in_first_chunk = received - (content_start - buf);
                    if (content_in_first_chunk > 0) {
                        size_t written = fwrite(content_start, 1, content_in_first_chunk, fd);
                        ESP_LOGI("HTTP_SERVER", "Wrote %d bytes from first chunk", written);
                    }
                    
                    // Update remaining bytes
                    remaining -= received;
                    
                    // Continue reading remaining chunks
                    while (remaining > 0) {
                        received = httpd_req_recv(req, buf, MIN(remaining, 4096));
                        if (received <= 0) {
                            ESP_LOGE("HTTP_SERVER", "Failed to receive chunk, remaining: %d", remaining);
                            break;
                        }
                        
                        // Look for multipart boundary (end of file)
                        char *boundary_pos = strstr(buf, "\r\n--");
                        if (boundary_pos) {
                            // Only write content before boundary
                            int write_len = boundary_pos - buf;
                            if (write_len > 0) {
                                fwrite(buf, 1, write_len, fd);
                            }
                            ESP_LOGI("HTTP_SERVER", "Found boundary, wrote final %d bytes", write_len);
                            break;
                        } else {
                            // Write entire chunk
                            size_t written = fwrite(buf, 1, received, fd);
                            if (written != received) {
                                ESP_LOGE("HTTP_SERVER", "Write error: expected %d, wrote %zu", received, written);
                            }
                        }
                        remaining -= received;
                    }
                    
                    fclose(fd);
                    ESP_LOGI("HTTP_SERVER", "File upload completed: %s", filepath);
                } else {
                    ESP_LOGE("HTTP_SERVER", "Failed to open file for writing: %s", filepath);
                }
            } else {
                ESP_LOGE("HTTP_SERVER", "Could not find content start marker");
            }
        } else {
            ESP_LOGE("HTTP_SERVER", "Could not parse filename end");
        }
    } else {
        ESP_LOGE("HTTP_SERVER", "Could not find filename in multipart data");
    }

    free(buf);

    // Send response
    if (file_opened && strlen(filename) > 0) {
        char response[512];
        snprintf(response, sizeof(response), 
            "{\"success\":true,\"filename\":\"%.100s\",\"path\":\"%.200s\"}", 
            filename, filepath);
        httpd_resp_send(req, response, strlen(response));
        ESP_LOGI("HTTP_SERVER", "File uploaded successfully: %s", filepath);
    } else {
        char *error_response = "{\"success\":false,\"error\":\"Failed to parse or save file\"}";
        httpd_resp_send(req, error_response, strlen(error_response));
        ESP_LOGE("HTTP_SERVER", "File upload failed - filename: %s, file_opened: %d", filename, file_opened);
    }

    return ESP_OK;
}

// Fast binary upload handler - streams directly to SD card
static esp_err_t tcp_upload_chunk_handler(httpd_req_t *req)
{
    ESP_LOGI("TCP_UPLOAD", "=== Fast binary upload handler called ===");
    
    static FILE *upload_file = NULL;
    static char upload_filename[300] = {0};
    static uint32_t total_size = 0;
    static uint32_t received_size = 0;
    static uint32_t expected_chunks = 0;
    static uint32_t received_chunks = 0;
    
    // Get metadata from headers instead of multipart parsing
    char filename[256] = {0};
    char chunk_index_str[16] = {0};
    char total_size_str[32] = {0};
    char total_chunks_str[16] = {0};
    
    // Extract metadata from HTTP headers
    size_t header_len = httpd_req_get_hdr_value_len(req, "X-Filename");
    if (header_len > 0 && header_len < sizeof(filename)) {
        httpd_req_get_hdr_value_str(req, "X-Filename", filename, header_len + 1);
    }
    
    header_len = httpd_req_get_hdr_value_len(req, "X-Chunk-Index");
    if (header_len > 0 && header_len < sizeof(chunk_index_str)) {
        httpd_req_get_hdr_value_str(req, "X-Chunk-Index", chunk_index_str, header_len + 1);
    }
    
    header_len = httpd_req_get_hdr_value_len(req, "X-Total-Size");
    if (header_len > 0 && header_len < sizeof(total_size_str)) {
        httpd_req_get_hdr_value_str(req, "X-Total-Size", total_size_str, header_len + 1);
    }
    
    header_len = httpd_req_get_hdr_value_len(req, "X-Total-Chunks");
    if (header_len > 0 && header_len < sizeof(total_chunks_str)) {
        httpd_req_get_hdr_value_str(req, "X-Total-Chunks", total_chunks_str, header_len + 1);
    }
    
    ESP_LOGI("TCP_UPLOAD", "Headers: filename='%s' chunk=%s/%s total=%s", 
             filename, chunk_index_str, total_chunks_str, total_size_str);
    
    // Validate required headers
    if (strlen(filename) == 0) {
        ESP_LOGE("TCP_UPLOAD", "Missing X-Filename header");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename header");
        return ESP_FAIL;
    }
    
    uint32_t chunk_index = atoi(chunk_index_str);
    size_t content_length = req->content_len;
    
    ESP_LOGI("TCP_UPLOAD", "Processing chunk %u: %zu bytes for '%s'", chunk_index, content_length, filename);
    
    // Initialize upload session on first chunk
    if (chunk_index == 0) {
        // Close any previous file
        if (upload_file) {
            fclose(upload_file);
            upload_file = NULL;
        }
        
        total_size = atoi(total_size_str);
        expected_chunks = atoi(total_chunks_str);
        received_size = 0;
        received_chunks = 0;
        
        // Create file path
        snprintf(upload_filename, sizeof(upload_filename), "/sdcard/%s", filename);
        
        // Open file for writing
        upload_file = fopen(upload_filename, "wb");
        if (!upload_file) {
            ESP_LOGE("TCP_UPLOAD", "Failed to create file: %s", upload_filename);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
            return ESP_FAIL;
        }
        
        ESP_LOGI("TCP_UPLOAD", "Started upload: %s (%u bytes, %u chunks)", filename, total_size, expected_chunks);
    }
    
    if (!upload_file) {
        ESP_LOGE("TCP_UPLOAD", "No active upload session for chunk %u", chunk_index);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No active upload session");
        return ESP_FAIL;
    }
    
    // Stream directly to SD card with optimized buffer size
    const size_t buffer_size = 16 * 1024; // 16KB streaming buffer - balanced performance
    char *stream_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    if (!stream_buffer) {
        stream_buffer = malloc(buffer_size);
        if (!stream_buffer) {
            ESP_LOGE("TCP_UPLOAD", "Failed to allocate %zu bytes for streaming buffer", buffer_size);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_FAIL;
        }
    }
    
    size_t remaining = content_length;
    size_t chunk_received = 0;
    
    while (remaining > 0) {
        size_t to_read = MIN(remaining, buffer_size);
        int received = httpd_req_recv(req, stream_buffer, to_read);
        
        if (received <= 0) {
            ESP_LOGE("TCP_UPLOAD", "Failed to receive data: %d", received);
            free(stream_buffer);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read data");
            return ESP_FAIL;
        }
        
        // Write immediately to SD card
        size_t written = fwrite(stream_buffer, 1, received, upload_file);
        if (written != received) {
            ESP_LOGE("TCP_UPLOAD", "Write failed: expected %d, wrote %zu", received, written);
            free(stream_buffer);
            fclose(upload_file);
            upload_file = NULL;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        
        remaining -= received;
        chunk_received += received;
        
        // Yield less frequently for better performance - every 128KB
        if (chunk_received % (128 * 1024) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    free(stream_buffer);
    fflush(upload_file);
    
    received_size += chunk_received;
    received_chunks++;
    
    // Log progress
    if (received_chunks % 10 == 0 || received_chunks == expected_chunks) {
        float progress = (float)received_size / total_size * 100.0f;
        ESP_LOGI("TCP_UPLOAD", "Progress: %.1f%% (%u/%u chunks, %u bytes)", 
                 progress, received_chunks, expected_chunks, received_size);
    }
    
    // Check if upload is complete
    bool upload_complete = (received_chunks >= expected_chunks);
    if (upload_complete && upload_file) {
        fclose(upload_file);
        upload_file = NULL;
        ESP_LOGI("TCP_UPLOAD", "Upload completed: %s (%u bytes)", upload_filename, received_size);
    }
    
    // Send JSON response
    httpd_resp_set_type(req, "application/json");
    char response[256];
    snprintf(response, sizeof(response), 
        "{\"success\":true,\"chunk_index\":%s,\"received_chunks\":%" PRIu32 ",\"total_chunks\":%" PRIu32 ",\"complete\":%s}",
        chunk_index_str, received_chunks, expected_chunks, upload_complete ? "true" : "false");
    
    return httpd_resp_send(req, response, strlen(response));
}

// HTTP handler for deleting files from SD card
static esp_err_t delete_file_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    buf[ret] = 0;

    // Parse JSON
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *file_path = cJSON_GetObjectItem(json, "file_path");
    if (!cJSON_IsString(file_path)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file_path");
        return ESP_FAIL;
    }

    // Delete the file
    httpd_resp_set_type(req, "application/json");
    
    if (remove(file_path->valuestring) == 0) {
        char response[256];
        snprintf(response, sizeof(response), 
            "{\"success\":true,\"message\":\"File deleted successfully\"}");
        httpd_resp_send(req, response, strlen(response));
        ESP_LOGI("HTTP_SERVER", "File deleted successfully: %s", file_path->valuestring);
    } else {
        char *error_response = "{\"success\":false,\"error\":\"Failed to delete file\"}";
        httpd_resp_send(req, error_response, strlen(error_response));
        ESP_LOGE("HTTP_SERVER", "Failed to delete file: %s", file_path->valuestring);
    }

    cJSON_Delete(json);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    // Log memory status before starting HTTP server
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    
    ESP_LOGI("HTTP_SERVER", "Memory before HTTP server start:");
    ESP_LOGI("HTTP_SERVER", "  Free heap: %zu bytes", free_heap);
    ESP_LOGI("HTTP_SERVER", "  Free PSRAM: %zu bytes", free_psram);
    ESP_LOGI("HTTP_SERVER", "  Min free heap: %zu bytes", min_free_heap);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192; // 8KB stack size (conservative for reliability)
    config.max_uri_handlers = 15; // Increased to accommodate all URI handlers
    config.max_resp_headers = 8; // Reduced response header limit
    config.send_wait_timeout = 30; // Reduced timeout
    config.recv_wait_timeout = 30; // Reduced timeout
    config.max_open_sockets = 4; // Conservative socket count
    config.backlog_conn = 4; // Match backlog to max sockets
    config.ctrl_port = 32768; // Use alternate control port
    config.lru_purge_enable = true; // Enable LRU purging for better memory management

    ESP_LOGI("HTTP_SERVER", "Starting HTTP server on port 80 with 8KB stack, 4 sockets, and 20 URI handlers");

    esp_err_t result = httpd_start(&server, &config);
    if (result == ESP_OK)
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

        // SD Card Files API
        httpd_uri_t sd_files_uri = {
            .uri = "/sd_files",
            .method = HTTP_GET,
            .handler = sd_files_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &sd_files_uri);

        // Play SD File API
        httpd_uri_t play_sd_file_uri = {
            .uri = "/play_sd_file",
            .method = HTTP_POST,
            .handler = play_sd_file_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &play_sd_file_uri);

        // File Upload API
        httpd_uri_t upload_file_uri = {
            .uri = "/upload_file",
            .method = HTTP_POST,
            .handler = upload_file_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &upload_file_uri);

        // TCP Upload Chunk API
        httpd_uri_t tcp_upload_chunk_uri = {
            .uri = "/tcp_upload_chunk",
            .method = HTTP_POST,
            .handler = tcp_upload_chunk_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &tcp_upload_chunk_uri);

        // File Delete API
        httpd_uri_t delete_file_uri = {
            .uri = "/delete_file",
            .method = HTTP_POST,
            .handler = delete_file_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &delete_file_uri);

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

    // Enhanced error reporting
    ESP_LOGE("HTTP_SERVER", "❌ Failed to start HTTP server: %s (0x%x)", esp_err_to_name(result), result);
    
    // Log memory status after failure
    size_t free_heap_after = esp_get_free_heap_size();
    size_t free_psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGE("HTTP_SERVER", "Memory after failure:");
    ESP_LOGE("HTTP_SERVER", "  Free heap: %zu bytes", free_heap_after);
    ESP_LOGE("HTTP_SERVER", "  Free PSRAM: %zu bytes", free_psram_after);
    
    if (result == ESP_ERR_HTTPD_TASK) {
        ESP_LOGE("HTTP_SERVER", "💡 Suggestion: HTTP server task creation failed (likely insufficient memory)");
        ESP_LOGE("HTTP_SERVER", "💡 Try reducing stack_size or max_open_sockets in HTTP config");
    }
    
    return NULL;
}

// Stop and cleanup audio pipeline
static void stop_audio_pipeline(void)
{
    if (!global_pipeline) {
        return; // Nothing to stop
    }

    ESP_LOGI("AUDIO_PLAYER", "🛑 Stopping audio pipeline...");

    // Stop pipeline first
    audio_pipeline_stop(global_pipeline);
    audio_pipeline_wait_for_stop(global_pipeline);

    // Remove listener before cleanup
    audio_pipeline_remove_listener(global_pipeline);

    // Terminate pipeline
    audio_pipeline_terminate(global_pipeline);

    // Unregister and deinitialize elements
    if (global_http_stream) {
        audio_pipeline_unregister(global_pipeline, global_http_stream);
        audio_element_deinit(global_http_stream);
        global_http_stream = NULL;
    }
    if (fatfs_stream_reader) {
        audio_pipeline_unregister(global_pipeline, fatfs_stream_reader);
        audio_element_deinit(fatfs_stream_reader);
        fatfs_stream_reader = NULL;
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

    // Finally deinitialize pipeline
    audio_pipeline_deinit(global_pipeline);
    global_pipeline = NULL;

    // Reset playback state
    is_playing_from_sd = false;
    current_sd_file[0] = 0;

    ESP_LOGI("AUDIO_PLAYER", "✅ Audio pipeline stopped and cleaned up");
}

// Start audio pipeline with given URL
static void start_audio_pipeline(const char *url)
{
    ESP_LOGI("AUDIO_PLAYER", "🎵 Starting audio pipeline with URL: %s", url);

    // Create event interface with additional safety checks
    if (!evt)
    {
        ESP_LOGI("AUDIO_PLAYER", "Creating new audio event interface");
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        evt_cfg.queue_set_size = 3; // Smaller queue to prevent overflow
        evt = audio_event_iface_init(&evt_cfg);
        if (!evt)
        {
            ESP_LOGE("AUDIO_PLAYER", "Failed to create event interface");
            return;
        }
        ESP_LOGI("AUDIO_PLAYER", "Audio event interface created successfully");
    } else {
        ESP_LOGI("AUDIO_PLAYER", "Reusing existing audio event interface");
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

// Initialize SD card peripheral
// Initialize SD card using direct SPI implementation
static esp_err_t init_sdcard(void)
{
    ESP_LOGI("main", "🗂️ Initializing SD card via SPI...");
    ESP_LOGI("main", "🔧 Function entry - starting SPI configuration...");
    
    // SD card SPI configuration
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_11,  // MOSI
        .miso_io_num = GPIO_NUM_13,  // MISO  
        .sclk_io_num = GPIO_NUM_12,  // SCLK
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    // Initialize SPI bus
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_LOGI("main", "🔧 SPI bus initialize result: %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ESP_LOGE("main", "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI("main", "✅ SPI bus initialized successfully");
    
    // SD card host configuration
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_10;  // Chip Select
    slot_config.host_id = SPI2_HOST;
    
    // Mount configuration with Long Filename Support
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 20,  // Increase max files for better LFN support
        .allocation_unit_size = 64 * 1024,  // Use larger allocation unit like ESP-ADF
        .disk_status_check_enable = false   // Disable status check for better compatibility
    };
    
    // Mount the SD card
    sdmmc_card_t *card;
    ESP_LOGI("main", "🔧 Attempting to mount SD card...");
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    ESP_LOGI("main", "🔧 Mount result: %s", esp_err_to_name(ret));
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("main", "Failed to mount filesystem. Check if SD card is inserted and formatted.");
        } else {
            ESP_LOGE("main", "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        // Don't return error - allow system to continue without SD card
        ESP_LOGW("main", "⚠️ SD card not available - system will continue without SD card support");
        return ESP_OK;
    }
    
    // Print card info
    ESP_LOGI("SDCARD", "✅ SD card mounted successfully");
    ESP_LOGI("SDCARD", "📊 SD Card Info:");
    ESP_LOGI("SDCARD", "   Name: %s", card->cid.name);
    ESP_LOGI("SDCARD", "   Speed: %s", (card->csd.tr_speed > 25000000) ? "high speed" : "default speed");
    ESP_LOGI("SDCARD", "   Size: %lluMB", ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    
    return ESP_OK;
}

// Get audio format from file extension
static audio_format_t detect_audio_format_from_file(const char *filename)
{
    if (!filename) return AUDIO_FORMAT_UNKNOWN;
    
    const char *ext = strrchr(filename, '.');
    if (!ext) return AUDIO_FORMAT_UNKNOWN;
    
    ext++; // Skip the dot
    
    if (strcasecmp(ext, "mp3") == 0) return AUDIO_FORMAT_MP3;
    if (strcasecmp(ext, "aac") == 0 || strcasecmp(ext, "m4a") == 0) return AUDIO_FORMAT_AAC;
    if (strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "fla") == 0) return AUDIO_FORMAT_FLAC;  // Handle 8.3 filenames
    if (strcasecmp(ext, "wav") == 0) return AUDIO_FORMAT_WAV;
    if (strcasecmp(ext, "ogg") == 0) return AUDIO_FORMAT_OGG;
    if (strcasecmp(ext, "amr") == 0) return AUDIO_FORMAT_AMR;
    
    return AUDIO_FORMAT_UNKNOWN;
}

// Start audio pipeline with SD card file
static void start_audio_pipeline_from_sd(const char *file_path)
{
    ESP_LOGI("AUDIO_PLAYER", "🎵 Starting audio pipeline with SD file: %s", file_path);

    // Stop current pipeline if running
    stop_audio_pipeline();

    // Create event interface
    if (!evt)
    {
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        evt_cfg.queue_set_size = 3;
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

    // Create FATFS stream reader
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);
    if (!fatfs_stream_reader)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create FATFS stream");
        goto cleanup_pipeline;
    }

    // Detect audio format from file extension
    current_audio_format = detect_audio_format_from_file(file_path);
    strncpy(current_decoder_name, get_decoder_name(current_audio_format), sizeof(current_decoder_name) - 1);
    current_decoder_name[sizeof(current_decoder_name) - 1] = 0;

    ESP_LOGI("AUDIO_PLAYER", "🔍 Detected audio format: %s from file: %s", current_decoder_name, file_path);

    global_decoder = create_decoder_for_format(current_audio_format);
    if (!global_decoder)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create %s decoder", current_decoder_name);
        goto cleanup_fatfs;
    }

    ESP_LOGI("AUDIO_PLAYER", "✅ Created %s decoder successfully", current_decoder_name);

    // Create equalizer for volume control
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    static int eq_gain[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    eq_cfg.set_gain = eq_gain;
    eq_cfg.task_stack = 4096 * 6;
    eq_cfg.task_prio = 5;
    eq_cfg.task_core = 1;
    global_equalizer = equalizer_init(&eq_cfg);
    if (!global_equalizer)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create equalizer");
        goto cleanup_decoder;
    }
    ESP_LOGI("AUDIO_PLAYER", "🎛️ Equalizer created for volume control");

    // Create I2S stream (reuse existing configuration)
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.buffer_len = 1024 * 36;
    i2s_cfg.use_alc = true;
    i2s_cfg.task_stack = 4096 * 4;
    i2s_cfg.chan_cfg.dma_desc_num = 8;
    i2s_cfg.chan_cfg.dma_frame_num = 1023;
    i2s_cfg.std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    i2s_cfg.std_cfg.gpio_cfg.mclk = GPIO_NUM_NC;

    global_i2s_stream = i2s_stream_init(&i2s_cfg);
    if (!global_i2s_stream)
    {
        ESP_LOGE("AUDIO_PLAYER", "Failed to create I2S stream");
        goto cleanup_equalizer;
    }
    ESP_LOGI("AUDIO_PLAYER", "🔊 I2S stream created successfully");

    // Register all elements to pipeline
    audio_pipeline_register(global_pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(global_pipeline, global_decoder, current_decoder_name);
    audio_pipeline_register(global_pipeline, global_equalizer, "equalizer");
    audio_pipeline_register(global_pipeline, global_i2s_stream, "i2s");

    // Set up the pipeline - file -> decoder -> equalizer -> i2s
    const char *link_tag[4] = {"file", current_decoder_name, "equalizer", "i2s"};
    audio_pipeline_link(global_pipeline, &link_tag[0], 4);

    // Set up event listener
    audio_pipeline_set_listener(global_pipeline, evt);

    // Set the URI for the file
    audio_element_set_uri(fatfs_stream_reader, file_path);

    // Set initial volume
    update_pipeline_volume(current_volume);

    // Start the pipeline
    audio_pipeline_run(global_pipeline);

    // Mark as playing from SD
    is_playing_from_sd = true;
    strncpy(current_sd_file, file_path, sizeof(current_sd_file) - 1);
    current_sd_file[sizeof(current_sd_file) - 1] = 0;

    // Send WebSocket notification about file playing
    char ws_message[512];
    snprintf(ws_message, sizeof(ws_message), 
        "{\"type\":\"sd_file_playing\",\"data\":{\"file_path\":\"%s\",\"file_name\":\"%s\"}}", 
        file_path, strrchr(file_path, '/') ? strrchr(file_path, '/') + 1 : file_path);
    broadcast_message(ws_message);

    ESP_LOGI("AUDIO_PLAYER", "✅ SD card audio pipeline started successfully");
    return;

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
cleanup_fatfs:
    if (fatfs_stream_reader) {
        audio_element_deinit(fatfs_stream_reader);
        fatfs_stream_reader = NULL;
    }
cleanup_pipeline:
    if (global_pipeline) {
        audio_pipeline_deinit(global_pipeline);
        global_pipeline = NULL;
    }
}

void app_main(void)
{
    // Initialize cJSON to use PSRAM for large allocations
    cJSON_Hooks hooks;
    hooks.malloc_fn = cjson_malloc_psram;
    hooks.free_fn = cjson_free_psram;
    cJSON_InitHooks(&hooks);
    ESP_LOGI("CJSON", "Initialized cJSON with PSRAM support for large allocations");

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("wifi_idf", ESP_LOG_INFO);
    esp_log_level_set("HTTP_SERVER", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_PLAYER", ESP_LOG_INFO);
    esp_log_level_set("BUFFER_MONITOR", ESP_LOG_INFO); // Enable buffer monitoring logs
    esp_log_level_set("SDCARD", ESP_LOG_INFO); // Enable SD card logs

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

    // Initialize upload session
    init_upload_session();

    // Initialize WiFi
    init_wifi();

    // Initialize RGB LED
    init_rgb_led();

    // Initialize SD card
    ESP_LOGI("main", "🗂️ About to initialize SD card...");
    init_sdcard();
    ESP_LOGI("main", "🗂️ SD card initialization completed");

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

        // Start TCP file upload server
        start_tcp_upload_server();

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

            // Monitor pipeline status with safety checks
            if (global_pipeline && evt)
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