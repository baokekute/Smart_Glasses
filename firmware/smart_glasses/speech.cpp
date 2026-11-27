#include "speech.h"
#include "config.h"
#include "display_manager.h"
#include "audio_dsp.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#define I2S_PORT I2S_NUM_0

// ==============================================================
// ĐIỀN CHÍNH XÁC ĐỊA CHỈ IPv4 CỦA LAPTOP BẠN VÀO ĐÂY
// ==============================================================
String ws_host = "192.168.1.17";
const uint16_t ws_port = 8765;
const uint16_t UDP_DISCOVERY_PORT = 8766;

bool discoverServerUDP(String &discovered_ip, uint32_t timeout_ms = 3000) {
    WiFiUDP udp;
    if (!udp.begin(UDP_DISCOVERY_PORT)) {
        Serial.println("[UDP] Khong the mo cong UDP.");
        return false;
    }

    Serial.println("[UDP] Gui broadcast tim kiem Server...");
    IPAddress broadcastIP(255, 255, 255, 255);
    IPAddress subnetBroadcast = WiFi.broadcastIP();

    for (int retry = 0; retry < 2; retry++) {
        udp.beginPacket(broadcastIP, UDP_DISCOVERY_PORT);
        udp.print("DISCOVER_SMART_GLASSES_SERVER");
        udp.endPacket();

        if (subnetBroadcast != broadcastIP) {
            udp.beginPacket(subnetBroadcast, UDP_DISCOVERY_PORT);
            udp.print("DISCOVER_SMART_GLASSES_SERVER");
            udp.endPacket();
        }
        delay(50);
    }

    uint32_t start_ms = millis();
    while (millis() - start_ms < timeout_ms) {
        int packetSize = udp.parsePacket();
        if (packetSize > 0) {
            char buffer[128];
            int len = udp.read(buffer, sizeof(buffer) - 1);
            if (len > 0) {
                buffer[len] = '\0';
                String msg = String(buffer);
                Serial.printf("[UDP] Nhan: %s tu %s\n", msg.c_str(), udp.remoteIP().toString().c_str());
                if (msg.startsWith("SMART_GLASSES_SERVER")) {
                    discovered_ip = udp.remoteIP().toString();
                    udp.stop();
                    return true;
                }
            }
        }
        delay(20);
    }
    udp.stop();
    return false;
}

RingbufHandle_t audio_rb = NULL;
volatile bool is_recording = false;
AudioFilter dsp(1.0);
WebSocketsClient webSocket;

static const size_t AUDIO_PACKET_BYTES = 16000 * 2 / 20;
static const size_t AUDIO_PACKET_BUFFER_SIZE = AUDIO_PACKET_BYTES + 512;
static const size_t AUDIO_RINGBUFFER_BYTES = 48 * 1024;
static volatile uint32_t audio_ringbuffer_drops = 0;

String ws_result_text = "";
String ws_live_text = "";
String ws_live_stable_text = "";
String ws_live_partial_text = "";
String ws_live_translation_text = "";
String ws_error_text = "";
volatile bool ws_live_ready = false;
volatile bool ws_corrected_ready = false;
volatile bool ws_translation_ready = false;
volatile bool ws_error_ready = false;
volatile bool ws_done_ready = false;
volatile bool ws_connected = false;
uint32_t ws_session_id = 0;
String ws_corrected_text = "";

static bool buttonPressed(uint8_t pin) {
    if (digitalRead(pin) != LOW) return false;
    delay(30);
    return digitalRead(pin) == LOW;
}

static void waitButtonRelease(uint8_t pin) {
    while (digitalRead(pin) == LOW) {
        webSocket.loop();
        delay(5);
    }
    delay(30);
}

static void setLiveTranslation(String text) {
    text.trim();
    if (text.startsWith("[VI]")) {
        text.remove(0, 4);
        text.trim();
    }
    if (text.length() == 0) return;

    ws_live_translation_text = text;
    ws_translation_ready = true;
}

