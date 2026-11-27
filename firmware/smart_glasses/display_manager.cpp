#include "display_manager.h"
#include "config.h" // Chứa BUTTON_PIN
#include <U8g2lib.h>
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 15, /* data=*/ 14);

static const int LIVE_DISPLAY_MAX_LINES = 64;
static const int LIVE_DISPLAY_VISIBLE_LINES = 4;        // Hiển thị tối đa đúng 4 dòng phụ đề
static const int LIVE_DISPLAY_TRANSLATION_LINES = 3;
static const int LIVE_DISPLAY_TRANSLATION_FIRST_BASELINE = 28;
static const int LIVE_DISPLAY_TRANSLATION_LINE_SPACING = 14;
static const int LIVE_DISPLAY_FIRST_BASELINE = 23;      // Baseline dòng 1 (dưới thanh tiêu đề y=12)
static const int LIVE_DISPLAY_LINE_SPACING = 13;        // Khoảng cách dòng: 23, 36, 49, 62 (vừa vặn màn 64px)
static const uint32_t LIVE_DISPLAY_TYPEWRITER_INTERVAL_MS = 30; // Nhịp gõ phụ đề STT (30ms/ký tự)
static const uint32_t TRANSLATION_TYPEWRITER_INTERVAL_MS = 60; // Nhịp gõ bản dịch chậm hơn để dễ đọc (60ms/ký tự)

static String live_display_lines[LIVE_DISPLAY_MAX_LINES];
static String live_display_text = "";
static String live_display_target_text = "";
static String live_display_status = "Dang nghe";
static uint8_t live_display_dots = 0;
static int live_display_line_count = 0;
static int live_display_first_line = 0;
static uint32_t live_display_last_character = 0;
static bool live_display_initialized = false;
static bool live_display_dirty = false;
// Chế độ bản dịch: thẻ ngôn ngữ ([VI], [JA]...) tách riêng để hiệu ứng gõ
// không gõ cả thẻ ra màn hình, và font vẽ khớp với font dùng khi ngắt dòng.
static bool live_display_translation_mode = false;
static String live_display_font_tag = "";

// Hàm đọc thẻ ngôn ngữ và đổi Font động
void applyDynamicFont(String &text) {
    if (text.startsWith("[JA]")) {
        u8g2.setFont(u8g2_font_b12_t_japanese1); // Font Tiếng Nhật
        text = text.substring(4);
    } 
    else if (text.startsWith("[ZH]")) {
        u8g2.setFont(u8g2_font_wqy12_t_gb2312); // Font Tiếng Trung
        text = text.substring(4);
    } 
    else if (text.startsWith("[KO]")) {
        u8g2.setFont(u8g2_font_unifont_t_korean1); // Font Tiếng Hàn
        text = text.substring(4);
    } 
    else if (text.startsWith("[VI]")) {
        u8g2.setFont(u8g2_font_unifont_t_vietnamese1); // Font Tiếng Việt & Châu Âu
        text = text.substring(4);
    } 
    else {
        u8g2.setFont(u8g2_font_unifont_t_vietnamese1); // Mặc định
    }
}

void initDisplay() {
    Wire.begin(14, 15);
    Wire.setClock(400000); // 400kHz Fast I2C: tối ưu tốc độ đẩy buffer lên màn hình OLED
    u8g2.begin();
    u8g2.enableUTF8Print(); 
}

void displayMessage(String msg) {
    applyDynamicFont(msg);
    u8g2.clearBuffer();
     
    int y_pos = 32;
    int start = 0;
    int end = msg.indexOf('\n');
    while (end != -1) {
        u8g2.setCursor(0, y_pos);
        u8g2.print(msg.substring(start, end));
        y_pos += 16;
        start = end + 1;
        end = msg.indexOf('\n', start);
    }
    u8g2.setCursor(0, y_pos);
    u8g2.print(msg.substring(start));
    u8g2.sendBuffer();
}

static void appendLiveDisplayLine(String line) {
    if (live_display_line_count < LIVE_DISPLAY_MAX_LINES) {
        live_display_lines[live_display_line_count++] = line;
        return;
    }

    for (int i = 1; i < LIVE_DISPLAY_MAX_LINES; i++) {
        live_display_lines[i - 1] = live_display_lines[i];
    }
    live_display_lines[LIVE_DISPLAY_MAX_LINES - 1] = line;
}

