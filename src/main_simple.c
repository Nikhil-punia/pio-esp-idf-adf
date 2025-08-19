// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <sys/stat.h>
// #include <dirent.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_wifi.h"
// #include "esp_event.h"
// #include "esp_log.h"
// #include "esp_system.h"
// #include "nvs_flash.h"
// #include "esp_netif.h"
// #include "esp_http_server.h"
// #include "esp_vfs_fat.h"
// #include "sdmmc_cmd.h"
// #include "driver/sdspi_host.h"
// #include "driver/spi_common.h"
// #include "driver/gpio.h"
// #include "lwip/err.h"
// #include "lwip/sockets.h"
// #include "lwip/sys.h"
// #include <lwip/netdb.h>

// static const char *TAG = "SD_WEBSERVER";

// #define WIFI_SSID "Groid"
// #define WIFI_PASS "ghotu440@440"
// #define MOUNT_POINT "/sdcard"
// #define MAX_FILE_SIZE (200*1024*1024) // 200MB max file size

// // Server ports
// #define TCP_PORT 8080
// #define UDP_PORT 8081

// // SD card pin definitions
// #define ESP_SD_PIN_CLK   GPIO_NUM_12  // SPI CLK
// #define ESP_SD_PIN_CMD   GPIO_NUM_11  // SPI MOSI
// #define ESP_SD_PIN_D0    GPIO_NUM_13  // SPI MISO
// #define ESP_SD_PIN_D3    GPIO_NUM_10  // SPI CS

// #ifndef ESP_SD_PIN_D1
// #define ESP_SD_PIN_D1    GPIO_NUM_NC
// #endif
// #ifndef ESP_SD_PIN_D2
// #define ESP_SD_PIN_D2    GPIO_NUM_NC
// #endif
// #ifndef ESP_SD_PIN_CD
// #define ESP_SD_PIN_CD    GPIO_NUM_NC
// #endif
// #ifndef ESP_SD_PIN_WP
// #define ESP_SD_PIN_WP    GPIO_NUM_NC
// #endif

// static httpd_handle_t server = NULL;
// static sdmmc_card_t *card;
// static httpd_handle_t ws_server = NULL;
// static int ws_fd = -1;  // Global WebSocket file descriptor for progress updates

// // WebSocket frame structure for progress updates
// typedef struct {
//     uint8_t fin_rsv_opcode;
//     uint8_t mask_len;
//     uint8_t payload[];
// } ws_frame_t;

// // Protocol definitions for TCP/UDP
// typedef struct {
//     uint32_t filename_len;
//     uint32_t file_size;
//     char data[];  // filename + file data
// } file_transfer_t;

// // Music file detection and album art extraction
// static bool is_music_file(const char* filename) {
//     const char* ext = strrchr(filename, '.');
//     if (!ext) return false;
    
//     return (strcasecmp(ext, ".mp3") == 0 || 
//             strcasecmp(ext, ".flac") == 0 || 
//             strcasecmp(ext, ".m4a") == 0 ||
//             strcasecmp(ext, ".wav") == 0 ||
//             strcasecmp(ext, ".ogg") == 0);
// }

// // Simple ID3v2 album art extraction for MP3 files
// static bool extract_mp3_album_art(const char* filepath, char* art_path, size_t art_path_size) {
//     FILE* f = fopen(filepath, "rb");
//     if (!f) return false;
    
//     // Read ID3v2 header
//     unsigned char header[10];
//     if (fread(header, 1, 10, f) != 10) {
//         fclose(f);
//         return false;
//     }
    
//     // Check for ID3v2 tag
//     if (memcmp(header, "ID3", 3) != 0) {
//         fclose(f);
//         return false;
//     }
    
//     // Calculate tag size
//     uint32_t tag_size = ((header[6] & 0x7F) << 21) | 
//                        ((header[7] & 0x7F) << 14) | 
//                        ((header[8] & 0x7F) << 7) | 
//                        (header[9] & 0x7F);
    
//     // Look for APIC frame (album art)
//     unsigned char frame_header[10];
//     size_t pos = 10; // Start after ID3v2 header
    
//     while (pos < tag_size + 10) {
//         if (fread(frame_header, 1, 10, f) != 10) break;
        
//         // Check for APIC frame
//         if (memcmp(frame_header, "APIC", 4) == 0) {
//             uint32_t frame_size = (frame_header[4] << 24) | 
//                                  (frame_header[5] << 16) | 
//                                  (frame_header[6] << 8) | 
//                                  frame_header[7];
            
//             if (frame_size > 0 && frame_size < 500000) { // Reasonable size limit
//                 // Create album art filename
//                 const char* base_name = strrchr(filepath, '/');
//                 base_name = base_name ? base_name + 1 : filepath;
//                 snprintf(art_path, art_path_size, "%s/.album_art_%s.jpg", MOUNT_POINT, base_name);
                
//                 // Skip APIC frame data (encoding, MIME type, etc.) and extract image
//                 fseek(f, 1, SEEK_CUR); // Skip encoding
                
//                 // Skip MIME type (null terminated)
//                 int c;
//                 while ((c = fgetc(f)) != 0 && c != EOF);
                
//                 // Skip picture type and description
//                 fgetc(f); // Picture type
//                 while ((c = fgetc(f)) != 0 && c != EOF); // Description
                
//                 // Now we should be at the image data
//                 long img_start = ftell(f);
//                 long img_size = frame_size - (img_start - pos - 10);
                
//                 if (img_size > 0) {
//                     FILE* art_file = fopen(art_path, "wb");
//                     if (art_file) {
//                         unsigned char buffer[1024];
//                         size_t remaining = img_size;
                        
//                         while (remaining > 0) {
//                             size_t to_read = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
//                             size_t read_bytes = fread(buffer, 1, to_read, f);
//                             if (read_bytes == 0) break;
                            
//                             fwrite(buffer, 1, read_bytes, art_file);
//                             remaining -= read_bytes;
//                         }
                        
//                         fclose(art_file);
//                         fclose(f);
//                         return true;
//                     }
//                 }
//             }
//             break;
//         }
        
//         // Move to next frame
//         uint32_t frame_size = (frame_header[4] << 24) | 
//                              (frame_header[5] << 16) | 
//                              (frame_header[6] << 8) | 
//                              frame_header[7];
        
//         fseek(f, frame_size, SEEK_CUR);
//         pos += 10 + frame_size;
//     }
    
//     fclose(f);
//     return false;
// }

// // Generate placeholder album art for music files without embedded art
// static void generate_placeholder_art(const char* filename, char* art_path, size_t art_path_size) {
//     snprintf(art_path, art_path_size, "%s/.placeholder_art.svg", MOUNT_POINT);
    
//     // Create a simple SVG placeholder if it doesn't exist
//     FILE* f = fopen(art_path, "r");
//     if (!f) {
//         f = fopen(art_path, "w");
//         if (f) {
//             fprintf(f, 
//                 "<svg width=\"200\" height=\"200\" xmlns=\"http://www.w3.org/2000/svg\">"
//                 "<rect width=\"200\" height=\"200\" fill=\"#f0f0f0\" stroke=\"#ccc\"/>"
//                 "<circle cx=\"100\" cy=\"80\" r=\"30\" fill=\"#666\"/>"
//                 "<path d=\"M70 130 Q100 110 130 130 L130 160 Q100 180 70 160 Z\" fill=\"#666\"/>"
//                 "<text x=\"100\" y=\"190\" text-anchor=\"middle\" font-family=\"Arial\" font-size=\"12\" fill=\"#666\">Music File</text>"
//                 "</svg>");
//             fclose(f);
//         }
//     } else {
//         fclose(f);
//     }
// }

// // Function to send progress updates via WebSocket
// static void send_progress_update(const char* upload_type, const char* filename, int percent, uint32_t bytes_received, uint32_t total_bytes, float speed_kbps)
// {
//     if (ws_fd == -1) return;  // No WebSocket connection
    
//     char progress_msg[384];
//     snprintf(progress_msg, sizeof(progress_msg),
//         "{\"type\":\"progress\",\"upload_type\":\"%s\",\"filename\":\"%s\",\"percent\":%d,\"bytes_received\":%lu,\"total_bytes\":%lu,\"speed_kbps\":%.1f}",
//         upload_type, filename, percent, (unsigned long)bytes_received, (unsigned long)total_bytes, speed_kbps);
    
//     httpd_ws_frame_t ws_pkt;
//     memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
//     ws_pkt.payload = (uint8_t*)progress_msg;
//     ws_pkt.len = strlen(progress_msg);
//     ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
//     httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
// }