static void clearAudioBuffer() {
    if (audio_rb == NULL) return;
    size_t item_size;
    void *item;
    while ((item = xRingbufferReceive(audio_rb, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(audio_rb, item);
    }
}

static void appendAudioPacket(
    const uint8_t *data,
    size_t length,
    uint8_t *packet,
    size_t &packet_length
) {
    while (length > 0) {
        size_t available = AUDIO_PACKET_BUFFER_SIZE - packet_length;
        size_t copy_length = length < available ? length : available;
        memcpy(packet + packet_length, data, copy_length);
        packet_length += copy_length;
        data += copy_length;
        length -= copy_length;

        while (packet_length >= AUDIO_PACKET_BYTES) {
            webSocket.sendBIN(packet, AUDIO_PACKET_BYTES);
            packet_length -= AUDIO_PACKET_BYTES;
            if (packet_length > 0) {
                memmove(packet, packet + AUDIO_PACKET_BYTES, packet_length);
            }
        }
    }
}

// Nhận transcript, bản dịch theo lô và kết quả cuối cùng từ server.
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
        ws_connected = true;
    } else if (type == WStype_DISCONNECTED) {
        ws_connected = false;
    } else if (type == WStype_TEXT) {
        String response;
        response.reserve(length);
        for (size_t i = 0; i < length; i++) {
            response += (char)payload[i];
        }

        if (!response.startsWith("[S:")) return;
        int session_end = response.indexOf(']');
        if (session_end < 0) return;
        uint32_t response_session = (uint32_t)response.substring(3, session_end).toInt();
        if (response_session != ws_session_id) return;
        response.remove(0, session_end + 1);

        if (response.startsWith("[EN_PENDING]")) {
            // The OLED indicates listening through its animated title.
        } else if (response.startsWith("[EN_LIVE]")) {
            String live_parts = response.substring(9);
            int separator = live_parts.indexOf('\t');
            if (separator >= 0) {
                ws_live_stable_text = live_parts.substring(0, separator);
                ws_live_partial_text = live_parts.substring(separator + 1);
            } else {
                ws_live_stable_text = "";
                ws_live_partial_text = live_parts;
            }
            ws_live_text = ws_live_stable_text;
            if (ws_live_text.length() > 0 && ws_live_partial_text.length() > 0) {
                ws_live_text += " ";
            }
            ws_live_text += ws_live_partial_text;
            ws_live_ready = true;
        } else if (response.startsWith("[EN_CORRECTED]")) {
            ws_corrected_text = response.substring(14);
            ws_live_stable_text = ws_corrected_text;
            ws_live_partial_text = "";
            ws_corrected_ready = true;
        } else if (response.startsWith("[EN_REPLACE]")) {
            ws_live_text = response.substring(12);
            ws_live_stable_text = "";
            ws_live_partial_text = ws_live_text;
            ws_live_ready = true;
        } else if (response.startsWith("[FINAL_EN]")) {
            ws_live_text = response.substring(10);
            ws_live_stable_text = ws_live_text;
            ws_live_partial_text = "";
            ws_live_ready = true;
        } else if (response.startsWith("[EN_APPEND]")) {
            ws_live_text += response.substring(11);
            ws_live_ready = true;
        } else if (response.startsWith("[EN]")) {
            ws_live_text = response.substring(4);
            ws_live_ready = true;
        } else if (response.startsWith("[VI]")) {
            // Dùng bản dịch đầy đủ làm dự phòng nếu gói [VI_PART] bị thiếu.
            setLiveTranslation(response.substring(4));
            Serial.printf(
                "[WS] Nhan ban dich day du, %u byte.\n",
                static_cast<unsigned int>(ws_live_translation_text.length())
            );
        } else if (response.startsWith("[VI_PART]")) {
            String translated_part = response.substring(9);
            translated_part.trim();
            if (translated_part.length() > 0) {
                if (ws_live_translation_text.length() > 0) {
                    ws_live_translation_text += " ";
                }
                ws_live_translation_text += translated_part;
                ws_translation_ready = true;
                Serial.printf(
                    "[WS] Nhan VI_PART, tong cong %u byte.\n",
                    static_cast<unsigned int>(ws_live_translation_text.length())
                );
            }
        } else if (response.startsWith("[DONE]")) {
            String final_result = response.substring(6);
            final_result.trim();
            if (final_result.startsWith("[VI]")) {
                setLiveTranslation(final_result.substring(4));
            }
            ws_result_text = final_result;
            ws_done_ready = true;
            Serial.printf(
                "[WS] Nhan DONE, ban dich da nhan=%s.\n",
                ws_live_translation_text.length() > 0 ? "co" : "khong"
            );
        } else if (response.startsWith("[ERR]")) {
            ws_error_text = response.substring(5);
            ws_error_ready = true;
        } else if (response.startsWith("[STATE]")) {
            // Chỉ là thông báo trạng thái; không được coi là DONE.
        } else {
            ws_result_text = response;
            ws_done_ready = true;
        }
    }
}

