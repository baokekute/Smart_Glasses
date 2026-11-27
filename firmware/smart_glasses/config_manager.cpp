#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

WebServer server(80);
Preferences preferences;

// Các biến lưu cấu hình
String ssid = "";
String password = "";

// Giao diện Web HTML (Viết trực tiếp vào biến String)
const char* html_page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cài đặt Kính Thông Minh</title>
  <style>
    body { font-family: Arial; padding: 20px; }
    input, select { width: 100%; padding: 10px; margin: 10px 0; }
    button { padding: 10px 20px; background: #007bff; color: white; border: none; }
  </style>
</head>
<body>
  <h2>Cấu hình Smart Glasses</h2>
  <form action="/save" method="POST">
    Tên WiFi: <input type="text" name="ssid"><br>
    Mật khẩu WiFi: <input type="password" name="password"><br>
    <button type="submit">Lưu Cài Đặt</button>
  </form>
</body>
</html>
)rawliteral";

void setupWiFiManager() {
  // 1. Đọc dữ liệu cũ từ bộ nhớ
  preferences.begin("config", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  
  // 2. Thử kết nối WiFi
  if (ssid != "") {
    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
  }

  // 3. Nếu kết nối thất bại -> Mở chế độ phát WiFi để cài đặt
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SmartGlasses_Setup", "12345678"); // Pass của WiFi kính phát ra
    
    // Khởi tạo trang chủ HTML
    server.on("/", HTTP_GET, []() {
      server.send(200, "text/html", html_page);
    });

    // Hàm xử lý khi người dùng bấm nút "Lưu Cài Đặt"
    server.on("/save", HTTP_POST, []() {
      // Nhận dữ liệu từ Form HTML
      String new_ssid = server.arg("ssid");
      String new_pass = server.arg("password");

      // Lưu vĩnh viễn vào bộ nhớ ESP32
      preferences.putString("ssid", new_ssid);
      preferences.putString("password", new_pass);
      preferences.remove("apikey");
      preferences.remove("model");

      server.send(200, "text/html", "<h1>Da luu! Kinh se khoi dong lai...</h1>");
      delay(2000);
      ESP.restart(); // Khởi động lại mạch để ăn cấu hình mới
    });

    server.begin();
    
    // Nếu rớt vào vòng lặp này, code sẽ đứng đây để phục vụ web, 
    // không chạy luồng chụp ảnh cho đến khi có mạng
    while(true) {
       server.handleClient();
    }
  }
}