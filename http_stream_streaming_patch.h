/*
 * HTTP Stream Streaming Optimizations
 * 
 * This file contains macro definitions to override ESP-ADF http_stream.c
 * defaults for better streaming performance and stability.
 * 
 * Include this file before http_stream.h to apply optimizations.
 */

#ifndef _HTTP_STREAM_STREAMING_PATCH_H_
#define _HTTP_STREAM_STREAMING_PATCH_H_

// Override default buffer size for streaming (original: 2048)
#undef HTTP_STREAM_BUFFER_SIZE
#define HTTP_STREAM_BUFFER_SIZE (1024*128)  // 16KB for better streaming performance

// Override max connection attempts for streaming (original: 5)
#undef HTTP_MAX_CONNECT_TIMES
#define HTTP_MAX_CONNECT_TIMES (20)      // More retry attempts for robust streaming

// Enhanced default ringbuffer size for streaming
#undef HTTP_STREAM_RINGBUFFER_SIZE
#define HTTP_STREAM_RINGBUFFER_SIZE (2 * 1024 * 1024)  // 512KB default (original: 20KB)

// Optimized task stack for streaming processing
#undef HTTP_STREAM_TASK_STACK
#define HTTP_STREAM_TASK_STACK (16 * 1024)       // 16KB stack (original: 6KB)

// Higher priority for streaming reliability
#undef HTTP_STREAM_TASK_PRIO
#define HTTP_STREAM_TASK_PRIO (8)                // Higher priority (original: 4)

// Streaming-optimized default configuration
#undef HTTP_STREAM_CFG_DEFAULT
#define HTTP_STREAM_CFG_DEFAULT() {              \
    .type = AUDIO_STREAM_READER,                 \
    .out_rb_size = HTTP_STREAM_RINGBUFFER_SIZE,  \
    .task_stack = HTTP_STREAM_TASK_STACK,        \
    .task_core = HTTP_STREAM_TASK_CORE,          \
    .task_prio = HTTP_STREAM_TASK_PRIO,          \
    .stack_in_ext = true,                        \
    .event_handle = NULL,                        \
    .user_data = NULL,                           \
    .auto_connect_next_track = true,             \
    .enable_playlist_parser = true,              \
    .multi_out_num = 0,                          \
    .cert_pem  = NULL,                           \
    .crt_bundle_attach = NULL,                   \
    .user_agent = "Mozilla/5.0 (compatible; ESP32-AudioStreamer/1.0)", \
    .request_size = 32768,                       \
    .request_range_size = 0                      \
}

#endif // _HTTP_STREAM_STREAMING_PATCH_H_