// Luồng đọc Micro siêu nhẹ, đã được đẩy sang Core 1
// 1. LUỒNG ĐỌC MICRO I2S (Sửa lỗi Bit-shift INMP441)
void i2s_record_task(void *arg) {
    size_t bytes_read;
    int32_t sample32[256];
    int16_t sample16[256];
    while (true) {
        if (is_recording) {
            esp_err_t read_status = i2s_read(
                I2S_PORT,
                sample32,
                sizeof(sample32),
                &bytes_read,
                pdMS_TO_TICKS(20)
            );
            if (read_status != ESP_OK || !is_recording || bytes_read < sizeof(int32_t)) {
                continue;
            }
            size_t samples_count = bytes_read / sizeof(int32_t);
            if (samples_count > sizeof(sample32) / sizeof(sample32[0])) {
                samples_count = sizeof(sample32) / sizeof(sample32[0]);
            }
            
            for (size_t i = 0; i < samples_count; i++) {
                // INMP441 is 24-bit data left-aligned in a 32-bit I2S sample.
                int32_t pcm16 = sample32[i] >> 16;
                sample16[i] = dsp.process(pcm16);
            }
            
            if (audio_rb != NULL) {
                BaseType_t queued = xRingbufferSend(
                    audio_rb,
                    sample16,
                    samples_count * sizeof(sample16[0]),
                    pdMS_TO_TICKS(10)
                );
                if (queued != pdTRUE) {
                    audio_ringbuffer_drops++;
                    if (audio_ringbuffer_drops == 1 || audio_ringbuffer_drops % 100 == 0) {
                        Serial.printf(
                            "Audio ringbuffer overrun: %lu block(s) dropped\n",
                            (unsigned long)audio_ringbuffer_drops
                        );
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// 2. LOGIC PUSH-TO-TALK (Gom dữ liệu chờ xử lý AI)
void initSpeechRecognition()
{
    Serial.println("Khoi tao Micro & WebSocket...");
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP);

    // Cấu hình I2S chuẩn cho INMP441 (32-bit)
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN
    };
    
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    audio_rb = xRingbufferCreate(AUDIO_RINGBUFFER_BYTES, RINGBUF_TYPE_BYTEBUF);

    // Khởi tạo luồng nền xử lý Microphone
    if (audio_rb != NULL) {
        xTaskCreatePinnedToCore(i2s_record_task, "MicTask", 8192, NULL, 3, NULL, 1);
    } else {
        Serial.println("Khong the cap phat audio ringbuffer.");
    }

    // ==============================================================
    // 1. Quét UDP tìm Server tự động
    // ==============================================================
    displayMessage("Dang tim Server...");
    String discovered_ip = "";
    bool found_udp = discoverServerUDP(discovered_ip, 3000);

    if (found_udp && discovered_ip.length() > 0) {
        ws_host = discovered_ip;
        saveServerIP(ws_host);
        Serial.printf("[Speech] Tim thay server qua UDP: %s\n", ws_host.c_str());
        displayMessage("Server:\n" + ws_host);
        delay(1200);
    } else {
        // ==============================================================
        // 2. Không thấy qua UDP -> Kiểm tra IP đã lưu trong Flash
        // ==============================================================
        Serial.println("[Speech] Khong thay qua UDP, kiem tra Flash...");
        String saved_ip = getSavedServerIP();
        if (saved_ip.length() > 0) {
            ws_host = saved_ip;
            Serial.printf("[Speech] Dung IP trong Flash: %s\n", ws_host.c_str());
            displayMessage("Dung IP cu:\n" + ws_host);
            delay(1200);
        } else {
            // ==============================================================
            // 3. Chưa có IP -> Bắt buộc người dùng nhập qua trang Web
            // ==============================================================
            Serial.println("[Speech] Chua co IP! Bat buoc nguoi dung nhap...");
            displayMessage("Chua co IP Server\nVao 192.168.4.1\nnhap IP laptop");
            startConfigPortal("Chua co IP Server\nVao 192.168.4.1\nnhap IP laptop");
            ws_host = getSavedServerIP();
            if (ws_host.length() == 0) {
                ws_host = "192.168.1.17";
            }
        }
    }

    // Bắt đầu kết nối WebSocket tới Server Python
    Serial.printf("[Speech] Ket noi WebSocket toi %s:%u\n", ws_host.c_str(), ws_port);
    webSocket.begin(ws_host.c_str(), ws_port, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}
String listenSpeech() 
{
    if (audio_rb == NULL) {
        displayMessage("Audio buffer error\nKhoi dong lai");
        return "CANCEL";
    }

    uint8_t audio_packet[AUDIO_PACKET_BUFFER_SIZE];
    size_t audio_packet_length = 0;

    // Chỉ bắt đầu phiên sau khi lần nhấn mở chế độ đã được nhả.
    waitButtonRelease(BUTTON_PIN);

    uint32_t connect_started = millis();
    while (!ws_connected && millis() - connect_started < 4000) {
        webSocket.loop();
        delay(10);
    }
    if (!ws_connected) {
        // Thu quet UDP nhanh 1 lan nua phong khi server vua bat sau
        String re_ip = "";
        if (discoverServerUDP(re_ip, 2000)) {
            ws_host = re_ip;
            saveServerIP(ws_host);
            webSocket.disconnect();
            webSocket.begin(ws_host.c_str(), ws_port, "/");
            displayMessage("Tim thay Server:\n" + ws_host);
            delay(1000);
        } else {
            displayMessage("Server offline\nKiem tra IP/WiFi");
            return "CANCEL";
        }
    }

    clearAudioBuffer();

    resetLiveTranscriptDisplay();
    ws_live_text = "";
    ws_live_stable_text = "";
    ws_live_partial_text = "";
    ws_live_translation_text = "";
    ws_corrected_text = "";
    ws_result_text = "";
    ws_live_ready = false;
    ws_corrected_ready = false;
    ws_translation_ready = false;
    ws_error_ready = false;
    ws_done_ready = false;
    ws_session_id++;
    if (ws_session_id == 0) ws_session_id = 1;
    setLiveTranscriptStatus("Dang nghe");
    displayLiveTranscript("");
    is_recording = true;
    webSocket.sendTXT("START:" + String(ws_session_id));

    // QUÁ TRÌNH 1: THU ÂM & QUÉT NGẮT
    uint32_t recording_started = millis();
    while (true) {
        webSocket.loop();

        if (ws_corrected_ready) {
            displayCorrectedLiveTranscript(ws_corrected_text);
            ws_live_text = ws_corrected_text;
            ws_corrected_ready = false;
            ws_live_ready = false;
        } else if (ws_live_ready) {
            displayLiveTranscriptParts(
                ws_live_stable_text, ws_live_partial_text
            );
            ws_live_ready = false;
        }
        updateLiveTranscriptDisplay();
        if (ws_error_ready) {
            displayMessage(ws_error_text);
            ws_error_ready = false;
        }
        size_t size_received;
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio_rb, &size_received, pdMS_TO_TICKS(10));
        if (item != NULL) {
            appendAudioPacket(
                item, size_received, audio_packet, audio_packet_length
            );
            vRingbufferReturnItem(audio_rb, (void *)item);
        }

        // Nhấn lần hai sẽ chốt phiên và dịch phần đã nhận dạng.
        if (buttonPressed(BUTTON_PIN)) break;

        if (millis() - recording_started >= 180000) break;

        if (buttonPressed(CANCEL_BUTTON_PIN)) {
            is_recording = false;
            webSocket.sendTXT("CANCEL:" + String(ws_session_id));
            waitButtonRelease(CANCEL_BUTTON_PIN);
            delay(20);
            clearAudioBuffer();
            ws_live_text = "";
            return "CANCEL"; 
        }
    }
    
    waitButtonRelease(BUTTON_PIN);

    // Dừng mic rồi gửi hết audio đang chờ trong ring buffer.
    is_recording = false;
    delay(20);
    size_t size_received;
    uint8_t *item;
    while ((item = (uint8_t *)xRingbufferReceive(audio_rb, &size_received, 0)) != NULL) {
        appendAudioPacket(
            item, size_received, audio_packet, audio_packet_length
        );
        vRingbufferReturnItem(audio_rb, item);
    }
    if (audio_packet_length > 0) {
        webSocket.sendBIN(audio_packet, audio_packet_length);
    }

    // Giữ phụ đề tiếng Anh trong khi server dịch các cụm đã nhận dạng.
    setLiveTranscriptStatus("Dang xu ly");
    ws_result_text = "";
    if (ws_live_text.length() > 0) {
        displayLiveTranscript(ws_live_text);
    } else {
        displayLiveTranscript("Dang chot STT...");
    }
    webSocket.sendTXT("STOP:" + String(ws_session_id)); 

    uint32_t timeout = millis();
    bool translation_displayed = false;

    while (millis() - timeout < 600000) {
        webSocket.loop();
        if (translation_displayed) {
            // Đã sang bản dịch: bỏ phụ đề EN đến muộn để không đè màn hình.
            ws_corrected_ready = false;
            ws_live_ready = false;
        } else if (ws_corrected_ready) {
            displayCorrectedLiveTranscript(ws_corrected_text);
            ws_live_text = ws_corrected_text;
            ws_corrected_ready = false;
            ws_live_ready = false;
        } else if (ws_live_ready) {
            displayLiveTranscriptParts(
                ws_live_stable_text, ws_live_partial_text
            );
            ws_live_ready = false;
        }
        if (ws_translation_ready) {
            displayLiveTranslation(ws_live_translation_text);
            ws_translation_ready = false;
            translation_displayed = true;
        } else if (translation_displayed) {
            updateLiveTranslationDisplay();  // tiếp tục gõ bản dịch
        } else {
            updateLiveTranscriptDisplay();
        }
        if (ws_error_ready) {
            Serial.printf("[WS] Server bao loi: %s\n", ws_error_text.c_str());
            displayMessage(ws_error_text);
            ws_error_ready = false;
            return "CANCEL";
        }
        if (ws_done_ready && translation_displayed) {
            break;
        }

        // Nếu bấm bất kỳ nút nào trong lúc đợi Server -> Hủy ngang
        if (buttonPressed(BUTTON_PIN) || buttonPressed(CANCEL_BUTTON_PIN)) {
            webSocket.sendTXT("CANCEL:" + String(ws_session_id));
            waitButtonRelease(
                digitalRead(BUTTON_PIN) == LOW ? BUTTON_PIN : CANCEL_BUTTON_PIN
            );
            delay(20);
            clearAudioBuffer();
            ws_live_text = "";
            return "CANCEL";
        }
        delay(10);
    }

    if (!translation_displayed || !ws_done_ready) {
        webSocket.sendTXT("CANCEL:" + String(ws_session_id));
        delay(20);
        clearAudioBuffer();
        ws_live_text = "";
        displayMessage("Server timeout\nKiem tra server");
        return "CANCEL";
    }
    return "STREAMED";
}

String getRecognizedSpeech() {
    return ws_live_text;
}