void resetLiveTranscriptDisplay() {
    live_display_text = "";
    live_display_target_text = "";
    live_display_status = "Dang nghe";
    live_display_dots = 0;
    live_display_line_count = 0;
    live_display_first_line = 0;
    live_display_last_character = 0;
    live_display_initialized = false;
    live_display_dirty = false;
    live_display_translation_mode = false;
    live_display_font_tag = "";
    for (int i = 0; i < LIVE_DISPLAY_MAX_LINES; i++) {
        live_display_lines[i] = "";
    }
}

void setLiveTranscriptStatus(String status) {
    if (live_display_status == status) return;
    live_display_status = status;
    live_display_dirty = true;
}

static bool isUtf8Continuation(char value) {
    return (static_cast<uint8_t>(value) & 0xC0) == 0x80;
}

static int previousUtf8Boundary(const String &text, int end) {
    int start = end - 1;
    while (start > 0 && isUtf8Continuation(text[start])) {
        start--;
    }
    return start;
}

// Tách toàn bộ đoạn văn bản liên tục vào từng dòng theo độ rộng màn hình 128px
static void rebuildLiveDisplayLines(String text, bool compactFont) {
    if (compactFont) {
        u8g2.setFont(u8g2_font_6x10_tf);
    } else {
        text = live_display_font_tag + text;
        applyDynamicFont(text);
    }
    live_display_line_count = 0;
    for (int i = 0; i < LIVE_DISPLAY_MAX_LINES; i++) {
        live_display_lines[i] = "";
    }

    if (text.length() == 0) {
        live_display_dirty = true;
        return;
    }

    String current_word = "";
    String src = text;
    // Đảm bảo từ cuối cùng đang gõ dở được vẽ lên dòng
    if (src.length() > 0 && src[src.length() - 1] != ' ' && src[src.length() - 1] != '\n') {
        src += " ";
    }

    for (int i = 0; i < src.length(); i++) {
        char c = src[i];
        if (c == ' ' || c == '\n') {
            if (current_word.length() > 0) {
                if (live_display_line_count == 0) {
                    appendLiveDisplayLine(current_word);
                } else {
                    String candidate = live_display_lines[live_display_line_count - 1] + " " + current_word;
                    // Độ rộng tối đa 126px để chữ không bị cắt viền phải
                    if (u8g2.getUTF8Width(candidate.c_str()) > 126) {
                        appendLiveDisplayLine(current_word);
                    } else {
                        live_display_lines[live_display_line_count - 1] = candidate;
                    }
                }
                current_word = "";
            }
            if (c == '\n') {
                appendLiveDisplayLine("");
            }
        } else {
            current_word += c;
            // Xử lý từ đơn lẻ quá dài không có dấu cách
            if (u8g2.getUTF8Width(current_word.c_str()) > 124) {
                appendLiveDisplayLine(current_word);
                current_word = "";
            }
        }
    }

    live_display_dirty = true;
}

static void renderLiveText(String title, bool compactFont) {
    if (!live_display_initialized) return;

    if (compactFont) {
        u8g2.setFont(u8g2_font_6x10_tf);
    } else {
        u8g2.setFont(u8g2_font_unifont_t_vietnamese1);
    }
    // Font cho phần nội dung bản dịch (tiêu đề vẫn dùng font tiếng Việt).
    String body_font_probe = live_display_font_tag;

    uint32_t now = millis();
    if (title == live_display_status) {
        uint8_t dots = (now / 350) % 4;
        if (dots != live_display_dots) {
            live_display_dots = dots;
            live_display_dirty = true;
        }
        for (uint8_t i = 0; i < live_display_dots; i++) {
            title += ".";
        }
    } else if (live_display_dots != 0) {
        live_display_dots = 0;
        live_display_dirty = true;
    }

    int visible_lines = compactFont
        ? LIVE_DISPLAY_VISIBLE_LINES
        : LIVE_DISPLAY_TRANSLATION_LINES;

    if (!live_display_dirty) return;

    u8g2.clearBuffer();

    if (compactFont) {
        // Thanh trạng thái ở trên cùng (y=0..12)
        u8g2.setCursor(0, 9);
        u8g2.print(title);
        u8g2.drawLine(0, 12, 127, 12);

        // Hiển thị 4 dòng phụ đề ổn định
        for (int i = 0; i < visible_lines; i++) {
            int line_index = live_display_first_line + i;
            if (line_index < live_display_line_count &&
                live_display_lines[line_index].length() > 0) {
                int baseline = LIVE_DISPLAY_FIRST_BASELINE + (i * LIVE_DISPLAY_LINE_SPACING);
                u8g2.setCursor(0, baseline);
                u8g2.print(live_display_lines[line_index]);
            }
        }
    } else {
        u8g2.setCursor(0, 12);
        u8g2.print(title);
        u8g2.drawLine(0, 14, 127, 14);
        applyDynamicFont(body_font_probe);

        for (int i = 0; i < visible_lines; i++) {
            int line_index = live_display_first_line + i;
            if (line_index < live_display_line_count &&
                live_display_lines[line_index].length() > 0) {
                int baseline = LIVE_DISPLAY_TRANSLATION_FIRST_BASELINE
                    + (i * LIVE_DISPLAY_TRANSLATION_LINE_SPACING);
                u8g2.setCursor(0, baseline);
                u8g2.print(live_display_lines[line_index]);
            }
        }
    }

    u8g2.sendBuffer();
    live_display_dirty = false;
}

