#include "wifi_manager.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "display_manager.h"

namespace {

constexpr char kSetupSsid[] = "SmartGlasses_Setup";
constexpr char kSetupPassword[] = "12345678";
constexpr char kPreferencesNamespace[] = "config";
constexpr uint32_t kConnectionTimeoutMs = 15000;
constexpr byte kDnsPort = 53;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
String savedSsid;
String savedPassword;
String savedWsHost;
bool connectionPending = false;
bool connectionTimedOut = false;
uint32_t connectionStartedAt = 0;

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char character = value[i];
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += character;
    } else if (character >= 0x20) {
      escaped += character;
    }
  }
  return escaped;
}

void sendScanResults() {
  const int networkCount = WiFi.scanNetworks();
  if (networkCount < 0) {
    server.send(500, "application/json", "{\"error\":\"WiFi scan failed\"}");
    return;
  }

  String response = "[";
  for (int i = 0; i < networkCount; ++i) {
    if (i > 0) {
      response += ',';
    }
    response += "{\"ssid\":\"";
    response += escapeJson(WiFi.SSID(i));
    response += "\",\"rssi\":";
    response += String(WiFi.RSSI(i));
    response += ",\"secure\":";
    response += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true";
    response += '}';
  }
  response += ']';
  WiFi.scanDelete();
  server.send(200, "application/json", response);
}

void startConnection(const String& ssid, const String& password) {
  savedSsid = ssid;
  savedPassword = password;
  preferences.putString("ssid", savedSsid);
  preferences.putString("password", savedPassword);

  connectionTimedOut = false;
  connectionPending = true;
  connectionStartedAt = millis();
  WiFi.disconnect(false, false);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
}

void registerRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", R"rawliteral(
<!doctype html>
<html lang="vi">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Cài đặt Wi-Fi & Server Kính</title>
  <style>
    body{font:16px Arial,sans-serif;max-width:520px;margin:32px auto;padding:0 16px;color:#202124}
    select,input,button{box-sizing:border-box;width:100%;padding:12px;margin:8px 0}
    button{border:0;border-radius:4px;background:#1769aa;color:white;font-weight:bold;cursor:pointer}
    #status{min-height:24px;color:#444}
    label{font-weight:bold;margin-top:10px;display:block}
  </style>
</head>
<body>
  <h2>Cài đặt Wi-Fi & Server Kính</h2>
  <p>Chọn mạng Wi-Fi và nhập IPv4 của Laptop (xem từ lệnh <code>ipconfig</code>).</p>
  <label for="networks">Mạng tìm thấy</label>
  <select id="networks"><option value="">Đang quét...</option></select>
  <button type="button" onclick="scan()">Quét lại mạng</button>
  <form id="wifi-form">
    <label for="ssid">Tên Wi-Fi (SSID)</label>
    <input id="ssid" name="ssid" required maxlength="32" autocomplete="off">
    <label for="password">Mật khẩu Wi-Fi</label>
    <input id="password" name="password" type="password" maxlength="64" autocomplete="current-password">
    <label for="server_ip">Địa chỉ IPv4 Laptop (Server)</label>
    <input id="server_ip" name="server_ip" placeholder="Ví dụ: 192.168.1.17" maxlength="64" autocomplete="off">
    <button type="submit">Lưu Cấu Hình</button>
  </form>
  <p id="status" role="status"></p>
  <script>
    const select=document.getElementById('networks');
    const statusText=document.getElementById('status');
    async function loadConfig(){
      try{
        const res=await fetch('/config');
        if(res.ok){
          const data=await res.json();
          if(data.ssid) document.getElementById('ssid').value=data.ssid;
          if(data.server_ip) document.getElementById('server_ip').value=data.server_ip;
        }
      }catch(e){}
    }
    async function scan(){
      select.innerHTML='<option value="">Đang quét...</option>';
      try{
        const response=await fetch('/scan');
        if(!response.ok) throw new Error('Không quét được Wi-Fi');
        const networks=await response.json();
        select.innerHTML='<option value="">-- Chọn mạng Wi-Fi --</option>';
        networks.forEach(network=>{
          if(!network.ssid) return;
          const option=document.createElement('option');
          option.value=network.ssid;
          option.textContent=network.ssid+' ('+network.rssi+' dBm)'+(network.secure?' (bảo mật)':'');
          select.appendChild(option);
        });
        if(networks.length===0) statusText.textContent='Không tìm thấy mạng. Hãy quét lại hoặc nhập SSID.';
      }catch(error){statusText.textContent=error.message;}
    }
    select.addEventListener('change',()=>{if(select.value) document.getElementById('ssid').value=select.value;});
    document.getElementById('wifi-form').addEventListener('submit',async event=>{
      event.preventDefault();
      statusText.textContent='Đang lưu và kết nối...';
      const body=new URLSearchParams(new FormData(event.target));
      try{
        const response=await fetch('/save',{method:'POST',body});
        const result=await response.json();
        statusText.textContent=result.message;
      }catch(error){statusText.textContent='Không gửi được cấu hình: '+error.message;}
    });
    async function checkConnection(){
      try{
        const result=await (await fetch('/status')).json();
        if(result.connected){statusText.textContent='Đã kết nối Wi-Fi. Kính đang khởi động...';}
        else if(result.timedOut){statusText.textContent='Kết nối thất bại. Kiểm tra mật khẩu rồi thử lại.';}
      }catch(error){statusText.textContent='Không kiểm tra được trạng thái kết nối: '+error.message;}
    }
    loadConfig();
    scan();
    setInterval(checkConnection,2000);
  </script>
</body>
</html>
)rawliteral");
  });

  server.on("/config", HTTP_GET, []() {
    String response = "{\"ssid\":\"";
    response += escapeJson(savedSsid);
    response += "\",\"server_ip\":\"";
    response += escapeJson(savedWsHost);
    response += "\"}";
    server.send(200, "application/json", response);
  });

  server.on("/scan", HTTP_GET, sendScanResults);
  
  server.on("/status", HTTP_GET, []() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    String response = "{\"connected\":";
    response += connected ? "true" : "false";
    response += ",\"timedOut\":";
    response += connectionTimedOut ? "true" : "false";
    response += '}';
    server.send(200, "application/json", response);
  });

  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    ssid.trim();
    String server_ip = server.arg("server_ip");
    server_ip.trim();

    if (server_ip.length() > 0) {
      savedWsHost = server_ip;
      preferences.putString("ws_host", savedWsHost);
    }

    if (ssid.length() == 0 && WiFi.status() != WL_CONNECTED) {
      server.send(400, "application/json", "{\"message\":\"Tên Wi-Fi không được để trống.\"}");
      return;
    }

    if (ssid.length() > 0 && (ssid != WiFi.SSID() || WiFi.status() != WL_CONNECTED)) {
      startConnection(ssid, server.arg("password"));
      server.send(200, "application/json", "{\"message\":\"Đã lưu. Đang thử kết nối Wi-Fi...\"}");
    } else {
      server.send(200, "application/json", "{\"message\":\"Đã lưu IP Server thành công!\"}");
    }
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });
}