// // HTML templates
// static const char index_html[] = 
// "<!DOCTYPE html><html><head><title>SD Card Browser</title>"
// "<style>"
// "body{font-family:Arial;margin:20px;}"
// "table{border-collapse:collapse;width:100%%;}"
// "th,td{border:1px solid #ddd;padding:8px;text-align:left;vertical-align:middle;}"
// "th{background-color:#f2f2f2;}"
// ".upload{margin:20px 0;padding:20px;border:1px solid #ccc;border-radius:5px;}"
// ".progress-container{margin:10px 0;display:none;}"
// ".progress-bar{width:100%%;height:20px;background-color:#f0f0f0;border-radius:10px;overflow:hidden;}"
// ".progress-fill{height:100%%;background-color:#4CAF50;width:0%%;transition:width 0.3s;}"
// ".upload-status{margin:10px 0;font-weight:bold;}"
// ".ws-upload{background-color:#f9f9f9;}"
// ".music-file{background-color:#f0f8ff;}"
// ".album-art{width:40px;height:40px;object-fit:cover;border-radius:4px;margin-right:10px;}"
// ".file-info{display:flex;align-items:center;}"
// ".music-icon{color:#1e90ff;margin-right:5px;}"
// ".play-btn{background:#4CAF50;color:white;border:none;padding:4px 8px;border-radius:3px;cursor:pointer;font-size:12px;}"
// ".play-btn:hover{background:#45a049;}"
// ".modal{display:none;position:fixed;z-index:1000;left:0;top:0;width:100%%;height:100%%;background:rgba(0,0,0,0.5);}"
// ".modal-content{background:#fff;margin:5%% auto;padding:20px;border-radius:8px;max-width:500px;}"
// ".close{color:#aaa;float:right;font-size:28px;font-weight:bold;cursor:pointer;}"
// ".close:hover{color:#000;}"
// ".album-art-large{width:200px;height:200px;object-fit:cover;border-radius:8px;margin:10px auto;display:block;}"
// "</style></head><body>"
// "<h1>🎵 SD Card File Browser</h1>"

// "<div class='upload ws-upload'>"
// "<h3>🚀 WebSocket Upload (with Progress)</h3>"
// "<input type='file' id='wsFile' accept='*/*'>"
// "<button onclick='uploadWithWebSocket()'>Upload via WebSocket</button>"
// "<div class='progress-container' id='wsProgress'>"
// "<div class='progress-bar'><div class='progress-fill' id='wsProgressFill'></div></div>"
// "<div class='upload-status' id='wsStatus'>Ready</div>"
// "</div></div>"

// "<div class='upload'>"
// "<h3>📁 Traditional HTTP Upload</h3>"
// "<form action='/upload' method='post' enctype='multipart/form-data'>"
// "<input type='file' name='file' required>"
// "<input type='submit' value='Upload'>"
// "</form></div>"

// "<div class='upload'>"
// "<h3>🔗 Raw TCP Upload (Port %d) - Direct Socket Connection</h3>"
// "<input type='file' id='tcpFile' accept='*/*'>"
// "<button onclick='uploadViaTCP()'>Upload via Raw TCP</button>"
// "<div class='progress-container' id='tcpProgress'>"
// "<div class='progress-bar'><div class='progress-fill' id='tcpProgressFill'></div></div>"
// "<div class='upload-status' id='tcpStatus'>Ready</div>"
// "</div></div>"

// "<div class='upload'>"
// "<h3>📡 Raw UDP Upload (Port %d) - Direct Socket Connection</h3>"
// "<input type='file' id='udpFile' accept='*/*'>"
// "<button onclick='uploadViaUDP()'>Upload via Raw UDP</button>"
// "<div class='progress-container' id='udpProgress'>"
// "<div class='progress-bar'><div class='progress-fill' id='udpProgressFill'></div></div>"
// "<div class='upload-status' id='udpStatus'>Ready</div>"
// "</div>"
// "<p><small>Protocol: [4 bytes filename_len][4 bytes file_size][filename][file_data]</small></p>"
// "</div>"

// "<h3>🗂️ Files on SD Card</h3>"
// "<table><tr><th>Name</th><th>Size</th><th>Type</th><th>Actions</th></tr>"
// "%s</table>"

// "<!-- Music Player Modal -->"
// "<div id='musicModal' class='modal'>"
// "<div class='modal-content'>"
// "<span class='close' onclick='closeMusicModal()'>&times;</span>"
// "<h3 id='modalTitle'>Music Player</h3>"
// "<img id='modalAlbumArt' class='album-art-large' src='' alt='Album Art'>"
// "<audio id='audioPlayer' controls style='width:100%%;margin:10px 0;'>"
// "Your browser does not support the audio element."
// "</audio>"
// "<div style='text-align:center;margin:10px 0;'>"
// "<button onclick='closeMusicModal()' style='padding:8px 16px;background:#666;color:white;border:none;border-radius:4px;cursor:pointer;'>Close</button>"
// "</div>"
// "</div>"
// "</div>"

// "<script>"
// "let ws = null;"
// "let uploadFile = null;"
// "const CHUNK_SIZE = 64 * 1024;"

// "function connectWebSocket() {"
// "  if (ws && ws.readyState === WebSocket.OPEN) return Promise.resolve();"
// "  return new Promise((resolve, reject) => {"
// "    ws = new WebSocket('ws://' + window.location.host + '/ws');"
// "    ws.onopen = () => resolve();"
// "    ws.onerror = () => reject('WebSocket connection failed');"
// "    ws.onmessage = (event) => {"
// "      try {"
// "        const data = JSON.parse(event.data);"
// "        if (data.type === 'progress') {"
// "          const uploadType = data.upload_type || 'ws';"
// "          const speed = data.speed_kbps || 0;"
// "          if (uploadType === 'tcp') {"
// "            updateTCPProgress(data.percent, 'Uploading...', speed);"
// "          } else if (uploadType === 'udp') {"
// "            updateUDPProgress(data.percent, 'Uploading...', speed);"
// "          } else {"
// "            updateProgress(data.percent, 'Uploading...');"
// "          }"
// "        } else if (data.type === 'complete') {"
// "          const uploadType = data.upload_type || 'ws';"
// "          if (uploadType === 'tcp') {"
// "            updateTCPProgress(100, 'Upload completed successfully!', 0);"
// "          } else if (uploadType === 'udp') {"
// "            updateUDPProgress(100, 'Upload completed successfully!', 0);"
// "          } else {"
// "            updateProgress(100, 'Upload completed successfully!');"
// "          }"
// "          setTimeout(() => window.location.reload(), 1000);"
// "        } else if (data.type === 'error') {"
// "          const uploadType = data.upload_type || 'ws';"
// "          const errorMsg = 'Error: ' + data.message;"
// "          if (uploadType === 'tcp') {"
// "            updateTCPProgress(0, errorMsg, 0);"
// "          } else if (uploadType === 'udp') {"
// "            updateUDPProgress(0, errorMsg, 0);"
// "          } else {"
// "            updateProgress(0, errorMsg);"
// "          }"
// "        }"
// "      } catch(e) { console.error('Parse error:', e); }"
// "    };"
// "  });"
// "}"

// "function updateProgress(percent, status) {"
// "  document.getElementById('wsProgressFill').style.width = percent + '%%';"
// "  document.getElementById('wsStatus').textContent = status + ' (' + percent + '%%)'; "
// "}"

// "async function uploadWithWebSocket() {"
// "  const fileInput = document.getElementById('wsFile');"
// "  if (!fileInput.files[0]) {"
// "    alert('Please select a file first');"
// "    return;"
// "  }"
// "  uploadFile = fileInput.files[0];"
// "  try {"
// "    document.getElementById('wsProgress').style.display = 'block';"
// "    updateProgress(0, 'Connecting...');"
// "    await connectWebSocket();"
// "    updateProgress(0, 'Starting upload...');"
// "    const totalChunks = Math.ceil(uploadFile.size / CHUNK_SIZE);"
// "    for (let i = 0; i < totalChunks; i++) {"
// "      const start = i * CHUNK_SIZE;"
// "      const end = Math.min(start + CHUNK_SIZE, uploadFile.size);"
// "      const chunk = uploadFile.slice(start, end);"
// "      const isLast = (i === totalChunks - 1);"
// "      await sendChunk(chunk, i, totalChunks, uploadFile.name, uploadFile.size, isLast);"
// "      const percent = Math.round((i + 1) / totalChunks * 100);"
// "      updateProgress(percent, 'Uploading...');"
// "    }"
// "  } catch (error) {"
// "    updateProgress(0, 'Error: ' + error);"
// "  }"
// "}"

// "function sendChunk(chunk, index, total, filename, fileSize, isLast) {"
// "  return new Promise((resolve, reject) => {"
// "    const reader = new FileReader();"
// "    reader.onload = () => {"
// "      const header = JSON.stringify({"
// "        filename: filename,"
// "        chunkIndex: index,"
// "        totalChunks: total,"
// "        fileSize: fileSize,"
// "        isLast: isLast"
// "      }) + '\\n';"
// "      const headerBytes = new TextEncoder().encode(header);"
// "      const combined = new Uint8Array(headerBytes.length + chunk.size);"
// "      combined.set(headerBytes);"
// "      combined.set(new Uint8Array(reader.result), headerBytes.length);"
// "      ws.send(combined);"
// "      setTimeout(resolve, 10);"
// "    };"
// "    reader.onerror = reject;"
// "    reader.readAsArrayBuffer(chunk);"
// "  });"
// "}"