// Hiển thị live text theo kiểu gõ/sửa và cuộn 4 dòng.
static void displayLiveText(
    String title,
    String text,
    bool compactFont,
    bool typewriter = true
) {
    bool visible_changed = false;

    if (text != live_display_target_text) {
        live_display_target_text = text;
        if (!typewriter) {
            visible_changed = (live_display_text != live_display_target_text);
            live_display_text = live_display_target_text;
        }
    }

    uint32_t now = millis();
    uint32_t interval = live_display_translation_mode
        ? TRANSLATION_TYPEWRITER_INTERVAL_MS
        : LIVE_DISPLAY_TYPEWRITER_INTERVAL_MS;
    bool animation_due = (now - live_display_last_character >= interval);

    if (typewriter && animation_due) {
        int max_common = min(
            live_display_text.length(), live_display_target_text.length()
        );
        int common_prefix = 0;
        while (common_prefix < max_common &&
               live_display_text[common_prefix] ==
                   live_display_target_text[common_prefix]) {
            common_prefix++;
        }
        while (common_prefix > 0 &&
               ((common_prefix < live_display_text.length() &&
                 isUtf8Continuation(live_display_text[common_prefix])) ||
                (common_prefix < live_display_target_text.length() &&
                 isUtf8Continuation(live_display_target_text[common_prefix])))) {
            common_prefix--;
        }

        if (live_display_text.length() > common_prefix) {
            int chars_to_delete =
                live_display_text.length() - common_prefix > 25 ? 2 : 1;
            for (int d = 0; d < chars_to_delete; d++) {
                int previous_boundary = previousUtf8Boundary(
                    live_display_text, live_display_text.length()
                );
                if (previous_boundary < common_prefix) {
                    break;
                }
                live_display_text.remove(previous_boundary);
            }
            live_display_last_character = now;
            visible_changed = true;
        } else if (live_display_text.length() < live_display_target_text.length()) {
            int chars_to_add =
                (!live_display_translation_mode &&
                 live_display_target_text.length() - live_display_text.length() > 20)
                    ? 2
                    : 1;
            for (int a = 0;
                 a < chars_to_add &&
                 live_display_text.length() < live_display_target_text.length();
                 a++) {
                int next_boundary = live_display_text.length() + 1;
                while (next_boundary < live_display_target_text.length() &&
                       isUtf8Continuation(live_display_target_text[next_boundary])) {
                    next_boundary++;
                }
                live_display_text += live_display_target_text.substring(
                    live_display_text.length(), next_boundary
                );
            }
            live_display_last_character = now;
            visible_changed = true;
        }
    }

    if (!live_display_initialized || visible_changed) {
        rebuildLiveDisplayLines(live_display_text, compactFont);
        live_display_initialized = true;

        int visible_lines = compactFont
            ? LIVE_DISPLAY_VISIBLE_LINES
            : LIVE_DISPLAY_TRANSLATION_LINES;

        // LOGIC CUỘN 4 DÒNG TỊNH TIẾN CHUẨN XÁC:
        // - Khi có <= 4 dòng: first_line = 0 (hiện từ dòng 1 đến dòng 4)
        // - Khi có dòng 5: first_line = 1 (dòng 1 cuộn lên biến mất, hiện dòng 2, 3, 4, 5)
        // - Khi có dòng 6: first_line = 2 (dòng 2 cuộn lên biến mất, hiện dòng 3, 4, 5, 6)
        // Cứ thế tịnh tiến liên tục, luôn ghim 4 dòng mới nhất đang nói trên màn hình.
        if (live_display_line_count > visible_lines) {
            live_display_first_line = live_display_line_count - visible_lines;
        } else {
            live_display_first_line = 0;
        }
        live_display_dirty = true;
    }

    if (title == "Phu de (EN):") {
        title = live_display_status;
    }
    renderLiveText(title, compactFont);
}