void serviceConnectionAttempt() {
  if (!connectionPending) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    connectionPending = false;
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    return;
  }
  if (millis() - connectionStartedAt >= kConnectionTimeoutMs) {
    connectionPending = false;
    connectionTimedOut = true;
    WiFi.disconnect(false, false);
    Serial.println("Wi-Fi connection timed out; setup portal remains available.");
  }
}

}  // namespace

String getSavedServerIP() {
  Preferences prefs;
  prefs.begin(kPreferencesNamespace, true);
  String ip = prefs.getString("ws_host", "");
  prefs.end();
  return ip;
}

void saveServerIP(const String& ip) {
  Preferences prefs;
  prefs.begin(kPreferencesNamespace, false);
  prefs.putString("ws_host", ip);
  prefs.end();
  savedWsHost = ip;
}

void startConfigPortal(const String& customPrompt) {
  preferences.begin(kPreferencesNamespace, false);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  savedWsHost = preferences.getString("ws_host", "");

  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(kSetupSsid, kSetupPassword)) {
    preferences.end();
    Serial.println("Failed to start Wi-Fi setup access point.");
    displayMessage("WiFi setup failed");
    while (true) {
      delay(1000);
    }
  }

  registerRoutes();
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  server.begin();

  Serial.println("Wi-Fi setup portal is ready.");
  Serial.printf("Connect to %s (password: %s), then open http://192.168.4.1\n",
                kSetupSsid, kSetupPassword);
  
  if (customPrompt.length() > 0) {
    displayMessage(customPrompt);
  } else {
    displayMessage("SmartGlasses_Setup\nPass: 12345678\n192.168.4.1");
  }

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    serviceConnectionAttempt();

    // Điều kiện thoát portal:
    // Đã có Wi-Fi kết nối VÀ đã có IP server được lưu
    if (WiFi.status() == WL_CONNECTED) {
      String current_ip = preferences.getString("ws_host", "");
      if (current_ip.length() > 0) {
        delay(1500);
        break;
      }
    }
    delay(2);
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  preferences.end();
}

void connectWiFi() {
  preferences.begin(kPreferencesNamespace, false);
  preferences.remove("apikey");
  preferences.remove("model");
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  savedWsHost = preferences.getString("ws_host", "");

  WiFi.mode(WIFI_STA);
  if (savedSsid.length() > 0) {
    Serial.printf("Connecting to saved Wi-Fi: %s\n", savedSsid.c_str());
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < kConnectionTimeoutMs) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
      preferences.end();
      return;
    }
    WiFi.disconnect(false, false);
  }

  // Kết nối lưu thất bại hoặc chưa có Wi-Fi -> mở AP setup
  preferences.end();
  startConfigPortal();
}