// "function updateTCPProgress(percent, status, speed) {"
// "  document.getElementById('tcpProgressFill').style.width = percent + '%%';"
// "  const speedText = speed ? ' - ' + speed.toFixed(1) + ' KB/s' : '';"
// "  document.getElementById('tcpStatus').textContent = status + ' (' + percent + '%%)' + speedText; "
// "}"

// "function updateUDPProgress(percent, status, speed) {"
// "  document.getElementById('udpProgressFill').style.width = percent + '%%';"
// "  const speedText = speed ? ' - ' + speed.toFixed(1) + ' KB/s' : '';"
// "  document.getElementById('udpStatus').textContent = status + ' (' + percent + '%%)' + speedText; "
// "}"

// "async function uploadViaTCP() {"
// "  const fileInput = document.getElementById('tcpFile');"
// "  if (!fileInput.files[0]) {"
// "    alert('Please select a file first');"
// "    return;"
// "  }"
// "  const file = fileInput.files[0];"
// "  const progressContainer = document.getElementById('tcpProgress');"
// "  progressContainer.style.display = 'block';"
// "  updateTCPProgress(0, 'Connecting to TCP server...', 0);"
// "  try {"
// "    await connectWebSocket();"
// "    updateTCPProgress(5, 'Preparing data...', 0);"
// "    const filename = file.name;"
// "    const fileSize = file.size;"
// "    const filenameBytes = new TextEncoder().encode(filename);"
// "    const filenameLen = filenameBytes.length;"
// "    const buffer = new ArrayBuffer(8 + filenameLen + fileSize);"
// "    const view = new DataView(buffer);"
// "    view.setUint32(0, filenameLen, false);"
// "    view.setUint32(4, fileSize, false);"
// "    const uint8View = new Uint8Array(buffer);"
// "    uint8View.set(filenameBytes, 8);"
// "    const fileBytes = new Uint8Array(await file.arrayBuffer());"
// "    uint8View.set(fileBytes, 8 + filenameLen);"
// "    updateTCPProgress(10, 'Connecting to TCP server...', 0);"
// "    const startTime = Date.now();"
// "    const response = await fetch('/upload_tcp_raw', {"
// "      method: 'POST',"
// "      headers: {"
// "        'Content-Type': 'application/octet-stream',"
// "        'X-Filename': filename,"
// "        'X-File-Size': fileSize.toString(),"
// "        'X-Protocol': 'tcp-raw'"
// "      },"
// "      body: buffer"
// "    });"
// "    const elapsed = (Date.now() - startTime) / 1000;"
// "    const speedKbps = (fileSize / 1024) / elapsed;"
// "    if (response.ok) {"
// "      updateTCPProgress(100, 'TCP upload completed successfully!', speedKbps);"
// "      setTimeout(() => window.location.reload(), 1000);"
// "    } else {"
// "      updateTCPProgress(0, 'TCP upload failed: ' + response.statusText, 0);"
// "    }"
// "  } catch (error) {"
// "    updateTCPProgress(0, 'TCP upload error: ' + error.message, 0);"
// "  }"
// "}"

// "async function uploadViaUDP() {"
// "  const fileInput = document.getElementById('udpFile');"
// "  if (!fileInput.files[0]) {"
// "    alert('Please select a file first');"
// "    return;"
// "  }"
// "  const file = fileInput.files[0];"
// "  const progressContainer = document.getElementById('udpProgress');"
// "  progressContainer.style.display = 'block';"
// "  updateUDPProgress(0, 'Preparing UDP data...', 0);"
// "  try {"
// "    await connectWebSocket();"
// "    updateUDPProgress(5, 'Preparing data...', 0);"
// "    const filename = file.name;"
// "    const fileSize = file.size;"
// "    const filenameBytes = new TextEncoder().encode(filename);"
// "    const filenameLen = filenameBytes.length;"
// "    const buffer = new ArrayBuffer(8 + filenameLen + fileSize);"
// "    const view = new DataView(buffer);"
// "    view.setUint32(0, filenameLen, false);"
// "    view.setUint32(4, fileSize, false);"
// "    const uint8View = new Uint8Array(buffer);"
// "    uint8View.set(filenameBytes, 8);"
// "    const fileBytes = new Uint8Array(await file.arrayBuffer());"
// "    uint8View.set(fileBytes, 8 + filenameLen);"
// "    updateUDPProgress(10, 'Sending to UDP server...', 0);"
// "    const startTime = Date.now();"
// "    const response = await fetch('/upload_udp_raw', {"
// "      method: 'POST',"
// "      headers: {"
// "        'Content-Type': 'application/octet-stream',"
// "        'X-Filename': filename,"
// "        'X-File-Size': fileSize.toString(),"
// "        'X-Protocol': 'udp-raw'"
// "      },"
// "      body: buffer"
// "    });"
// "    const elapsed = (Date.now() - startTime) / 1000;"
// "    const speedKbps = (fileSize / 1024) / elapsed;"
// "    if (response.ok) {"
// "      updateUDPProgress(100, 'UDP upload completed successfully!', speedKbps);"
// "      setTimeout(() => window.location.reload(), 1000);"
// "    } else {"
// "      updateUDPProgress(0, 'UDP upload failed: ' + response.statusText, 0);"
// "    }"
// "  } catch (error) {"
// "    updateUDPProgress(0, 'UDP upload error: ' + error.message, 0);"
// "  }"
// "}"

// "function playMusic(filename) {"
// "  const modal = document.getElementById('musicModal');"
// "  const audioPlayer = document.getElementById('audioPlayer');"
// "  const modalTitle = document.getElementById('modalTitle');"
// "  const modalAlbumArt = document.getElementById('modalAlbumArt');"
// "  "
// "  modalTitle.textContent = 'Playing: ' + filename;"
// "  audioPlayer.src = '/download?file=' + encodeURIComponent(filename);"
// "  modalAlbumArt.src = '/album_art?file=' + encodeURIComponent(filename);"
// "  modalAlbumArt.onerror = function() { this.style.display = 'none'; };"
// "  "
// "  modal.style.display = 'block';"
// "  "
// "  audioPlayer.load();"
// "  audioPlayer.play().catch(e => console.log('Autoplay prevented:', e));"
// "}"

// "function closeMusicModal() {"
// "  const modal = document.getElementById('musicModal');"
// "  const audioPlayer = document.getElementById('audioPlayer');"
// "  "
// "  modal.style.display = 'none';"
// "  audioPlayer.pause();"
// "  audioPlayer.currentTime = 0;"
// "}"

// "window.onclick = function(event) {"
// "  const modal = document.getElementById('musicModal');"
// "  if (event.target === modal) {"
// "    closeMusicModal();"
// "  }"
// "}"
// "</script>"
// "</body></html>";

// // WiFi event handler
// static void wifi_event_handler(void* arg, esp_event_base_t event_base,
//                               int32_t event_id, void* event_data)
// {
//     if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
//         esp_wifi_connect();
//     } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
//         esp_wifi_connect();
//         ESP_LOGI(TAG, "retry to connect to the AP");
//     } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
//         ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
//         ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
//     }
// }

// // Initialize WiFi
// static void wifi_init_sta(void)
// {
//     esp_netif_init();
//     esp_event_loop_create_default();
//     esp_netif_create_default_wifi_sta();

//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     esp_wifi_init(&cfg);

//     esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
//     esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

//     wifi_config_t wifi_config = {
//         .sta = {
//             .ssid = WIFI_SSID,
//             .password = WIFI_PASS,
//         },
//     };
//     esp_wifi_set_mode(WIFI_MODE_STA);
//     esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
//     esp_wifi_start();
// }

// // Initialize SD card
// static esp_err_t init_sd_card(void)
// {
//     ESP_LOGI("main", "🗂️ Initializing SD card via SPI...");
//     ESP_LOGI("main", "🔧 Function entry - starting SPI configuration...");
    
//     // SD card SPI configuration
//     spi_bus_config_t bus_cfg = {
//         .mosi_io_num = GPIO_NUM_11,  // MOSI
//         .miso_io_num = GPIO_NUM_13,  // MISO  
//         .sclk_io_num = GPIO_NUM_12,  // SCLK
//         .quadwp_io_num = -1,
//         .quadhd_io_num = -1,
//         .max_transfer_sz = 4000,
//     };
    
//     // Initialize SPI bus
//     esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
//     ESP_LOGI("main", "🔧 SPI bus initialize result: %s", esp_err_to_name(ret));
//     if (ret != ESP_OK) {
//         ESP_LOGE("main", "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
//         return ret;
//     }
//     ESP_LOGI("main", "✅ SPI bus initialized successfully");
    
