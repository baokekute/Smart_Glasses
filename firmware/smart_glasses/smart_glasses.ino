#include "config.h"
#include "wifi_manager.h"
#include "display_manager.h"
#include "speech.h"
#include <WebSocketsClient.h>

extern WebSocketsClient webSocket;
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
void setup()
{
    Serial.begin(115200);
    initDisplay();
    displayMessage("Booting AI Glasses");
    
    // Nếu giữ nút CANCEL khi vừa bật nguồn -> Chủ động vào trang Cài đặt Wi-Fi & IP
    pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(CANCEL_BUTTON_PIN) == LOW) {
        displayMessage("Che do Cai dat\n192.168.4.1");
        startConfigPortal("SmartGlasses_Setup\nPass: 12345678\n192.168.4.1");
    } else {
        connectWiFi();
    }
    displayMessage("WiFi Connected");
    
    initSpeechRecognition();
    displayMessage("Speech Ready");
    delay(2000);
}

void loop() {
    webSocket.loop();

    // Thiết lập màn hình mặc định
    displayMessage("Speech Ready");

    while (true) {
        webSocket.loop();

        // XỬ LÝ NHẤN NÚT DỊCH GIỌNG NÓI
        if (digitalRead(BUTTON_PIN) == LOW) {
            delay(30);
            if (digitalRead(BUTTON_PIN) == LOW) {
                String result = listenSpeech();
            
                // Nếu không bị hủy ngang, hiển thị kết quả
                if (result == "STREAMED") {
                    holdLiveTranslationDisplay();
                } else if (result != "CANCEL") {
                    displayTranslationOnly(result);
                }
                while (digitalRead(BUTTON_PIN) == LOW) {
                    webSocket.loop();
                    delay(10);
                }
                delay(30);
                break; // Trở lại màn hình mặc định
            }
        }
        delay(20);
    }
}
