from flask import Flask, render_template_string

app = Flask(__name__)

HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="UTF-8">
    <title>Smart Glasses - Dashboard Độc Lập</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
</head>
<body class="bg-light">
    <div class="container mt-4">
        <h2 class="mb-4 text-primary text-center">⚙️ Bảng Lịch Sử Kính Thông Minh</h2>
        
        <div class="row g-4">
            <div class="col-md-4">
                <div class="card mb-4 shadow-sm">
                    <div class="card-header bg-dark text-white">Ngôn ngữ Dịch</div>
                    <div class="card-body">
                        <div class="mb-3">
                            <label class="form-label">Dịch từ Tiếng Anh sang:</label>
                            <select id="target_lang_input" class="form-select">
                                <option value="vietnamese">Tiếng Việt</option>
                                <option value="french">Tiếng Pháp (French)</option>
                                <option value="spanish">Tiếng Tây Ban Nha (Spanish)</option>
                                <option value="japanese">Tiếng Nhật (Japanese)</option>
                                <option value="korean">Tiếng Hàn (Korean)</option>
                                <option value="chinese (simplified)">Tiếng Trung Giản Thể (Chinese)</option>
                            </select>
                        </div>
                        <button onclick="updateConfig()" class="btn btn-success">Cập nhật toàn hệ thống</button>
                        <div id="speech-config-status" class="small mt-2 text-muted">
                            Ngôn ngữ dịch giọng nói: đang đồng bộ...
                        </div>
                    </div>
                </div>
            </div>

            <div class="col-md-8">
                <div class="card mb-4 shadow-sm">
                    <div class="card-header bg-primary text-white d-flex justify-content-between align-items-center">
                        <span>STT live tổng (English)</span>
                        <span id="live-stt-status" class="badge bg-secondary">Chưa có phiên</span>
                    </div>
                    <div class="card-body">
                        <pre id="live-stt-text" class="mb-0" style="min-height: 100px; max-height: 220px; overflow-y: auto; white-space: pre-wrap;">Transcript live sẽ hiện ở đây.</pre>
                    </div>
                </div>

                <div class="card shadow-sm h-100">
                    <div class="card-header bg-success text-white d-flex justify-content-between align-items-center">
                        <span>🎙️ Lịch sử Dịch Giọng Nói</span>
                        <span id="ws-status" class="badge bg-warning text-dark">Đang kết nối...</span>
                    </div>
                    <div class="card-body" style="max-height: 700px; overflow-y: auto; background-color: #1e1e1e;">
                        <div id="audio-log" class="d-flex flex-column gap-3"></div>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <script>
        const host = window.location.hostname;
        const wsUrl = "ws://" + host + ":8765";

        // XỬ LÝ WEBSOCKET 
        const ws = new WebSocket(wsUrl);
        const statusSpan = document.getElementById("ws-status");
        const audioLogDiv = document.getElementById("audio-log");
        const liveSttText = document.getElementById("live-stt-text");
        const liveSttStatus = document.getElementById("live-stt-status");

        ws.onopen = function() {
            statusSpan.innerText = "Đã kết nối";
            statusSpan.className = "badge bg-light text-success";
            ws.send("WEB_CLIENT"); 
        };
        function renderAudioEntry(data, prepend) {
            const entryDiv = document.createElement("div");
            entryDiv.style = "background: #2d2d2d; padding: 15px; border-radius: 8px; border-left: 4px solid #4CAF50;";

            const original = document.createElement("div");
            original.style = "color: #888; font-size: 14px; margin-bottom: 5px; font-style: italic;";
            original.textContent = "EN: " + data.en;

            const translated = document.createElement("div");
            translated.style = "color: #fff; font-size: 16px; font-weight: 500;";
            translated.textContent = "DỊCH (" + String(data.lang || "vietnamese").toUpperCase() + "): " + data.vi;

            const timestamp = document.createElement("div");
            timestamp.style = "color: #666; font-size: 12px; margin-top: 8px;";
            timestamp.textContent = data.time || "";

            entryDiv.appendChild(original);
            entryDiv.appendChild(translated);
            if (timestamp.textContent) entryDiv.appendChild(timestamp);
            if (prepend) {
                audioLogDiv.prepend(entryDiv);
            } else {
                audioLogDiv.appendChild(entryDiv);
            }
        }
        ws.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                if (data.type === "live_stt") {
                    liveSttText.textContent = data.text || "Chưa nhận dạng được lời nói.";
                    liveSttStatus.textContent = data.state === "listening"
                        ? "Đang nghe"
                        : data.state === "stopped"
                            ? "Đã dừng"
                            : data.state === "cancelled"
                                ? "Đã hủy"
                                : "Chưa có phiên";
                    liveSttStatus.className = data.state === "listening"
                        ? "badge bg-success"
                        : "badge bg-secondary";
                } else if (data.type === "history") {
                    audioLogDiv.innerHTML = "";
                    if (Array.isArray(data.entries) && data.entries.length > 0) {
                        data.entries.forEach(entry => renderAudioEntry(entry, false));
                    } else {
                        audioLogDiv.textContent = "Chưa có lịch sử dịch giọng nói.";
                    }
                    if (data.target_lang && document.activeElement !== langInput) {
                        langInput.value = data.target_lang;
                    }
                } else if (data.type === "config_result") {
                    speechConfigStatus.textContent = data.status === "success"
                        ? "Ngôn ngữ dịch giọng nói: " + data.target_lang
                        : "Không cập nhật được ngôn ngữ dịch giọng nói: " + data.message;
                    speechConfigStatus.className = data.status === "success"
                        ? "small mt-2 text-success"
                        : "small mt-2 text-danger";
                } else if (data.en && data.vi && data.lang) {
                    if (audioLogDiv.textContent === "Chưa có lịch sử dịch giọng nói.") {
                        audioLogDiv.innerHTML = "";
                    }
                    renderAudioEntry(data, true);
                }
            } catch(e) {
                console.error("Không đọc được lịch sử giọng nói:", e);
            }
        };
        ws.onclose = function() {
            statusSpan.innerText = "Mất kết nối";
            statusSpan.className = "badge bg-danger text-white";
            liveSttStatus.textContent = "Mất kết nối";
            liveSttStatus.className = "badge bg-danger";
            speechConfigStatus.textContent = "Mất kết nối dịch giọng nói.";
            speechConfigStatus.className = "small mt-2 text-danger";
        };

        const langInput = document.getElementById("target_lang_input");
        const speechConfigStatus = document.getElementById("speech-config-status");
        let lastSpeechLangSent = null;

        function sendSpeechLanguage(language) {
            if (ws.readyState !== WebSocket.OPEN) {
                speechConfigStatus.textContent = "Giọng nói chưa kết nối; sẽ đồng bộ lại khi có thể.";
                speechConfigStatus.className = "small mt-2 text-warning";
                return;
            }
            if (lastSpeechLangSent === language) return;
            ws.send(JSON.stringify({ type: "config", target_lang: language }));
            lastSpeechLangSent = language;
            speechConfigStatus.textContent = "Đang cập nhật ngôn ngữ dịch giọng nói...";
            speechConfigStatus.className = "small mt-2 text-muted";
        }
        
        function updateConfig() {
            const newLang = langInput.value;
            sendSpeechLanguage(newLang);
        }
    </script>
</body>
</html>
"""

@app.route('/')
def index():
    return render_template_string(HTML_TEMPLATE)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8080)