//     // SD card host configuration
//     sdmmc_host_t host = SDSPI_HOST_DEFAULT();
//     host.slot = SPI2_HOST;
    
//     sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
//     slot_config.gpio_cs = GPIO_NUM_10;  // Chip Select
//     slot_config.host_id = SPI2_HOST;
    
//     // Mount configuration with Long Filename Support
//     esp_vfs_fat_sdmmc_mount_config_t mount_config = {
//         .format_if_mount_failed = false,
//         .max_files = 20,  // Increase max files for better LFN support
//         .allocation_unit_size = 64 * 1024,  // Use larger allocation unit like ESP-ADF
//         .disk_status_check_enable = false   // Disable status check for better compatibility
//     };
    
//     // Mount the SD card
//     sdmmc_card_t *card;
//     ESP_LOGI("main", "🔧 Attempting to mount SD card...");
//     ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
//     ESP_LOGI("main", "🔧 Mount result: %s", esp_err_to_name(ret));
    
//     if (ret != ESP_OK) {
//         if (ret == ESP_FAIL) {
//             ESP_LOGE("main", "Failed to mount filesystem. Check if SD card is inserted and formatted.");
//         } else {
//             ESP_LOGE("main", "Failed to initialize SD card: %s", esp_err_to_name(ret));
//         }
//         // Don't return error - allow system to continue without SD card
//         ESP_LOGW("main", "⚠️ SD card not available - system will continue without SD card support");
//         return ESP_OK;
//     }
    
//     // Print card info
//     ESP_LOGI("SDCARD", "✅ SD card mounted successfully");
//     ESP_LOGI("SDCARD", "📊 SD Card Info:");
//     ESP_LOGI("SDCARD", "   Name: %s", card->cid.name);
//     ESP_LOGI("SDCARD", "   Speed: %s", (card->csd.tr_speed > 25000000) ? "high speed" : "default speed");
//     ESP_LOGI("SDCARD", "   Size: %lluMB", ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    
//     return ESP_OK;
// }

// // TCP Server Task
// static void tcp_server_task(void *pvParameters)
// {
//     char addr_str[128];
//     int addr_family = AF_INET;
//     int ip_protocol = IPPROTO_IP;
//     int keepAlive = 1;
//     int keepIdle = 7200;
//     int keepInterval = 75;
//     int keepCount = 9;

//     struct sockaddr_storage dest_addr;
//     struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
//     dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
//     dest_addr_ip4->sin_family = AF_INET;
//     dest_addr_ip4->sin_port = htons(TCP_PORT);

//     int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
//     if (listen_sock < 0) {
//         ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
//         vTaskDelete(NULL);
//         return;
//     }

//     int opt = 1;
//     setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

//     ESP_LOGI(TAG, "Socket created");

//     int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
//     if (err != 0) {
//         ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
//         goto CLEAN_UP;
//     }
//     ESP_LOGI(TAG, "Socket bound, port %d", TCP_PORT);

//     err = listen(listen_sock, 1);
//     if (err != 0) {
//         ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
//         goto CLEAN_UP;
//     }

//     while (1) {
//         ESP_LOGI(TAG, "Socket listening");

//         struct sockaddr_storage source_addr;
//         socklen_t addr_len = sizeof(source_addr);
//         int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
//         if (sock < 0) {
//             ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
//             break;
//         }

//         // Set socket keepalive option
//         setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
//         setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
//         setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
//         setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

//         inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
//         ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

//         // Receive file transfer header (8 bytes: 4 for filename_len, 4 for file_size)
//         uint32_t header_data[2];
//         int len = recv(sock, header_data, 8, 0);
//         if (len > 0) {
//             uint32_t filename_len = ntohl(header_data[0]);
//             uint32_t file_size = ntohl(header_data[1]);
            
//             ESP_LOGI(TAG, "TCP: Receiving file, name_len: %d, size: %d", filename_len, file_size);

//             if (filename_len < 256 && file_size < MAX_FILE_SIZE) {
//                 char *filename = malloc(filename_len + 1);
//                 char *buffer = malloc(4096);
                
//                 // Receive filename
//                 recv(sock, filename, filename_len, 0);
//                 filename[filename_len] = '\0';
                
//                 char filepath[256];
//                 snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
                
//                 FILE *f = fopen(filepath, "wb");
//                 if (f) {
//                     uint32_t remaining = file_size;
//                     while (remaining > 0) {
//                         int to_recv = (remaining > 4096) ? 4096 : remaining;
//                         len = recv(sock, buffer, to_recv, 0);
//                         if (len > 0) {
//                             fwrite(buffer, 1, len, f);
//                             remaining -= len;
//                         } else {
//                             break;
//                         }
//                     }
//                     fclose(f);
//                     ESP_LOGI(TAG, "TCP: File saved: %s", filename);
//                     send(sock, "OK", 2, 0);
//                 } else {
//                     ESP_LOGE(TAG, "TCP: Failed to create file");
//                     send(sock, "ERROR", 5, 0);
//                 }
                
//                 free(filename);
//                 free(buffer);
//             }
//         }

//         shutdown(sock, 0);
//         close(sock);
//     }

// CLEAN_UP:
//     close(listen_sock);
//     vTaskDelete(NULL);
// }

// // UDP Server Task
// static void udp_server_task(void *pvParameters)
// {
//     char addr_str[128];
//     int addr_family = AF_INET;
//     int ip_protocol = IPPROTO_IP;

//     struct sockaddr_storage dest_addr;
//     struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
//     dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
//     dest_addr_ip4->sin_family = AF_INET;
//     dest_addr_ip4->sin_port = htons(UDP_PORT);

//     int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
//     if (sock < 0) {
//         ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
//         vTaskDelete(NULL);
//         return;
//     }

//     int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
//     if (err < 0) {
//         ESP_LOGE(TAG, "UDP Socket unable to bind: errno %d", errno);
//         close(sock);
//         vTaskDelete(NULL);
//         return;
//     }

//     ESP_LOGI(TAG, "UDP Socket bound, port %d", UDP_PORT);

//     struct sockaddr_storage source_addr;
//     socklen_t socklen = sizeof(source_addr);
//     char *rx_buffer = malloc(65536);

//     while (1) {
//         ESP_LOGI(TAG, "Waiting for UDP data");
//         int len = recvfrom(sock, rx_buffer, 65536, 0, (struct sockaddr *)&source_addr, &socklen);

//         if (len < 0) {
//             ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
//             break;
//         } else if (len >= 8) {  // At least 8 bytes for header
//             inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
//             ESP_LOGI(TAG, "Received UDP data from %s", addr_str);

//             uint32_t *header_data = (uint32_t *)rx_buffer;
//             uint32_t filename_len = ntohl(header_data[0]);
//             uint32_t file_size = ntohl(header_data[1]);

//             ESP_LOGI(TAG, "UDP: Receiving file, name_len: %d, size: %d", filename_len, file_size);

//             if (filename_len < 256 && file_size < MAX_FILE_SIZE && 
//                 len >= 8 + filename_len + file_size) {
                
//                 char *data_ptr = rx_buffer + 8;  // Skip 8-byte header
//                 char *filename = malloc(filename_len + 1);
//                 memcpy(filename, data_ptr, filename_len);
//                 filename[filename_len] = '\0';

//                 char filepath[256];
//                 snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);

//                 FILE *f = fopen(filepath, "wb");
//                 if (f) {
//                     char *file_data = data_ptr + filename_len;
//                     fwrite(file_data, 1, file_size, f);
//                     fclose(f);
//                     ESP_LOGI(TAG, "UDP: File saved: %s", filename);
                    
//                     // Send response
//                     sendto(sock, "OK", 2, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
//                 } else {
//                     ESP_LOGE(TAG, "UDP: Failed to create file");
//                     sendto(sock, "ERROR", 5, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
//                 }
                
//                 free(filename);
//             } else {
//                 ESP_LOGE(TAG, "UDP: Invalid packet size or parameters");
//                 sendto(sock, "ERROR", 5, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
//             }
//         }
//     }

//     free(rx_buffer);
//     close(sock);
//     vTaskDelete(NULL);
// }

// // Generate file list HTML
// static char* generate_file_list(void)
// {
//     DIR *dir = opendir(MOUNT_POINT);
//     if (!dir) return strdup("");

//     char *list = malloc(4096);  // Reduced from 8192
//     if (!list) return strdup("");
//     list[0] = '\0';
    
//     struct dirent *entry;
//     struct stat file_stat;
//     char *filepath = malloc(512);  // Move to heap
//     if (!filepath) {
//         free(list);
//         return strdup("");
//     }

//     while ((entry = readdir(dir)) != NULL) {
//         if (entry->d_name[0] == '.') continue;
        