void displayLiveTranscript(String text) {
    text.trim();
    displayLiveText("Phu de (EN):", text, true, true);
}

void displayLiveTranscriptParts(String stableText, String interimText) {
    stableText.trim();
    interimText.trim();

    String text = stableText;
    if (interimText.length() > 0) {
        if (text.length() > 0) text += " ";
        text += interimText;
    }
    displayLiveText("Phu de (EN):", text, true, true);
}

void displayCorrectedLiveTranscript(String text) {
    text.trim();
    displayLiveText("Phu de (EN):", text, true, true);
}

void updateLiveTranscriptDisplay() {
    displayLiveText("Phu de (EN):", live_display_target_text, true, true);
}

void displayFinalTranscript(String text) {
    text.trim();
    displayLiveText("STT - Bam nut xem dich", text, true, true);
}

static const char *TRANSLATION_TITLE = "Ban dich truc tiep:";

// Bản dịch hiện từ trái qua phải giống STT live (cùng nhịp 30ms/ký tự).
void displayLiveTranslation(String text) {
    text.trim();
    String tag = "";
    if (text.startsWith("[JA]") || text.startsWith("[ZH]") ||
        text.startsWith("[KO]") || text.startsWith("[VI]")) {
        tag = text.substring(0, 4);
        text = text.substring(4);
        text.trim();
        if (tag == "[JA]" || tag == "[ZH]") {
            // Server chèn '\n' mỗi 10 ký tự cho CJK; bỏ đi để bản [DONE]
            // trùng với [VI_PART] (không bị xóa lùi rồi gõ lại). Ngắt dòng
            // theo độ rộng đã có sẵn trong rebuildLiveDisplayLines.
            text.replace("\n", "");
        }
    }

    if (!live_display_translation_mode) {
        // Lần đầu chuyển sang bản dịch: bắt đầu gõ từ màn trống thay vì
        // xóa lùi từng ký tự phụ đề tiếng Anh.
        live_display_translation_mode = true;
        live_display_text = "";
        live_display_target_text = "";
        live_display_first_line = 0;
        live_display_initialized = false;
    }
    live_display_font_tag = tag;
    displayLiveText(TRANSLATION_TITLE, text, false, true);
}

void updateLiveTranslationDisplay() {
    if (!live_display_translation_mode) return;
    displayLiveText(TRANSLATION_TITLE, live_display_target_text, false, true);
}

bool isLiveTranslationTyping() {
    return live_display_translation_mode &&
           live_display_text != live_display_target_text;
}

// Hàm delay thông minh: Trả về 'true' ngay lập tức nếu nút bị nhấn
bool waitOrInterrupt(uint32_t ms) {
    uint32_t start = millis();
    while(millis() - start < ms) {
        if (digitalRead(BUTTON_PIN) == LOW) {
            return true; // Bị ngắt bởi nút bấm
        }
        delay(10);
    }
    return false;
}

void holdLiveTranslationDisplay() {
    const uint32_t display_duration_ms = 5000;
    const uint32_t scroll_interval_ms = 1200;
    // Gõ nốt phần bản dịch còn lại trước khi bắt đầu giữ màn hình.
    while (isLiveTranslationTyping()) {
        if (digitalRead(BUTTON_PIN) == LOW) return;
        updateLiveTranslationDisplay();
        delay(5);
    }

    uint32_t started_at = millis();
    uint32_t next_scroll_at = started_at;
    int last_first_line = max(
        0,
        live_display_line_count - LIVE_DISPLAY_TRANSLATION_LINES
    );

    // Bản dịch đã gõ xong và đang ghim các dòng cuối; chỉ khi chưa có hiệu ứng
    // gõ (đường cũ) mới quay về đầu rồi cuộn dần.
    if (!live_display_translation_mode) {
        live_display_first_line = 0;
    }
    live_display_dirty = true;
    renderLiveText("Ban dich truc tiep:", false);

    while (millis() - started_at < display_duration_ms) {
        if (digitalRead(BUTTON_PIN) == LOW) return;

        uint32_t now = millis();
        if (live_display_first_line < last_first_line &&
            static_cast<int32_t>(now - next_scroll_at) >= 0) {
            live_display_first_line++;
            live_display_dirty = true;
            renderLiveText("Ban dich truc tiep:", false);
            next_scroll_at = now + scroll_interval_ms;
        }
        delay(10);
    }
}

