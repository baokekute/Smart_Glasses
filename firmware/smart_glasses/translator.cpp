#include <HTTPClient.h>
#include <WiFiClientSecure.h>

String translateText(String text)
{
    // Xử lý khoảng trắng thành định dạng URL
    String encodedText = text;
    encodedText.replace(" ", "%20");

    // Endpoint API ẩn của Google Translate (Từ tiếng Anh - sl=en sang tiếng Việt - tl=vi)
    String url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=en&tl=vi&dt=t&q=" + encodedText;

    // Khởi tạo client bảo mật và bỏ qua kiểm tra chứng chỉ SSL
    WiFiClientSecure client;
    client.setInsecure(); 

    HTTPClient http;
    http.begin(client, url);

    int code = http.GET();
    String translated = "API Error";

    if (code > 0)
    {
        String payload = http.getString();
        
        // Google trả về chuỗi JSON dạng mảng lồng nhau: [[["xin chào...","hello..."]]]
        // Bóc tách nhanh chuỗi kết quả nằm giữa cặp dấu ngoặc kép đầu tiên
        int start = payload.indexOf("\"") + 1;
        int end = payload.indexOf("\"", start);
        
        if(start > 0 && end > start) {
            translated = payload.substring(start, end);
        } else {
            translated = "Parse Error";
        }
    } else {
        translated = "Net Error " + String(code);
    }

    http.end();
    return translated;
}