//         snprintf(filepath, 512, "%s/%s", MOUNT_POINT, entry->d_name);
//         if (stat(filepath, &file_stat) == 0) {
//             char *row = malloc(1024);  // Increase size for Play button
//             if (row) {
//                 if (is_music_file(entry->d_name)) {
//                     // Music file with album art thumbnail and play button
//                     snprintf(row, 2048,
//                         "<tr class='music-file'><td><div style='display:flex;align-items:center;'>"
//                         "<img src='/album_art?file=%.100s' alt='Album Art' "
//                         "class='album-art' onerror='this.style.display=\"none\"'>"
//                         "<div><div style='font-weight:bold;'>%.35s</div>"
//                         "<div style='font-size:0.8em;color:#666;'>🎵 Music File</div></div>"
//                         "</div></td><td>%ld bytes</td><td>Audio</td>"
//                         "<td><button class='play-btn' onclick='playMusic(\"%.100s\")'>▶ Play</button> | "
//                         "<a href='/download?file=%.100s'>Download</a> | "
//                         "<a href='/delete?file=%.100s'>Delete</a></td></tr>",
//                         entry->d_name, entry->d_name, file_stat.st_size,
//                         entry->d_name, entry->d_name, entry->d_name);
//                 } else {
//                     // Regular file
//                     snprintf(row, 2048,
//                         "<tr><td>%.50s</td><td>%ld bytes</td><td>%s</td>"
//                         "<td><a href='/download?file=%.50s'>Download</a> | "
//                         "<a href='/delete?file=%.50s'>Delete</a></td></tr>",
//                         entry->d_name, file_stat.st_size,
//                         S_ISDIR(file_stat.st_mode) ? "Directory" : "File",
//                         entry->d_name, entry->d_name);
//                 }
                
//                 // Check if we have space to add this row
//                 if (strlen(list) + strlen(row) < 4000) {  // Leave some buffer
//                     strcat(list, row);
//                 }
//                 free(row);
                
//                 // Stop if we're getting too long
//                 if (strlen(list) > 3500) break;
//             }
//         }
//     }
    
//     free(filepath);
//     closedir(dir);
//     return list;
// }

// // HTTP handlers
// static esp_err_t index_handler(httpd_req_t *req)
// {
//     char *file_list = generate_file_list();
//     if (!file_list) {
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
//         return ESP_FAIL;
//     }
    
//     // Calculate needed size more precisely
//     size_t response_len = strlen(index_html) + strlen(file_list) + 100;
//     char *response = malloc(response_len);
//     if (!response) {
//         free(file_list);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
//         return ESP_FAIL;
//     }
    
//     snprintf(response, response_len, index_html, TCP_PORT, UDP_PORT, file_list);
    
//     esp_err_t ret = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    
//     free(file_list);
//     free(response);
//     return ret;
// }

// static esp_err_t upload_handler(httpd_req_t *req)
// {
//     char *boundary = malloc(100);  // Move to heap
//     char *filepath = malloc(256);  // Move to heap
//     char *filename = malloc(128);  // Move to heap
//     FILE *f = NULL;
    
//     if (!boundary || !filepath || !filename) {
//         free(boundary);
//         free(filepath);
//         free(filename);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
//         return ESP_FAIL;
//     }
    
//     memset(filename, 0, 128);
    
//     // Parse multipart boundary
//     char *ct = NULL;
//     size_t ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
//     if (ct_len) {
//         ct = malloc(ct_len + 1);
//         if (ct) {
//             httpd_req_get_hdr_value_str(req, "Content-Type", ct, ct_len + 1);
//             char *boundary_start = strstr(ct, "boundary=");
//             if (boundary_start) {
//                 strncpy(boundary, boundary_start + 9, 99);
//                 boundary[99] = '\0';
//             }
//             free(ct);
//         }
//     }

//     char *buf = malloc(512);  // Smaller buffer
//     if (!buf) {
//         free(boundary);
//         free(filepath);
//         free(filename);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
//         return ESP_FAIL;
//     }
    
//     int remaining = req->content_len;
//     bool headers_done = false;
    
//     while (remaining > 0) {
//         int recv_len = httpd_req_recv(req, buf, MIN(remaining, 512));
//         if (recv_len <= 0) break;
        
//         if (!headers_done) {
//             // Parse headers to get filename
//             char *filename_start = strstr(buf, "filename=\"");
//             if (filename_start) {
//                 filename_start += 10;
//                 char *filename_end = strchr(filename_start, '"');
//                 if (filename_end) {
//                     int len = MIN(filename_end - filename_start, 127);
//                     strncpy(filename, filename_start, len);
//                     filename[len] = '\0';
                    
//                     snprintf(filepath, 256, "%s/%s", MOUNT_POINT, filename);
//                     f = fopen(filepath, "wb");
//                     if (!f) {
//                         free(boundary);
//                         free(filepath);
//                         free(filename);
//                         free(buf);
//                         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
//                         return ESP_FAIL;
//                     }
//                 }
//             }
            
//             // Skip to file data (after \r\n\r\n)
//             char *data_start = strstr(buf, "\r\n\r\n");
//             if (data_start) {
//                 data_start += 4;
//                 headers_done = true;
//                 int data_len = recv_len - (data_start - buf);
//                 if (f && data_len > 0) {
//                     fwrite(data_start, 1, data_len, f);
//                 }
//             }
//         } else if (f) {
//             // Write file data, but check for boundary end
//             char *boundary_end = strstr(buf, boundary);
//             if (boundary_end) {
//                 fwrite(buf, 1, boundary_end - buf - 2, f); // -2 for \r\n before boundary
//                 break;
//             } else {
//                 fwrite(buf, 1, recv_len, f);
//             }
//         }
        
//         remaining -= recv_len;
//     }
    
//     if (f) fclose(f);
    
//     free(boundary);
//     free(filepath);
//     free(buf);
    
//     httpd_resp_set_status(req, "303 See Other");
//     httpd_resp_set_hdr(req, "Location", "/");
//     httpd_resp_send(req, NULL, 0);
    
//     ESP_LOGI(TAG, "File uploaded: %s", filename);
//     free(filename);
//     return ESP_OK;
// }

// static esp_err_t download_handler(httpd_req_t *req)
// {
//     char query[256];
//     if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
//         char *file_param = strstr(query, "file=");
//         if (file_param) {
//             file_param += 5;
            
//             // URL decode the filename
//             char decoded_filename[128];
//             size_t decoded_len = 0;
//             for (size_t i = 0; file_param[i] && decoded_len < sizeof(decoded_filename) - 1; i++) {
//                 if (file_param[i] == '%' && file_param[i+1] && file_param[i+2]) {
//                     // Simple URL decode for common characters
//                     if (strncmp(&file_param[i], "%20", 3) == 0) {
//                         decoded_filename[decoded_len++] = ' ';
//                         i += 2;
//                     } else if (strncmp(&file_param[i], "%2E", 3) == 0) {
//                         decoded_filename[decoded_len++] = '.';
//                         i += 2;
//                     } else if (strncmp(&file_param[i], "%2D", 3) == 0) {
//                         decoded_filename[decoded_len++] = '-';
//                         i += 2;
//                     } else {
//                         decoded_filename[decoded_len++] = file_param[i];
//                     }
//                 } else if (file_param[i] == '&') {
//                     break; // End of parameter
//                 } else {
//                     decoded_filename[decoded_len++] = file_param[i];
//                 }
//             }
//             decoded_filename[decoded_len] = '\0';
            
//             ESP_LOGI(TAG, "Download request: raw='%s', decoded='%s'", file_param, decoded_filename);
            
//             char filepath[256];
//             snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, decoded_filename);
            
//             ESP_LOGI(TAG, "Trying to open file: %s", filepath);
            
//             FILE *f = fopen(filepath, "rb");
//             if (f) {
//                 ESP_LOGI(TAG, "File opened successfully: %s", decoded_filename);
//                 httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
                
//                 char buffer[1024];
//                 size_t read_bytes;
//                 while ((read_bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
//                     httpd_resp_send_chunk(req, buffer, read_bytes);
//                 }
//                 httpd_resp_send_chunk(req, NULL, 0);
//                 fclose(f);
//                 return ESP_OK;
//             } else {
//                 ESP_LOGE(TAG, "Failed to open file: %s", filepath);
//             }
//         }
//     }
    
//     httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
//     return ESP_FAIL;
// }

// static esp_err_t delete_handler(httpd_req_t *req)
// {
//     char query[128];
//     if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
//         char *file_param = strstr(query, "file=");
//         if (file_param) {
//             file_param += 5;
//             char filepath[256];
//             snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, file_param);
            
//             if (unlink(filepath) == 0) {
//                 ESP_LOGI(TAG, "File deleted: %s", file_param);
//             }
//         }
//     }
    
//     httpd_resp_set_status(req, "303 See Other");
//     httpd_resp_set_hdr(req, "Location", "/");
//     httpd_resp_send(req, NULL, 0);
//     return ESP_OK;
// }