// Hàm hiển thị bản dịch sau khi chốt phiên STT
bool printPagedText(String title, String text) {
    applyDynamicFont(text);
    
    const int PAGE_LINES = 4;
    String lines[PAGE_LINES] = {"", "", "", ""}; 
    int current_row = 0;
    text.trim();
    text += " "; 
    String current_word = "";
    
    for (int i = 0; i < text.length(); i++) {
        char c = text[i];
        if (c == ' ' || c == '\n') {
            if (current_word.length() == 0) continue;
            String test_line = lines[current_row].length() > 0 
                ? (lines[current_row] + " " + current_word) 
                : current_word;
            int text_width = u8g2.getUTF8Width(test_line.c_str());
            
            if (text_width > 126 && lines[current_row].length() > 0) {
                if (current_row >= PAGE_LINES - 1) {
                    if (waitOrInterrupt(100)) return true; 
                    for (int r = 0; r < PAGE_LINES - 1; r++) {
                        lines[r] = lines[r + 1];
                    }
                    lines[PAGE_LINES - 1] = current_word; 
                } else {
                    current_row++;
                    lines[current_row] = current_word;
                }
            } else {
                lines[current_row] = test_line;
            }
            current_word = "";
            
            u8g2.clearBuffer();
            u8g2.setCursor(0, 11); u8g2.print(title);
            u8g2.drawLine(0, 13, 128, 13);
            for (int r = 0; r < PAGE_LINES; r++) {
                if (lines[r].length() > 0) {
                    u8g2.setCursor(0, 25 + (r * 12));
                    u8g2.print(lines[r]);
                }
            }
            u8g2.sendBuffer();
            
            if (waitOrInterrupt(10)) return true; 
            
            if (c == '\n') {
                if (current_row >= PAGE_LINES - 1) {
                    if (waitOrInterrupt(100)) return true;
                    for (int r = 0; r < PAGE_LINES - 1; r++) {
                        lines[r] = lines[r + 1];
                    }
                    lines[PAGE_LINES - 1] = "";
                } else {
                    current_row++;
                }
            }
        } else {
            current_word += c;
        }
    }
    
    if (current_word.length() > 0) {
        String test_line = lines[current_row].length() > 0 
            ? (lines[current_row] + " " + current_word) 
            : current_word;
        if (u8g2.getUTF8Width(test_line.c_str()) > 126 && lines[current_row].length() > 0) {
            if (current_row >= PAGE_LINES - 1) {
                for (int r = 0; r < PAGE_LINES - 1; r++) {
                    lines[r] = lines[r + 1];
                }
                lines[PAGE_LINES - 1] = current_word;
            } else {
                current_row++;
                lines[current_row] = current_word;
            }
        } else {
            lines[current_row] = test_line;
        }
        u8g2.clearBuffer();
        u8g2.setCursor(0, 11); u8g2.print(title);
        u8g2.drawLine(0, 13, 128, 13);
        for (int r = 0; r < PAGE_LINES; r++) {
            if (lines[r].length() > 0) {
                u8g2.setCursor(0, 25 + (r * 12));
                u8g2.print(lines[r]);
            }
        }
        u8g2.sendBuffer();
    }
    
    // Đợi sau khi xong toàn bộ văn bản để người dùng đọc
    if (waitOrInterrupt(3000)) return true; 
    return false;
}

void displayTranslation(String original, String translated) {
    if (printPagedText("Tieng Anh:", original)) {
        displayMessage("San sang...\n(Bam de noi)");
        return;
    }
    if (printPagedText("Tieng Viet:", translated)) {
        displayMessage("San sang...\n(Bam de noi)");
        return;
    }
    
    if (!waitOrInterrupt(2000)) {
        displayMessage("San sang...\n(Bam de noi)");
    } else {
        displayMessage("San sang...\n(Bam de noi)");
    }
}

void displayTranslationOnly(String translated) {
    if (printPagedText("Tieng Viet:", translated)) {
        displayMessage("San sang...\n(Bam de noi)");
        return;
    }
    displayMessage("San sang...\n(Bam de noi)");
}