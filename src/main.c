/* Play an MP3, AAC or WAV file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "board.h"

#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

#define SELECT_MP3_DECODER 1

// Include all possible decoders
#if defined SELECT_AAC_DECODER
#include "aac_decoder.h"
#elif defined SELECT_AMR_DECODER
#include "amr_decoder.h"
#elif defined SELECT_FLAC_DECODER
#include "flac_decoder.h"
#elif defined SELECT_MP3_DECODER
#include "mp3_decoder.h"
#elif defined SELECT_OGG_DECODER
#include "ogg_decoder.h"
#elif defined SELECT_OPUS_DECODER
#include "opus_decoder.h"
#else
#include "wav_decoder.h"
#endif

// Global handles for the audio task
static audio_pipeline_handle_t global_pipeline = NULL;
static audio_element_handle_t global_http_stream = NULL;
static audio_element_handle_t global_decoder = NULL;
static audio_element_handle_t global_i2s_stream = NULL;

// WiFi event group and status
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

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
static void wifi_init_sta(void)
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

// Decoder configuration based on selected type
#if defined SELECT_AAC_DECODER
static const char *TAG = "HTTP_SELECT_AAC_EXAMPLE";
static const char *selected_decoder_name = "aac";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.aac";
#elif defined SELECT_AMR_DECODER
static const char *TAG = "HTTP_SELECT_AMR_EXAMPLE";
static const char *selected_decoder_name = "amr";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-1c-8000hz.amr";
#elif defined SELECT_FLAC_DECODER
static const char *TAG = "HTTP_SELECT_FLAC_EXAMPLE";
static const char *selected_decoder_name = "flac";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.flac";
#elif defined SELECT_MP3_DECODER
static const char *TAG = "HTTP_SELECT_MP3_EXAMPLE";
static const char *selected_decoder_name = "mp3";
static const char *selected_file_to_play = "https://stream.zeno.fm/vq6p5vxb4v8uv";
#elif defined SELECT_OGG_DECODER
static const char *TAG = "HTTP_SELECT_OGG_EXAMPLE";
static const char *selected_decoder_name = "ogg";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.ogg";
#elif defined SELECT_OPUS_DECODER
static const char *TAG = "HTTP_SELECT_OPUS_EXAMPLE";
static const char *selected_decoder_name = "opus";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.opus";
#else
static const char *TAG = "HTTP_SELECT_WAV_EXAMPLE";
static const char *selected_decoder_name = "wav";
static const char *selected_file_to_play = "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.wav";
#endif

// Audio playback task
static void audio_playback_task(void *pvParameters) {
    ESP_LOGI(TAG, "🎵 Audio playback task started on core %d", xPortGetCoreID());
    
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_reader, i2s_stream_writer, selected_decoder;

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_cfg.rb_size = 1024 * 1024;  // 1MB pipeline buffer
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.out_rb_size = 2048 * 1024;  // 2MB buffer for FLAC stability
    http_cfg.auto_connect_next_track = true;  // Auto reconnect
    http_cfg.stack_in_ext = true;        // Use external memory
    http_cfg.task_stack = 24576;         // Large stack for FLAC
    http_cfg.task_prio = 12;             // High priority for HTTP
    http_cfg.task_core = 0;              // Run HTTP on core 0
    
    // Enhanced stability settings for EMI resistance
   // http_cfg.buffer_len = 8192;          // Larger internal buffer
    http_cfg.request_size = 32768;       // Smaller request chunks to reduce WiFi load
    http_cfg.request_range_size = 65536; // Moderate range size
    // http_cfg.timeout_ms = 30000;      // 30 second timeout (not supported in this version)
    
    http_stream_reader = http_stream_init(&http_cfg);
    ESP_LOGI(TAG, "HTTP configured for EMI stability: req_size=%dKB",
             http_cfg.request_size/1024);

    ESP_LOGI(TAG, "[2.2] Create %s decoder to decode %s file", selected_decoder_name, selected_decoder_name);
    
#if defined SELECT_AAC_DECODER
    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    aac_cfg.out_rb_size = 512 * 1024;    // 512KB buffer for AAC
    aac_cfg.stack_in_ext = true;         // Use external memory
    aac_cfg.task_stack = 16384;          // Large task stack for AAC
    aac_cfg.task_prio = 15;              // High priority
    aac_cfg.task_core = 1;               // Pin to core 1 (with I2S)
    selected_decoder = aac_decoder_init(&aac_cfg);
    ESP_LOGI(TAG, "AAC decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             aac_cfg.out_rb_size/1024, aac_cfg.task_stack, aac_cfg.task_prio, aac_cfg.task_core);
#elif defined SELECT_AMR_DECODER
    amr_decoder_cfg_t amr_cfg = DEFAULT_AMR_DECODER_CONFIG();
    amr_cfg.out_rb_size = 256 * 1024;    // 256KB buffer for AMR
    amr_cfg.stack_in_ext = true;         // Use external memory
    amr_cfg.task_stack = 12288;          // Medium task stack for AMR
    amr_cfg.task_prio = 15;              // High priority
    amr_cfg.task_core = 1;               // Pin to core 1 (with I2S)
    selected_decoder = amr_decoder_init(&amr_cfg);
    ESP_LOGI(TAG, "AMR decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             amr_cfg.out_rb_size/1024, amr_cfg.task_stack, amr_cfg.task_prio, amr_cfg.task_core);
#elif defined SELECT_FLAC_DECODER
    flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    flac_cfg.out_rb_size = 1024 * 1024;  // 1MB buffer for FLAC stability
    flac_cfg.stack_in_ext = true;        // Use external memory
    flac_cfg.task_stack = 32768;         // Extra large task stack for FLAC
    flac_cfg.task_prio = 15;             // High priority
    flac_cfg.task_core = 1;              // Pin to core 1 (with I2S)
    selected_decoder = flac_decoder_init(&flac_cfg);
    ESP_LOGI(TAG, "FLAC decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             flac_cfg.out_rb_size/1024, flac_cfg.task_stack, flac_cfg.task_prio, flac_cfg.task_core);
#elif defined SELECT_MP3_DECODER
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.out_rb_size = 512 * 1024;    // 512KB buffer for MP3
    mp3_cfg.stack_in_ext = true;         // Use external memory
    mp3_cfg.task_stack = 16384;          // Large task stack for MP3
    mp3_cfg.task_prio = 15;              // High priority
    mp3_cfg.task_core = 1;               // Pin to core 1 (with I2S)
    selected_decoder = mp3_decoder_init(&mp3_cfg);
    ESP_LOGI(TAG, "MP3 decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             mp3_cfg.out_rb_size/1024, mp3_cfg.task_stack, mp3_cfg.task_prio, mp3_cfg.task_core);
#elif defined SELECT_OGG_DECODER
    ogg_decoder_cfg_t ogg_cfg = DEFAULT_OGG_DECODER_CONFIG();
    ogg_cfg.out_rb_size = 512 * 1024;    // 512KB buffer for OGG
    ogg_cfg.stack_in_ext = true;         // Use external memory
    ogg_cfg.task_stack = 16384;          // Large task stack for OGG
    ogg_cfg.task_prio = 15;              // High priority
    ogg_cfg.task_core = 1;               // Pin to core 1 (with I2S)
    selected_decoder = ogg_decoder_init(&ogg_cfg);
    ESP_LOGI(TAG, "OGG decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             ogg_cfg.out_rb_size/1024, ogg_cfg.task_stack, ogg_cfg.task_prio, ogg_cfg.task_core);
#elif defined SELECT_OPUS_DECODER
    opus_decoder_cfg_t opus_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    opus_cfg.out_rb_size = 512 * 1024;   // 512KB buffer for OPUS
    opus_cfg.stack_in_ext = true;        // Use external memory
    opus_cfg.task_stack = 16384;         // Large task stack for OPUS
    opus_cfg.task_prio = 15;             // High priority
    opus_cfg.task_core = 1;              // Pin to core 1 (with I2S)
    selected_decoder = decoder_opus_init(&opus_cfg);
    ESP_LOGI(TAG, "OPUS decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             opus_cfg.out_rb_size/1024, opus_cfg.task_stack, opus_cfg.task_prio, opus_cfg.task_core);
#else
    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    wav_cfg.out_rb_size = 256 * 1024;    // 256KB buffer for WAV
    wav_cfg.stack_in_ext = true;         // Use external memory
    wav_cfg.task_stack = 12288;          // Medium task stack for WAV
    wav_cfg.task_prio = 15;              // High priority
    wav_cfg.task_core = 1;               // Pin to core 1 (with I2S)
    selected_decoder = wav_decoder_init(&wav_cfg);
    ESP_LOGI(TAG, "WAV decoder: buffer=%dKB, stack=%d, prio=%d, core=%d", 
             wav_cfg.out_rb_size/1024, wav_cfg.task_stack, wav_cfg.task_prio, wav_cfg.task_core);
#endif

    ESP_LOGI(TAG, "[2.3] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.out_rb_size = 512 * 1024;    // 512KB I2S buffer
    i2s_cfg.stack_in_ext = true;         // Use external memory
    i2s_cfg.task_stack = 16384;          // Large stack
    i2s_cfg.task_prio = 20;              // Highest priority for I2S
    i2s_cfg.task_core = 1;               // Pin to core 1
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, selected_decoder, selected_decoder_name);
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->%s_decoder-->i2s_stream-->[codec_chip]", selected_decoder_name);
    const char *link_tag[3] = {"http", selected_decoder_name, "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[2.6] Set up uri (http as http_stream, %s as %s_decoder, and default output is i2s)",
             selected_decoder_name, selected_decoder_name);
    audio_element_set_uri(http_stream_reader, selected_file_to_play);

    // Set global handles for monitoring
    global_pipeline = pipeline;
    global_http_stream = http_stream_reader;
    global_decoder = selected_decoder;
    global_i2s_stream = i2s_stream_writer;

    ESP_LOGI(TAG, "[4] Set up event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[5] Start audio_pipeline");
    audio_pipeline_run(pipeline);
    
    // Pre-load buffer for FLAC stability
    ESP_LOGI(TAG, "Pre-loading HTTP buffer for FLAC stability...");
    vTaskDelay(3000 / portTICK_PERIOD_MS);  // Wait 3 seconds for initial buffer fill

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, 1000 / portTICK_PERIOD_MS); // 1 second timeout
        
        if (ret != ESP_OK) {
            // Timeout - check buffer levels and power stability
            int http_filled = audio_element_get_output_ringbuf_size(http_stream_reader);
            int decoder_filled = audio_element_get_output_ringbuf_size(selected_decoder);
            ESP_LOGD(TAG, "🎵 Buffer Status - HTTP: %dKB, Decoder: %dKB", 
                     http_filled/1024, decoder_filled/1024);
            
            // Enhanced stall detection for EMI/power issues
            static int last_http_level = 0;
            static int stall_count = 0;
            static int power_cycle_count = 0;
            
            if (http_filled == last_http_level && http_filled < 512 * 1024) {
                stall_count++;
                ESP_LOGW(TAG, "⚠️  Stream stall detected (count=%d) - HTTP: %dKB", 
                         stall_count, http_filled/1024);
                
                if (stall_count >= 3) {  // 3 seconds stalled
                    ESP_LOGE(TAG, "🚨 EMI/Power-related stall detected - implementing recovery!");
                    
                    // Power cycle recovery (simulates touching the chip)
                    power_cycle_count++;
                    ESP_LOGW(TAG, "🔄 Power recovery cycle #%d", power_cycle_count);
                    
                    // Stop pipeline gently
                    audio_pipeline_stop(pipeline);
                    audio_pipeline_wait_for_stop(pipeline);
                    
                    // Brief delay to let power stabilize
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    
                    // Reset all elements completely
                    audio_element_reset_state(selected_decoder);
                    audio_element_reset_state(i2s_stream_writer);
                    audio_element_reset_state(http_stream_reader);
                    audio_pipeline_reset_ringbuffer(pipeline);
                    audio_pipeline_reset_items_state(pipeline);
                    
                    // Additional delay for power stability
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    
                    // Restart with fresh state
                    audio_pipeline_run(pipeline);
                    ESP_LOGI(TAG, "🎵 Pipeline restarted after power recovery");
                    
                    stall_count = 0;
                }
            } else {
                stall_count = 0;
            }
            last_http_level = http_filled;
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) selected_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(selected_decoder, &music_info);

            ESP_LOGI(TAG, "🎵 Music info from %s decoder: %dHz, %d-bit, %d-channel",
                     selected_decoder_name, music_info.sample_rates, music_info.bits, music_info.channels);

            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        // Handle pipeline errors
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            int status = (int)msg.data;
            if (msg.source == (void *)http_stream_reader && status == AEL_STATUS_ERROR_OPEN) {
                ESP_LOGW(TAG, "🔄 HTTP error - restarting stream...");
                audio_pipeline_stop(pipeline);
                audio_pipeline_wait_for_stop(pipeline);
                audio_element_reset_state(selected_decoder);
                audio_element_reset_state(i2s_stream_writer);
                audio_element_reset_state(http_stream_reader);
                audio_pipeline_reset_ringbuffer(pipeline);
                audio_pipeline_reset_items_state(pipeline);
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                audio_pipeline_run(pipeline);
                continue;
            }
        }

        /* Stop when the last pipeline element receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "🛑 Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[6] Stop audio_pipeline and release all resources");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, http_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, selected_decoder);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(selected_decoder);
    
    ESP_LOGI(TAG, "🎵 Audio playback task ended");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "✅ NVS Flash initialized");

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "🎧 === FLAC HTTP STREAMING PLAYER ===");
    ESP_LOGI(TAG, "🎧 ESP32-S3 with 8MB PSRAM @ 80MHz");
    ESP_LOGI(TAG, "🎧 External DAC: PCM1334A");
    ESP_LOGI(TAG, "🎧 Multi-core audio processing");
    ESP_LOGI(TAG, "🎧 ===================================");

    ESP_LOGI(TAG, "[1] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3] Initialize and connect to Wi-Fi");
    wifi_init_sta();
    
    // Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ Connected to WiFi successfully!");
    } else {
        ESP_LOGE(TAG, "❌ Failed to connect to WiFi");
        return;
    }

    // Disable ALL power management for stability
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "✅ WiFi power save disabled for streaming stability");
    
    // Advanced power management for EMI stability
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 240,  // Keep CPU at max frequency - prevents voltage fluctuations
        .light_sleep_enable = false  // Disable light sleep completely
    };
    esp_pm_configure(&pm_config);
    ESP_LOGI(TAG, "✅ Power management: CPU locked at 240MHz, light sleep disabled");
    
    // Reduce WiFi transmission power to minimize EMI
    esp_wifi_set_max_tx_power(78);  // Keep at default for now
    ESP_LOGI(TAG, "✅ WiFi TX power configured");
    
    // Set WiFi to use only 2.4GHz band with specific channel to avoid interference
    wifi_country_t country_cfg = {
        .cc = "IN",
        .schan = 1,
        .nchan = 11,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_wifi_set_country(&country_cfg);
    ESP_LOGI(TAG, "✅ WiFi country/channel configuration set for stability");

    // Create audio playback task on core 1 (with high priority)
    ESP_LOGI(TAG, "🎵 Starting audio playback task...");
    xTaskCreatePinnedToCore(
        audio_playback_task,     // Task function
        "audio_playback",        // Task name
        32768,                   // Stack size (32KB)
        NULL,                    // Parameters
        20,                      // Priority (high)
        NULL,                    // Task handle
        1                        // Core 1 (dedicated for audio)
    );

    ESP_LOGI(TAG, "🎵 Audio task started. Main task will monitor system...");
    
    // Main task monitors WiFi and system health
    while (1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);  // Check every 5 seconds for faster response
        
        // Check WiFi connection stability
        wifi_ap_record_t ap_info;
        esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
        
        if (wifi_ret == ESP_OK) {
            ESP_LOGI(TAG, "📶 WiFi: RSSI=%d dBm, Channel=%d", ap_info.rssi, ap_info.primary);
            
            // Enhanced WiFi signal monitoring for EMI issues
            if (ap_info.rssi < -70) {
                ESP_LOGW(TAG, "⚠️  Weak WiFi signal (%d dBm) - potential EMI interference!", ap_info.rssi);
            }
            
            // Check for sudden signal drops (EMI indicator)
            static int last_rssi = 0;
            if (last_rssi != 0 && (ap_info.rssi - last_rssi) < -15) {
                ESP_LOGW(TAG, "🔴 Sudden WiFi signal drop detected: %d -> %d dBm (EMI?)", 
                         last_rssi, ap_info.rssi);
            }
            last_rssi = ap_info.rssi;
            
            // Log buffer status with enhanced monitoring
            if (global_http_stream && global_decoder) {
                int http_filled = audio_element_get_output_ringbuf_size(global_http_stream);
                int decoder_filled = audio_element_get_output_ringbuf_size(global_decoder);
                ESP_LOGI(TAG, "🔊 Audio Buffers - HTTP: %dKB, FLAC: %dKB", 
                         http_filled/1024, decoder_filled/1024);
                
                // Power stability indicator - check for buffer anomalies
                static int stable_buffer_count = 0;
                if (http_filled > 1024*1024 && decoder_filled > 256*1024) {
                    stable_buffer_count++;
                    if (stable_buffer_count == 3) {
                        ESP_LOGI(TAG, "💚 Buffers stable - good power/EMI conditions");
                        stable_buffer_count = 0;
                    }
                } else {
                    stable_buffer_count = 0;
                    if (http_filled < 512*1024) {
                        ESP_LOGW(TAG, "🟡 Low buffer levels - possible power/EMI interference");
                    }
                }
            }
        } else {
            ESP_LOGE(TAG, "❌ WiFi connection lost - attempting reconnection!");
            
            // Trigger reconnection
            esp_wifi_connect();
            
            // Wait for reconnection with timeout
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                   WIFI_CONNECTED_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   10000 / portTICK_PERIOD_MS); // 10 second timeout
            
            if (!(bits & WIFI_CONNECTED_BIT)) {
                ESP_LOGE(TAG, "❌ WiFi reconnection failed!");
            }
        }
        
        // Enhanced system health monitoring
        size_t free_heap = esp_get_free_heap_size();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "💾 Free heap: %zu bytes, Free PSRAM: %zu bytes", free_heap, free_psram);
        
        // Check for memory fragmentation (power stability indicator)
        if (free_heap < 100000) {  // Less than 100KB
            ESP_LOGW(TAG, "⚠️  Low heap memory - system stress indicator");
        }
    }
}