// // Album art handler
// static esp_err_t album_art_handler(httpd_req_t *req)
// {
//     char query[256];
//     ESP_LOGI(TAG, "Album art handler called");
    
//     if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
//         ESP_LOGI(TAG, "Query string: %s", query);
//         char *file_param = strstr(query, "file=");
//         if (file_param) {
//             file_param += 5;
            
//             // URL decode the filename
//             char decoded_filename[128];
//             size_t decoded_len = 0;
//             for (size_t i = 0; file_param[i] && decoded_len < sizeof(decoded_filename) - 1; i++) {
//                 if (file_param[i] == '%' && file_param[i+1] && file_param[i+2]) {
//                     // Simple URL decode for common characters
//                     if (strncmp(&file_param[i], "%20", 3) == 0) {
//                         decoded_filename[decoded_len++] = ' ';
//                         i += 2;
//                     } else if (strncmp(&file_param[i], "%2E", 3) == 0) {
//                         decoded_filename[decoded_len++] = '.';
//                         i += 2;
//                     } else if (strncmp(&file_param[i], "%2D", 3) == 0) {
//                         decoded_filename[decoded_len++] = '-';
//                         i += 2;
//                     } else {
//                         decoded_filename[decoded_len++] = file_param[i];
//                     }
//                 } else if (file_param[i] == '&') {
//                     break; // End of parameter
//                 } else {
//                     decoded_filename[decoded_len++] = file_param[i];
//                 }
//             }
//             decoded_filename[decoded_len] = '\0';
            
//             ESP_LOGI(TAG, "Album art request: raw='%s', decoded='%s'", file_param, decoded_filename);
            
//             // Debug: List files in SD card directory
//             DIR *dir = opendir(MOUNT_POINT);
//             if (dir) {
//                 ESP_LOGI(TAG, "Files in %s:", MOUNT_POINT);
//                 struct dirent *entry;
//                 int file_count = 0;
//                 while ((entry = readdir(dir)) != NULL && file_count < 10) {
//                     ESP_LOGI(TAG, "  - %s", entry->d_name);
//                     file_count++;
//                 }
//                 closedir(dir);
//             } else {
//                 ESP_LOGE(TAG, "Failed to open directory: %s", MOUNT_POINT);
//             }
            
//             char filepath[256];
//             snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, decoded_filename);
            
//             ESP_LOGI(TAG, "Album art request for: %s", decoded_filename);
//             ESP_LOGI(TAG, "File path: %s", filepath);
            
//             // Check if it's a music file
//             if (is_music_file(decoded_filename)) {
//                 ESP_LOGI(TAG, "File identified as music file");
//                 char art_path[300];
//                 bool has_art = false;
                
//                 // Try to extract album art from MP3
//                 if (strstr(decoded_filename, ".mp3") || strstr(decoded_filename, ".MP3")) {
//                     ESP_LOGI(TAG, "Attempting MP3 album art extraction");
//                     has_art = extract_mp3_album_art(filepath, art_path, sizeof(art_path));
//                     ESP_LOGI(TAG, "MP3 album art extraction result: %s", has_art ? "success" : "failed");
//                 }
                
//                 // If no embedded art found, use placeholder
//                 if (!has_art) {
//                     ESP_LOGI(TAG, "Generating placeholder art");
//                     generate_placeholder_art(decoded_filename, art_path, sizeof(art_path));
//                     has_art = true;
//                     ESP_LOGI(TAG, "Placeholder art path: %s", art_path);
//                 }
                
//                 if (has_art) {
//                     ESP_LOGI(TAG, "Trying to open art file: %s", art_path);
//                     FILE *f = fopen(art_path, "rb");
//                     if (f) {
//                         ESP_LOGI(TAG, "Successfully opened art file");
//                         // Set appropriate content type
//                         if (strstr(art_path, ".jpg") || strstr(art_path, ".jpeg")) {
//                             httpd_resp_set_type(req, "image/jpeg");
//                         } else if (strstr(art_path, ".png")) {
//                             httpd_resp_set_type(req, "image/png");
//                         } else if (strstr(art_path, ".svg")) {
//                             httpd_resp_set_type(req, "image/svg+xml");
//                         } else {
//                             httpd_resp_set_type(req, "image/jpeg");
//                         }
                        
//                         // Set cache headers
//                         httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
                        
//                         // Send file data
//                         char buffer[1024];
//                         size_t read_bytes;
//                         while ((read_bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
//                             httpd_resp_send_chunk(req, buffer, read_bytes);
//                         }
//                         httpd_resp_send_chunk(req, NULL, 0);
//                         fclose(f);
//                         return ESP_OK;
//                     } else {
//                         ESP_LOGE(TAG, "Failed to open art file: %s", art_path);
//                     }
//                 } else {
//                     ESP_LOGE(TAG, "No album art available");
//                 }
//             } else {
//                 ESP_LOGI(TAG, "File is not a music file: %s", decoded_filename);
//             }
//         }
//     }
    
//     httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Album art not found");
//     return ESP_FAIL;
// }

// // WebSocket upload handler
// static esp_err_t ws_handler(httpd_req_t *req)
// {
//     if (req->method == HTTP_GET) {
//         ESP_LOGI(TAG, "WebSocket handshake");
//         ws_fd = httpd_req_to_sockfd(req);  // Store WebSocket connection for progress updates
//         return ESP_OK;
//     }

//     // Handle WebSocket frames
//     httpd_ws_frame_t ws_pkt;
//     uint8_t *buf = NULL;
//     memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

//     // Get frame info
//     esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
//         return ret;
//     }

//     if (ws_pkt.len) {
//         buf = malloc(ws_pkt.len + 1);
//         if (buf == NULL) {
//             ESP_LOGE(TAG, "Failed to malloc memory for buf");
//             return ESP_ERR_NO_MEM;
//         }
//         ws_pkt.payload = buf;
        
//         ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
//         if (ret != ESP_OK) {
//             ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
//             free(buf);
//             return ret;
//         }
//     }

//     // Process binary upload data
//     if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
//         static FILE *upload_file = NULL;
//         static char upload_filename[256] = {0};
//         static uint32_t expected_size = 0;
//         static uint32_t received_size = 0;
//         static uint32_t current_chunk = 0;
//         static uint32_t total_chunks = 0;
        
//         // Parse JSON header
//         char *data = (char*)ws_pkt.payload;
//         char *newline = strchr(data, '\n');
//         if (newline) {
//             *newline = '\0';
//             char *json_str = data;
//             char *binary_data = newline + 1;
//             size_t binary_len = ws_pkt.len - (binary_data - data);
            
//             // Simple JSON parsing for our specific format
//             char *filename_start = strstr(json_str, "\"filename\":\"");
//             char *chunk_start = strstr(json_str, "\"chunkIndex\":");
//             char *total_start = strstr(json_str, "\"totalChunks\":");
//             char *size_start = strstr(json_str, "\"fileSize\":");
//             char *last_start = strstr(json_str, "\"isLast\":");
            
//             if (filename_start && chunk_start && total_start && size_start) {
//                 // Extract filename
//                 filename_start += 12; // Skip "filename":"
//                 char *filename_end = strchr(filename_start, '"');
//                 if (filename_end) {
//                     size_t name_len = filename_end - filename_start;
//                     if (name_len < sizeof(upload_filename)) {
//                         strncpy(upload_filename, filename_start, name_len);
//                         upload_filename[name_len] = '\0';
//                     }
//                 }
                
//                 // Extract chunk info
//                 current_chunk = atoi(chunk_start + 13);
//                 total_chunks = atoi(total_start + 14);
//                 expected_size = atoi(size_start + 11);
//                 bool is_last = (last_start && strstr(last_start + 9, "true"));
                
//                 // Open file on first chunk
//                 if (current_chunk == 0) {
//                     char filepath[300];
//                     snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, upload_filename);
//                     upload_file = fopen(filepath, "wb");
//                     received_size = 0;
                    
//                     if (!upload_file) {
//                         ESP_LOGE(TAG, "Failed to create file: %s", upload_filename);
//                         // Send error response
//                         char error_msg[] = "{\"type\":\"error\",\"message\":\"Failed to create file\"}";
//                         httpd_ws_frame_t ws_resp = {
//                             .final = true,
//                             .fragmented = false,
//                             .type = HTTPD_WS_TYPE_TEXT,
//                             .payload = (uint8_t*)error_msg,
//                             .len = strlen(error_msg)
//                         };
//                         httpd_ws_send_frame(req, &ws_resp);
//                         free(buf);
//                         return ESP_FAIL;
//                     }
//                     ESP_LOGI(TAG, "Started receiving file: %s, size: %d, chunks: %d", 
//                             upload_filename, expected_size, total_chunks);
//                 }
                
//                 // Write chunk data
//                 if (upload_file && binary_len > 0) {
//                     size_t written = fwrite(binary_data, 1, binary_len, upload_file);
//                     received_size += written;
                    
//                     // Send progress update
//                     char progress_msg[200];
//                     uint32_t percent = (received_size * 100) / expected_size;
//                     snprintf(progress_msg, sizeof(progress_msg), 
//                             "{\"type\":\"progress\",\"percent\":%" PRIu32 ",\"status\":\"Uploading\"}", 
//                             percent);
                    
//                     httpd_ws_frame_t ws_resp = {
//                         .final = true,
//                         .fragmented = false,
//                         .type = HTTPD_WS_TYPE_TEXT,
//                         .payload = (uint8_t*)progress_msg,
//                         .len = strlen(progress_msg)
//                     };
//                     httpd_ws_send_frame(req, &ws_resp);
                    
//                     ESP_LOGI(TAG, "Chunk %d/%d received, %d bytes, total: %d/%d", 
//                             current_chunk + 1, total_chunks, written, received_size, expected_size);
//                 }
                
//                 // Close file on last chunk
//                 if (is_last && upload_file) {
//                     fclose(upload_file);
//                     upload_file = NULL;
                    
//                     ESP_LOGI(TAG, "File upload completed: %s (%d bytes)", upload_filename, received_size);
                    
//                     // Send completion response
//                     char complete_msg[] = "{\"type\":\"complete\",\"message\":\"Upload completed successfully\"}";
//                     httpd_ws_frame_t ws_resp = {
//                         .final = true,
//                         .fragmented = false,
//                         .type = HTTPD_WS_TYPE_TEXT,
//                         .payload = (uint8_t*)complete_msg,
//                         .len = strlen(complete_msg)
//                     };
//                     httpd_ws_send_frame(req, &ws_resp);
//                 }
//             }
//         }
//     }

//     if (buf) {
//         free(buf);
//     }
//     return ESP_OK;
// }

// // TCP Upload Handler
// static esp_err_t tcp_upload_handler(httpd_req_t *req)
// {
//     char filename[256] = {0};
//     char filesize_str[32] = {0};
    
//     // Get filename from header
//     size_t filename_len = httpd_req_get_hdr_value_len(req, "X-Filename");
//     if (filename_len > 0 && filename_len < sizeof(filename)) {
//         httpd_req_get_hdr_value_str(req, "X-Filename", filename, filename_len + 1);
//     }
    
//     // Get file size from header
//     size_t filesize_hdr_len = httpd_req_get_hdr_value_len(req, "X-File-Size");
//     if (filesize_hdr_len > 0 && filesize_hdr_len < sizeof(filesize_str)) {
//         httpd_req_get_hdr_value_str(req, "X-File-Size", filesize_str, filesize_hdr_len + 1);
//     }
    
//     if (strlen(filename) == 0) {
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename header");
//         return ESP_FAIL;
//     }
    
//     uint32_t file_size = atoi(filesize_str);
//     ESP_LOGI(TAG, "TCP Upload: %s (%d bytes)", filename, file_size);
    
//     // Create file path
//     char filepath[300];
//     snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
    
//     FILE *f = fopen(filepath, "wb");
//     if (!f) {
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
//         return ESP_FAIL;
//     }
    
//     // Read and write file data with progress updates and speed calculation
//     char *buffer = malloc(8192*2);  // 8KB buffer for better speed
//     int remaining = req->content_len;
//     uint32_t total_received = 0;
//     uint32_t last_update_bytes = 0;
//     uint32_t update_interval = file_size / 20;  // Update every 5% to reduce overhead
//     if (update_interval < 8192/2) update_interval = 8192/2;  // Minimum 4KB between updates

//     // Speed calculation variables
//     uint32_t start_time = esp_log_timestamp();
//     uint32_t last_time = start_time;
    
//     while (remaining > 0) {
//         int recv_len = httpd_req_recv(req, buffer, MIN(remaining, 8192));
//         if (recv_len <= 0) break;
        
//         fwrite(buffer, 1, recv_len, f);
//         total_received += recv_len;
//         remaining -= recv_len;
        
//         // Send progress update only at intervals to reduce overhead
//         if (total_received - last_update_bytes >= update_interval || remaining == 0) {
//             uint32_t current_time = esp_log_timestamp();
//             float elapsed_sec = (current_time - start_time) / 1000.0f;
//             float speed_kbps = (elapsed_sec > 0) ? (total_received / 1024.0f) / elapsed_sec : 0;
            
//             int percent = (file_size > 0) ? (total_received * 100 / file_size) : 0;
//             send_progress_update("tcp", filename, percent, total_received, file_size, speed_kbps);
//             last_update_bytes = total_received;
//         }
//     }
    
//     fclose(f);
//     free(buffer);
    
//     ESP_LOGI(TAG, "TCP Upload completed: %s (%d bytes)", filename, total_received);
//     httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
//     return ESP_OK;
// }

// // UDP Upload Handler  
// static esp_err_t udp_upload_handler(httpd_req_t *req)
// {
//     char filename[256] = {0};
//     char filesize_str[32] = {0};
    
//     // Get filename from header
//     size_t filename_len = httpd_req_get_hdr_value_len(req, "X-Filename");
//     if (filename_len > 0 && filename_len < sizeof(filename)) {
//         httpd_req_get_hdr_value_str(req, "X-Filename", filename, filename_len + 1);
//     }
    
//     // Get file size from header  
//     size_t filesize_hdr_len = httpd_req_get_hdr_value_len(req, "X-File-Size");
//     if (filesize_hdr_len > 0 && filesize_hdr_len < sizeof(filesize_str)) {
//         httpd_req_get_hdr_value_str(req, "X-File-Size", filesize_str, filesize_hdr_len + 1);
//     }
    
//     if (strlen(filename) == 0) {
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename header");
//         return ESP_FAIL;
//     }
    
//     uint32_t file_size = atoi(filesize_str);
//     ESP_LOGI(TAG, "UDP Upload: %s (%d bytes)", filename, file_size);
    
//     // Create file path
//     char filepath[300];
//     snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
    
//     FILE *f = fopen(filepath, "wb");
//     if (!f) {
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
//         return ESP_FAIL;
//     }
    
//     // Read and write file data with progress updates and speed calculation
//     char *buffer = malloc(16384);  // 16KB buffer for better speed
//     int remaining = req->content_len;
//     uint32_t total_received = 0;
//     uint32_t last_update_bytes = 0;
//     uint32_t update_interval = file_size / 20;  // Update every 5% to reduce overhead
//     if (update_interval < 8192) update_interval = 8192;  // Minimum 8KB between updates
    
//     // Speed calculation variables
//     uint32_t start_time = esp_log_timestamp();
    
//     while (remaining > 0) {
//         int recv_len = httpd_req_recv(req, buffer, MIN(remaining, 16384));
//         if (recv_len <= 0) break;
        
//         fwrite(buffer, 1, recv_len, f);
//         total_received += recv_len;
//         remaining -= recv_len;
        
//         // Send progress update only at intervals to reduce overhead
//         if (total_received - last_update_bytes >= update_interval || remaining == 0) {
//             uint32_t current_time = esp_log_timestamp();
//             float elapsed_sec = (current_time - start_time) / 1000.0f;
//             float speed_kbps = (elapsed_sec > 0) ? (total_received / 1024.0f) / elapsed_sec : 0;
            
//             int percent = (file_size > 0) ? (total_received * 100 / file_size) : 0;
//             send_progress_update("udp", filename, percent, total_received, file_size, speed_kbps);
//             last_update_bytes = total_received;
//         }
//     }
    
//     fclose(f);
//     free(buffer);
    
//     ESP_LOGI(TAG, "UDP Upload completed: %s (%d bytes)", filename, total_received);
//     httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
//     return ESP_OK;
// }

// // TCP Raw Upload Handler - forwards to actual TCP server
// static esp_err_t tcp_raw_upload_handler(httpd_req_t *req)
// {
//     char filename[256] = {0};
//     char filesize_str[32] = {0};
    
//     // Get metadata from headers
//     size_t filename_len = httpd_req_get_hdr_value_len(req, "X-Filename");
//     if (filename_len > 0 && filename_len < sizeof(filename)) {
//         httpd_req_get_hdr_value_str(req, "X-Filename", filename, filename_len + 1);
//     }
    
//     size_t filesize_hdr_len = httpd_req_get_hdr_value_len(req, "X-File-Size");
//     if (filesize_hdr_len > 0 && filesize_hdr_len < sizeof(filesize_str)) {
//         httpd_req_get_hdr_value_str(req, "X-File-Size", filesize_str, filesize_hdr_len + 1);
//     }
    
//     if (strlen(filename) == 0) {
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename header");
//         return ESP_FAIL;
//     }
    
//     uint32_t file_size = atoi(filesize_str);
//     ESP_LOGI(TAG, "TCP Raw Upload: %s (%d bytes) - forwarding to TCP server", filename, file_size);
    
//     // Create socket to connect to TCP server
//     int sock = socket(AF_INET, SOCK_STREAM, 0);
//     if (sock < 0) {
//         ESP_LOGE(TAG, "Failed to create TCP socket");
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create socket");
//         return ESP_FAIL;
//     }
    
//     struct sockaddr_in dest_addr;
//     dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
//     dest_addr.sin_family = AF_INET;
//     dest_addr.sin_port = htons(TCP_PORT);
    
//     if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) != 0) {
//         ESP_LOGE(TAG, "Failed to connect to TCP server");
//         close(sock);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to connect to TCP server");
//         return ESP_FAIL;
//     }
    
//     // Read data from HTTP request and forward to TCP server
//     char *buffer = malloc(4096);
//     if (!buffer) {
//         close(sock);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
//         return ESP_FAIL;
//     }
    
//     int remaining = req->content_len;
//     uint32_t total_sent = 0;
//     uint32_t start_time = esp_log_timestamp();
    
//     while (remaining > 0) {
//         int recv_len = httpd_req_recv(req, buffer, MIN(remaining, 4096));
//         if (recv_len <= 0) break;
        
//         int sent = send(sock, buffer, recv_len, 0);
//         if (sent <= 0) {
//             ESP_LOGE(TAG, "Failed to send data to TCP server");
//             break;
//         }
        
//         total_sent += sent;
//         remaining -= recv_len;
        
//         // Send progress update
//         uint32_t current_time = esp_log_timestamp();
//         float elapsed_sec = (current_time - start_time) / 1000.0f;
//         float speed_kbps = (elapsed_sec > 0) ? (total_sent / 1024.0f) / elapsed_sec : 0;
//         int percent = (file_size > 0) ? (total_sent * 100 / file_size) : 0;
//         send_progress_update("tcp", filename, percent, total_sent, file_size, speed_kbps);
//     }
    
//     // Wait for response from TCP server
//     char response[10];
//     int resp_len = recv(sock, response, sizeof(response) - 1, 0);
//     close(sock);
//     free(buffer);
    
//     if (resp_len > 0) {
//         response[resp_len] = '\0';
//         ESP_LOGI(TAG, "TCP server response: %s", response);
//         if (strncmp(response, "OK", 2) == 0) {
//             httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
//             return ESP_OK;
//         }
//     }
    
//     httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TCP server error");
//     return ESP_FAIL;
// }

// // UDP Raw Upload Handler - forwards to actual UDP server
// static esp_err_t udp_raw_upload_handler(httpd_req_t *req)
// {
//     char filename[256] = {0};
//     char filesize_str[32] = {0};
    
//     // Get metadata from headers
//     size_t filename_len = httpd_req_get_hdr_value_len(req, "X-Filename");
//     if (filename_len > 0 && filename_len < sizeof(filename)) {
//         httpd_req_get_hdr_value_str(req, "X-Filename", filename, filename_len + 1);
//     }
    
//     size_t filesize_hdr_len = httpd_req_get_hdr_value_len(req, "X-File-Size");
//     if (filesize_hdr_len > 0 && filesize_hdr_len < sizeof(filesize_str)) {
//         httpd_req_get_hdr_value_str(req, "X-File-Size", filesize_str, filesize_hdr_len + 1);
//     }
    
//     if (strlen(filename) == 0) {
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename header");
//         return ESP_FAIL;
//     }
    
//     uint32_t file_size = atoi(filesize_str);
//     ESP_LOGI(TAG, "UDP Raw Upload: %s (%d bytes) - forwarding to UDP server", filename, file_size);
    
//     // Create UDP socket
//     int sock = socket(AF_INET, SOCK_DGRAM, 0);
//     if (sock < 0) {
//         ESP_LOGE(TAG, "Failed to create UDP socket");
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create socket");
//         return ESP_FAIL;
//     }
    
//     struct sockaddr_in dest_addr;
//     dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
//     dest_addr.sin_family = AF_INET;
//     dest_addr.sin_port = htons(UDP_PORT);
    
//     // Read all data from HTTP request
//     char *buffer = malloc(req->content_len);
//     if (!buffer) {
//         close(sock);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
//         return ESP_FAIL;
//     }
    
//     int total_received = 0;
//     int remaining = req->content_len;
//     uint32_t start_time = esp_log_timestamp();
    
//     while (remaining > 0) {
//         int recv_len = httpd_req_recv(req, buffer + total_received, remaining);
//         if (recv_len <= 0) break;
//         total_received += recv_len;
//         remaining -= recv_len;
        
//         // Send progress update
//         uint32_t current_time = esp_log_timestamp();
//         float elapsed_sec = (current_time - start_time) / 1000.0f;
//         float speed_kbps = (elapsed_sec > 0) ? (total_received / 1024.0f) / elapsed_sec : 0;
//         int percent = (file_size > 0) ? (total_received * 100 / file_size) : 0;
//         send_progress_update("udp", filename, percent, total_received, file_size, speed_kbps);
//     }
    
//     // Send to UDP server
//     int sent = sendto(sock, buffer, total_received, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
//     if (sent != total_received) {
//         ESP_LOGE(TAG, "Failed to send complete data to UDP server");
//         close(sock);
//         free(buffer);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send to UDP server");
//         return ESP_FAIL;
//     }
    
//     // Wait for response from UDP server
//     char response[10];
//     socklen_t addr_len = sizeof(dest_addr);
//     int resp_len = recvfrom(sock, response, sizeof(response) - 1, 0, (struct sockaddr*)&dest_addr, &addr_len);
//     close(sock);
//     free(buffer);
    
//     if (resp_len > 0) {
//         response[resp_len] = '\0';
//         ESP_LOGI(TAG, "UDP server response: %s", response);
//         if (strncmp(response, "OK", 2) == 0) {
//             httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
//             return ESP_OK;
//         }
//     }
    
//     httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "UDP server error");
//     return ESP_FAIL;
// }

// // Start web server
// static httpd_handle_t start_webserver(void)
// {
//     httpd_config_t config = HTTPD_DEFAULT_CONFIG();
//     config.recv_wait_timeout = 30;
//     config.send_wait_timeout = 30;
//     config.max_uri_handlers = 16;
//     config.stack_size = 8192;  // Increase stack size from default 4096

//     if (httpd_start(&server, &config) == ESP_OK) {
//         httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
//         httpd_uri_t upload_uri = {.uri = "/upload", .method = HTTP_POST, .handler = upload_handler};
//         httpd_uri_t download_uri = {.uri = "/download", .method = HTTP_GET, .handler = download_handler};
//         httpd_uri_t delete_uri = {.uri = "/delete", .method = HTTP_GET, .handler = delete_handler};
//         httpd_uri_t ws_uri = {
//             .uri = "/ws",
//             .method = HTTP_GET,
//             .handler = ws_handler,
//             .user_ctx = NULL,
//             .is_websocket = true
//         };
//         httpd_uri_t tcp_upload_uri = {.uri = "/upload_tcp", .method = HTTP_POST, .handler = tcp_upload_handler};
//         httpd_uri_t udp_upload_uri = {.uri = "/upload_udp", .method = HTTP_POST, .handler = udp_upload_handler};
//         httpd_uri_t tcp_raw_upload_uri = {.uri = "/upload_tcp_raw", .method = HTTP_POST, .handler = tcp_raw_upload_handler};
//         httpd_uri_t udp_raw_upload_uri = {.uri = "/upload_udp_raw", .method = HTTP_POST, .handler = udp_raw_upload_handler};
//         httpd_uri_t album_art_uri = {.uri = "/album_art", .method = HTTP_GET, .handler = album_art_handler};

//         httpd_register_uri_handler(server, &index_uri);
//         httpd_register_uri_handler(server, &upload_uri);
//         httpd_register_uri_handler(server, &download_uri);
//         httpd_register_uri_handler(server, &delete_uri);
//         httpd_register_uri_handler(server, &ws_uri);
//         httpd_register_uri_handler(server, &tcp_upload_uri);
//         httpd_register_uri_handler(server, &udp_upload_uri);
//         httpd_register_uri_handler(server, &tcp_raw_upload_uri);
//         httpd_register_uri_handler(server, &udp_raw_upload_uri);
//         httpd_register_uri_handler(server, &album_art_uri);
        
//         ESP_LOGI(TAG, "HTTP server started with WebSocket support");
//     }
//     return server;
// }

// void app_main(void)
// {
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);

//     wifi_init_sta();
    
//     if (init_sd_card() == ESP_OK) {
//         start_webserver();
        
//         // Start TCP and UDP servers
//         xTaskCreate(tcp_server_task, "tcp_server", 4096*4, NULL, 20, NULL);
//         xTaskCreate(udp_server_task, "udp_server", 4096*2, NULL, 5, NULL);
        
//         ESP_LOGI(TAG, "All servers started. HTTP, TCP (port %d), UDP (port %d)", TCP_PORT, UDP_PORT);
//     } else {
//         ESP_LOGE(TAG, "Failed to initialize SD card");
//     }
// }