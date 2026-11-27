import asyncio
import json
import math
import os
import socket
import threading
import time
import wave
import numpy as np
import websockets
from faster_whisper import WhisperModel
from collections import deque
from pathlib import Path
import re
from concurrent.futures import ThreadPoolExecutor
from deep_translator import MyMemoryTranslator

WHISPER_LIVE_MODEL_NAME = os.getenv("WHISPER_LIVE_MODEL", "small.en")
WHISPER_FINAL_MODEL_NAME = os.getenv("WHISPER_FINAL_MODEL", "medium.en")
def _add_nvidia_dll_dirs():
    """Windows: cho phép dùng DLL CUDA/cuDNN cài qua pip
    (nvidia-cublas-cu12, nvidia-cudnn-cu12) mà không phải sửa PATH."""
    if os.name != "nt":
        return
    try:
        import importlib.util
        spec = importlib.util.find_spec("nvidia")
    except Exception:
        return
    if not spec or not spec.submodule_search_locations:
        return
    for root in spec.submodule_search_locations:
        for bin_dir in Path(root).glob("*/bin"):
            try:
                os.add_dll_directory(str(bin_dir))
                os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ["PATH"]
            except OSError:
                pass


def _cuda_available():
    try:
        import ctranslate2
        return ctranslate2.get_cuda_device_count() > 0
    except Exception:
        return False


_add_nvidia_dll_dirs()
# "auto" (mặc định): có GPU NVIDIA thì dùng CUDA, không thì CPU.
requested_device = os.getenv("WHISPER_LIVE_DEVICE", "auto").lower()
live_device = (
    ("cuda" if _cuda_available() else "cpu")
    if requested_device == "auto" else requested_device
)
live_cpu_threads = int(os.getenv(
    "WHISPER_LIVE_CPU_THREADS",
    str(min(8, os.cpu_count() or 4)),
))


def _default_compute_type(device):
    return os.getenv(
        "WHISPER_LIVE_COMPUTE_TYPE",
        "int8" if device == "cpu" else "float16",
    )


def _load_model(name, device):
    model = WhisperModel(
        name,
        device=device,
        compute_type=_default_compute_type(device),
        cpu_threads=live_cpu_threads,
    )
    if device == "cuda":
        # Chạy thử 1s im lặng: thiếu cuDNN/cuBLAS thường chỉ lỗi lúc suy luận.
        segments, _ = model.transcribe(
            np.zeros(16000, dtype=np.float32), beam_size=1, language="en"
        )
        list(segments)
    return model


def _load_both(device):
    print(
        f">>> Đang nạp faster-whisper live '{WHISPER_LIVE_MODEL_NAME}' "
        f"(device={device}, compute={_default_compute_type(device)})..."
    )
    live = _load_model(WHISPER_LIVE_MODEL_NAME, device)
    if WHISPER_FINAL_MODEL_NAME == WHISPER_LIVE_MODEL_NAME:
        return live, live
    print(
        f">>> Đang nạp faster-whisper chốt '{WHISPER_FINAL_MODEL_NAME}' "
        f"(device={device}, compute={_default_compute_type(device)})..."
    )
    return live, _load_model(WHISPER_FINAL_MODEL_NAME, device)


try:
    live_model, final_model = _load_both(live_device)
except Exception as error:
    if live_device != "cuda":
        raise
    print(
        f">>> [CẢNH BÁO] Không chạy được trên GPU ({error}). "
        f"Cài: pip install nvidia-cublas-cu12 nvidia-cudnn-cu12==9.* "
        f"— tạm chuyển sang CPU."
    )
    live_device = "cpu"
    live_model, final_model = _load_both(live_device)
live_compute_type = _default_compute_type(live_device)
print(
    f">>> Whisper sẵn sàng: live={WHISPER_LIVE_MODEL_NAME}, "
    f"chốt={WHISPER_FINAL_MODEL_NAME}, "
    f"device={live_device}, compute={live_compute_type}."
)

web_clients = set()
voice_history = deque(maxlen=100)
live_stt_state = {
    "type": "live_stt",
    "session_id": None,
    "text": "",
    "state": "idle",
}
server_config = {"target_lang": "vietnamese"}
model_locks = {
    id(live_model): threading.Lock(),
}
if final_model is not live_model:
    model_locks[id(final_model)] = threading.Lock()


async def broadcast_live_stt(session_id, text, state):
    live_stt_state.update({
        "session_id": session_id,
        "text": text,
        "state": state,
    })
    message = json.dumps(live_stt_state)
    for web_ws in list(web_clients):
        try:
            await web_ws.send(message)
        except websockets.exceptions.ConnectionClosed:
            web_clients.discard(web_ws)

SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2
AUDIO_PACKET_SECONDS = 0.05
AUDIO_PACKET_BYTES = int(SAMPLE_RATE * SAMPLE_WIDTH * AUDIO_PACKET_SECONDS)
STT_INTERVAL_SECONDS = 0.8
STT_INITIAL_INTERVAL_SECONDS = 0.1
STT_INITIAL_POLL_SECONDS = 0.05
STT_INITIAL_AUDIO_SECONDS = 1.0
STT_WINDOW_SECONDS = 6
STT_INTERVAL_BYTES = int(SAMPLE_RATE * SAMPLE_WIDTH * STT_INTERVAL_SECONDS)
STT_INITIAL_BYTES = int(
    SAMPLE_RATE * SAMPLE_WIDTH * STT_INITIAL_AUDIO_SECONDS
)
STT_WINDOW_BYTES = int(SAMPLE_RATE * SAMPLE_WIDTH * STT_WINDOW_SECONDS)
SPEECH_TARGET_LANG = "vietnamese"
SUPPORTED_TARGET_LANGUAGES = {
    "vietnamese",
    "french",
    "spanish",
    "japanese",
    "korean",
    "chinese (simplified)",
}
MAX_AUDIO_QUEUE_SECONDS = 2
MAX_AUDIO_QUEUE_PACKETS = math.ceil(
    MAX_AUDIO_QUEUE_SECONDS / AUDIO_PACKET_SECONDS
)
LIVE_VAD_FRAME_SAMPLES = int(SAMPLE_RATE * 0.02)
LIVE_MIN_AUDIO_RMS = 0.001
LIVE_VAD_RMS_THRESHOLD = 0.002
LIVE_VAD_PADDING_SAMPLES = int(SAMPLE_RATE * 0.2)
LIVE_STABILITY_SECONDS = 2.5
LIVE_CORRECTION_CORE_SECONDS = 8
LIVE_CORRECTION_CONTEXT_SECONDS = 1
# Beam cho các đoạn sửa trong lúc đang nói (giữ nhẹ để theo kịp realtime)
# và cho phần đuôi xử lý sau STOP (rộng hơn để chính xác hơn).
LIVE_CORRECTION_BEAM_SIZE = int(os.getenv("WHISPER_CORRECTION_BEAM", "2"))
FINAL_TAIL_BEAM_SIZE = int(os.getenv("WHISPER_FINAL_BEAM", "3"))
FINAL_MIN_TAIL_SECONDS = 0.3
# Số ký tự transcript trước đó dùng làm ngữ cảnh (initial_prompt) cho Whisper.
CORRECTION_PROMPT_CHARS = 200
# MyMemory giới hạn ~500 ký tự/request; dịch từng câu song song + cache
# để các câu đã chốt trong lúc nói được dịch sẵn trước khi bấm STOP.
MYMEMORY_BATCH_CHARS = 450
TRANSLATION_WORKERS = int(os.getenv("TRANSLATION_WORKERS", "6"))
TRANSLATION_TIMEOUT_SECONDS = float(os.getenv("TRANSLATION_TIMEOUT", "8"))
TRANSLATION_EXECUTOR = ThreadPoolExecutor(
    max_workers=TRANSLATION_WORKERS, thread_name_prefix="mymemory"
)
MAX_RECORDING_SECONDS = 180
MAX_RECORDING_BYTES = SAMPLE_RATE * SAMPLE_WIDTH * MAX_RECORDING_SECONDS
DEBUG_AUDIO_PATH = os.getenv(
    "WHISPER_DEBUG_WAV",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "whisper_debug.wav"),
)


def log_audio_quality(raw_bytes):
    samples = np.frombuffer(raw_bytes, dtype="<i2").astype(np.int32)
    if samples.size == 0:
        print(">>> [Audio] Không nhận được mẫu âm thanh.")
        return

    absolute_samples = np.abs(samples)
    rms = float(np.sqrt(np.mean(samples.astype(np.float64) ** 2)))
    peak = int(np.max(absolute_samples))
    clipping_percent = float(np.mean(absolute_samples >= 32760) * 100)
    duration = samples.size / SAMPLE_RATE
    print(
        f">>> [Audio] {duration:.1f}s, RMS={rms:.0f}, peak={peak}, "
        f"clipping={clipping_percent:.3f}%."
    )
    if clipping_percent >= 0.1:
        print(">>> [Audio] CANH BAO: tín hiệu bị clipping; giảm gain/âm lượng micro.")

    if DEBUG_AUDIO_PATH:
        path = Path(DEBUG_AUDIO_PATH)
        with wave.open(str(path), "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(SAMPLE_WIDTH)
            wav_file.setframerate(SAMPLE_RATE)
            wav_file.writeframes(raw_bytes)
        print(f">>> [Audio] Đã lưu bản debug theo yêu cầu: {path}")

# Hàm gắn thẻ ngôn ngữ cho OLED ESP32
def prepare_esp_text(text, lang):
    if lang == 'japanese':
        chunks = [text[i:i+10] for i in range(0, len(text), 10)]
        return "[JA]" + "\n".join(chunks)
    elif lang.startswith('chinese'):
        chunks = [text[i:i+10] for i in range(0, len(text), 10)]
        return "[ZH]" + "\n".join(chunks)
    elif lang == 'korean':
        return "[KO]" + text
    else:
        return "[VI]" + text


def transcribe_live_audio_bytes(raw_bytes):
    """Transcribe mono PCM audio with the low-latency live model."""
    if len(raw_bytes) < SAMPLE_RATE * SAMPLE_WIDTH // 2:
        return "", []

    audio = np.frombuffer(raw_bytes, dtype="<i2").astype(np.float32) / 32768.0
    audio_rms = float(np.sqrt(np.mean(audio * audio)))
    if audio_rms < LIVE_MIN_AUDIO_RMS:
        return "", []

    frame_count = audio.size // LIVE_VAD_FRAME_SAMPLES
    if frame_count == 0:
        return "", []
    framed_audio = audio[:frame_count * LIVE_VAD_FRAME_SAMPLES].reshape(
        frame_count, LIVE_VAD_FRAME_SAMPLES
    )
    frame_rms = np.sqrt(np.mean(framed_audio * framed_audio, axis=1))
    active_frames = np.flatnonzero(frame_rms >= LIVE_VAD_RMS_THRESHOLD)
    if active_frames.size == 0:
        return "", []

    start = max(
        0,
        int(active_frames[0]) * LIVE_VAD_FRAME_SAMPLES
        - LIVE_VAD_PADDING_SAMPLES,
    )
    end = min(
        audio.size,
        (int(active_frames[-1]) + 1) * LIVE_VAD_FRAME_SAMPLES
        + LIVE_VAD_PADDING_SAMPLES,
    )
    audio = audio[start:end]

    with model_locks[id(live_model)]:
        segments, _ = live_model.transcribe(
            audio,
            language="en",
            task="transcribe",
            beam_size=1,
            condition_on_previous_text=False,
            vad_filter=False,
            word_timestamps=True,
        )
        segments = list(segments)

    start_offset = start / SAMPLE_RATE
    live_segments = []
    for segment in segments:
        words = getattr(segment, "words", None)
        if words:
            for word in words:
                word_text = word.word.strip()
                if word_text and word.start is not None and word.end is not None:
                    live_segments.append(
                        {
                            "start": float(word.start) + start_offset,
                            "end": float(word.end) + start_offset,
                            "text": word_text,
                        }
                    )
        elif segment.text.strip():
            live_segments.append(
                {
                    "start": float(segment.start) + start_offset,
                    "end": float(segment.end) + start_offset,
                    "text": segment.text.strip(),
                }
            )
    recognized = " ".join(segment["text"] for segment in live_segments)
    return recognized, live_segments


async def transcribe_live_audio_async(raw_bytes):
    return await asyncio.to_thread(transcribe_live_audio_bytes, raw_bytes)


def transcribe_final_audio_bytes(raw_bytes):
    """Transcribe a complete recording for a more complete final transcript."""
    if len(raw_bytes) < SAMPLE_RATE * SAMPLE_WIDTH // 2:
        return ""

    audio = np.frombuffer(raw_bytes, dtype="<i2").astype(np.float32) / 32768.0
    with model_locks[id(final_model)]:
        segments, _ = final_model.transcribe(
            audio,
            language="en",
            task="transcribe",
            beam_size=FINAL_TAIL_BEAM_SIZE,
            condition_on_previous_text=False,
            vad_filter=False,
        )
        return " ".join(segment.text.strip() for segment in segments).strip()


async def transcribe_final_audio_async(raw_bytes):
    return await asyncio.to_thread(transcribe_final_audio_bytes, raw_bytes)


def transcribe_live_correction_bytes(
    raw_bytes, beam_size=LIVE_CORRECTION_BEAM_SIZE, prompt=None
):
    """Transcribe a live chunk with the larger model and word timestamps."""
    if len(raw_bytes) < SAMPLE_RATE * SAMPLE_WIDTH // 2:
        return []

    audio = np.frombuffer(raw_bytes, dtype="<i2").astype(np.float32) / 32768.0
    # Bỏ qua đoạn gần như im lặng để medium.en không "bịa" câu kiểu "Thank you."
    if float(np.sqrt(np.mean(audio * audio))) < LIVE_MIN_AUDIO_RMS:
        return []
    with model_locks[id(final_model)]:
        segments, _ = final_model.transcribe(
            audio,
            language="en",
            task="transcribe",
            beam_size=beam_size,
            condition_on_previous_text=True,
            initial_prompt=prompt or None,
            vad_filter=False,
            word_timestamps=True,
        )
        segments = list(segments)

    corrected_words = []
    for segment in segments:
        words = getattr(segment, "words", None)
        if words:
            for word in words:
                word_text = word.word.strip()
                if word_text and word.start is not None and word.end is not None:
                    corrected_words.append({
                        "start": float(word.start),
                        "end": float(word.end),
                        "text": word_text,
                    })
        elif segment.text.strip():
            corrected_words.append({
                "start": float(segment.start),
                "end": float(segment.end),
                "text": segment.text.strip(),
            })
    return corrected_words


async def await_with_progress(awaitable, stage, interval_seconds=15):
    task = asyncio.create_task(awaitable)
    started_at = time.monotonic()

    while not task.done():
        try:
            await asyncio.wait_for(
                asyncio.shield(task), timeout=interval_seconds
            )
        except asyncio.TimeoutError:
            elapsed = time.monotonic() - started_at
            print(f">>> [{stage}] Đang xử lý... {elapsed:.0f}s.")

    result = await task
    elapsed = time.monotonic() - started_at
    print(f">>> [{stage}] Hoàn tất sau {elapsed:.1f}s.")
    return result


def compose_live_transcript(
    stable_segments, current_segments, corrected_segments, corrected_ranges
):
    last_stable_end = stable_segments[-1]["end"] if stable_segments else 0
    live_segments = stable_segments + [
        segment for segment in current_segments
        if segment["start"] >= last_stable_end
    ]

    def is_corrected(segment):
        return any(
            start <= segment["start"] < end
            for start, end in corrected_ranges
        )

    visible_segments = [
        segment for segment in live_segments if not is_corrected(segment)
    ]
    visible_segments.extend(corrected_segments)
    visible_segments.sort(key=lambda segment: (segment["start"], segment["end"]))
    return " ".join(segment["text"] for segment in visible_segments).strip()


def split_translation_units(text, max_chars=MYMEMORY_BATCH_CHARS):
    """Chia theo câu; câu quá dài thì chia tiếp theo từ (giới hạn MyMemory)."""
    text = text.strip()
    if not text:
        return []
    sentences = re.split(r"(?<=[.!?])\s+", text)
    pieces = []
    for sentence in sentences:
        if len(sentence) <= max_chars:
            pieces.append(sentence)
            continue
        current = ""
        for word in sentence.split():
            candidate = f"{current} {word}".strip()
            if len(candidate) > max_chars and current:
                pieces.append(current)
                current = word
            else:
                current = candidate
        if current:
            pieces.append(current)
    return [piece for piece in pieces if piece.strip()]


def translate_unit_mymemory(text, target_lang):
    # Tạo translator riêng cho mỗi request để an toàn khi chạy song song.
    translator = MyMemoryTranslator(source="english", target=target_lang)
    translated = translator.translate(text)
    if not translated or not translated.strip():
        raise RuntimeError("MyMemoryTranslator returned an empty translation")
    return translated.strip()


class TranslationCache:
    """Cache bản dịch theo từng câu, dùng chung giữa lúc nói và sau STOP."""

    def __init__(self, target_lang):
        self.target_lang = target_lang
        self.futures = {}
        self.lock = threading.Lock()

    def submit(self, unit):
        with self.lock:
            future = self.futures.get(unit)
            # Request lỗi trước đó thì gửi lại thay vì dùng kết quả lỗi.
            if future is None or (
                future.done() and future.exception() is not None
            ):
                future = TRANSLATION_EXECUTOR.submit(
                    translate_unit_mymemory, unit, self.target_lang
                )
                self.futures[unit] = future
            return future

    def prefetch(self, text):
        """Dịch trước các câu đã kết thúc; bỏ câu cuối vì có thể còn đang nói."""
        units = split_translation_units(text)
        if units and not re.search(r"[.!?]$", units[-1]):
            units = units[:-1]
        for unit in units:
            self.submit(unit)

    def cached_count(self, units):
        with self.lock:
            return sum(
                1 for unit in units
                if unit in self.futures
                and self.futures[unit].done()
                and self.futures[unit].exception() is None
            )


async def translate_units_in_order(cache, units, on_part=None):
    """Chờ từng câu theo thứ tự (các request đã chạy song song), gửi dần."""
    futures = [cache.submit(unit) for unit in units]
    deadline = time.monotonic() + TRANSLATION_TIMEOUT_SECONDS
    results = []
    for unit, future in zip(units, futures):
        remaining = max(0.5, deadline - time.monotonic())
        try:
            translated = await asyncio.wait_for(
                asyncio.wrap_future(future), timeout=remaining
            )
        except Exception as error:
            print(f">>> [Dịch] Câu lỗi ({error}); thử lại một lần.")
            translated = await asyncio.wait_for(
                asyncio.wrap_future(cache.submit(unit)),
                timeout=TRANSLATION_TIMEOUT_SECONDS,
            )
        results.append(translated)
        if on_part is not None:
            await on_part(translated)
    return " ".join(results)


async def send_live_text(
    websocket, session_id, prefix, text, send_lock, cancel_event
):
    async with send_lock:
        if not cancel_event.is_set():
            await websocket.send(f"[S:{session_id}][{prefix}]{text}")


async def process_audio_queue(websocket, audio_queue, cancel_event, session_id):
    audio_full_buffer = bytearray()
    transcript = ""
    live_interim_text = ""
    last_sent_caption = ""
    stable_live_segments = []
    previous_live_segments = []
    corrected_live_segments = []
    corrected_live_ranges = []
    send_lock = asyncio.Lock()
    live_stop_event = asyncio.Event()
    initial_live_broadcast_sent = False
    translation_cache = TranslationCache(server_config["target_lang"])

    async def send_current_caption():
        nonlocal last_sent_caption
        nonlocal initial_live_broadcast_sent
        stable_text = " ".join(
            segment["text"] for segment in stable_live_segments
        )
        caption = " ".join(
            part for part in (stable_text, live_interim_text) if part
        )
        if not caption or caption == last_sent_caption:
            return
        last_sent_caption = caption
        print(f">>> [STT LIVE]: {caption}")
        if not initial_live_broadcast_sent:
            await broadcast_live_stt(session_id, caption, "listening")
            initial_live_broadcast_sent = True
        await send_live_text(
            websocket,
            session_id,
            "EN_LIVE",
            f"{stable_text}\t{live_interim_text}",
            send_lock,
            cancel_event,
        )

    async def transcribe_latest_audio():
        nonlocal live_interim_text
        nonlocal previous_live_segments
        last_processed_bytes = 0
        first_update = True
        next_update_at = time.monotonic() + STT_INITIAL_INTERVAL_SECONDS
        while not cancel_event.is_set() and not live_stop_event.is_set():
            await asyncio.sleep(max(0, next_update_at - time.monotonic()))
            if live_stop_event.is_set():
                return
            next_update_at += STT_INTERVAL_SECONDS
            current_size = len(audio_full_buffer)
            required_audio_bytes = (
                STT_INITIAL_BYTES if first_update else STT_INTERVAL_BYTES
            )
            if current_size - last_processed_bytes < required_audio_bytes:
                if first_update:
                    next_update_at = (
                        time.monotonic() + STT_INITIAL_POLL_SECONDS
                    )
                continue

            window_start = max(0, current_size - STT_WINDOW_BYTES)
            if window_start >= current_size:
                last_processed_bytes = current_size
                continue
            window = bytes(audio_full_buffer[window_start:])
            last_processed_bytes = current_size
            first_update = False
            inference_started = time.monotonic()
            try:
                _, segments = await transcribe_live_audio_async(window)
            except Exception as error:
                print(f">>> [Whisper live] Lỗi nhận dạng đoạn: {error}")
                await send_live_text(
                    websocket, session_id, "ERR",
                    f"Loi nhan dang Whisper: {error}",
                    send_lock, cancel_event
                )
                continue

            inference_seconds = time.monotonic() - inference_started
            window_seconds = len(window) / (SAMPLE_RATE * SAMPLE_WIDTH)
            audio_lag_seconds = max(
                0,
                len(audio_full_buffer) - current_size
            ) / (SAMPLE_RATE * SAMPLE_WIDTH)
            print(
                f">>> [Whisper live] {window_seconds:.1f}s audio xử lý "
                f"{inference_seconds:.1f}s bằng '{WHISPER_LIVE_MODEL_NAME}'; "
                f"audio mới phát sinh khi chờ={audio_lag_seconds:.1f}s."
            )
            if time.monotonic() >= next_update_at:
                next_update_at = time.monotonic()

            if cancel_event.is_set():
                return
            if live_stop_event.is_set():
                return
            if audio_lag_seconds > 0:
                print(
                    f">>> [Whisper live] Hiện kết quả mới nhất đã xử lý "
                    f"dù audio đang đi trước {audio_lag_seconds:.1f}s."
                )

            window_start_seconds = window_start / (
                SAMPLE_RATE * SAMPLE_WIDTH
            )
            candidate_segments = [
                {
                    "start": window_start_seconds + segment["start"],
                    "end": window_start_seconds + segment["end"],
                    "text": segment["text"],
                }
                for segment in segments
            ]
            previous_index = 0
            last_stable_end = (
                stable_live_segments[-1]["end"]
                if stable_live_segments else 0
            )
            previous_stable_end = last_stable_end
            for previous in previous_live_segments:
                if (
                    previous["end"] <= window_start_seconds
                    and previous["end"] > last_stable_end
                ):
                    stable_live_segments.append(previous)
                    last_stable_end = previous["end"]

            previous_interim_segments = [
                segment for segment in previous_live_segments
                if segment["start"] >= previous_stable_end
                and segment["end"] > last_stable_end
            ]
            stable_cutoff = (
                current_size / (SAMPLE_RATE * SAMPLE_WIDTH)
                - LIVE_STABILITY_SECONDS
            )
            for previous in previous_interim_segments:
                if (
                    previous["end"] <= stable_cutoff
                    and previous["end"] > last_stable_end
                ):
                    stable_live_segments.append(previous)
                    last_stable_end = previous["end"]

            current_interim_segments = [
                segment for segment in candidate_segments
                if segment["start"] >= last_stable_end
            ]
            if (
                previous_interim_segments
                and current_interim_segments
                and current_interim_segments[0]["start"]
                > previous_interim_segments[-1]["end"] + 1.0
            ):
                for previous in previous_interim_segments:
                    if previous["end"] > last_stable_end:
                        stable_live_segments.append(previous)
                        last_stable_end = previous["end"]

            for candidate in candidate_segments:
                candidate_text = candidate["text"].casefold().strip(
                    " \t\n.,!?;:"
                )
                matched_index = None
                for index in range(
                    previous_index, len(previous_live_segments)
                ):
                    previous = previous_live_segments[index]
                    if previous["start"] < candidate["start"] - 1.0:
                        previous_index = index + 1
                        continue
                    if previous["start"] > candidate["start"] + 1.0:
                        break
                    previous_text = previous["text"].casefold().strip(
                        " \t\n.,!?;:"
                    )
                    if candidate_text == previous_text:
                        matched_index = index
                        break
                if matched_index is None:
                    continue
                previous_index = matched_index + 1
                if candidate["end"] <= last_stable_end:
                    continue
                if (
                    stable_live_segments
                    and candidate["start"] < last_stable_end
                    and candidate_text
                    == stable_live_segments[-1]["text"]
                    .casefold()
                    .strip(" \t\n.,!?;:")
                ):
                    stable_live_segments[-1]["end"] = candidate["end"]
                    last_stable_end = candidate["end"]
                    continue
                if candidate["start"] > last_stable_end + 1.0:
                    break
                stable_live_segments.append(candidate)
                last_stable_end = candidate["end"]

            interim_segments = [
                segment for segment in candidate_segments
                if segment["start"] >= last_stable_end
            ]
            if not interim_segments:
                interim_segments = [
                    segment for segment in previous_live_segments
                    if segment["start"] >= last_stable_end
                ]
            live_interim_text = " ".join(
                segment["text"] for segment in interim_segments
            )
            previous_live_segments = candidate_segments
            await send_current_caption()

    async def correct_live_chunks():
        core_start = 0.0
        while not cancel_event.is_set():
            available_seconds = len(audio_full_buffer) / (
                SAMPLE_RATE * SAMPLE_WIDTH
            )
            is_final_tail = False
            core_end = core_start + LIVE_CORRECTION_CORE_SECONDS
            chunk_ready = (
                available_seconds
                >= core_end + LIVE_CORRECTION_CONTEXT_SECONDS
            )
            if live_stop_event.is_set():
                # Sau STOP: gom toàn bộ phần chưa sửa thành MỘT lần chạy
                # với beam lớn hơn, thay vì nhiều đoạn 8s nối tiếp.
                if available_seconds - core_start < FINAL_MIN_TAIL_SECONDS:
                    return
                core_end = available_seconds
                chunk_ready = True
                is_final_tail = True
            if not chunk_ready:
                await asyncio.sleep(0.1)
                continue

            window_start = max(
                0.0, core_start - LIVE_CORRECTION_CONTEXT_SECONDS
            )
            window_end = min(
                available_seconds,
                core_end + LIVE_CORRECTION_CONTEXT_SECONDS,
            )
            start_byte = int(
                window_start * SAMPLE_RATE * SAMPLE_WIDTH
            )
            end_byte = int(
                window_end * SAMPLE_RATE * SAMPLE_WIDTH
            )
            # Ngữ cảnh: phần đã sửa ngay trước đoạn này giúp Whisper nối câu
            # và giữ chính tả/tên riêng nhất quán.
            prompt_text = " ".join(
                segment["text"] for segment in sorted(
                    corrected_live_segments, key=lambda s: s["start"]
                )
                if segment["start"] < window_start
            )[-CORRECTION_PROMPT_CHARS:]
            started_at = time.monotonic()
            try:
                chunk_words = await asyncio.to_thread(
                    transcribe_live_correction_bytes,
                    bytes(audio_full_buffer[start_byte:end_byte]),
                    FINAL_TAIL_BEAM_SIZE if is_final_tail
                    else LIVE_CORRECTION_BEAM_SIZE,
                    prompt_text,
                )
            except Exception as error:
                print(
                    f">>> [Whisper live correction] Lỗi đoạn "
                    f"{core_start:.1f}-{core_end:.1f}s: {error}"
                )
                if is_final_tail:
                    return
                core_start += LIVE_CORRECTION_CORE_SECONDS
                continue

            corrected_core = [
                {
                    "start": window_start + word["start"],
                    "end": window_start + word["end"],
                    "text": word["text"],
                }
                for word in chunk_words
                if core_start <= window_start + word["start"] < core_end
            ]
            corrected_live_segments[:] = [
                segment for segment in corrected_live_segments
                if not core_start <= segment["start"] < core_end
            ]
            corrected_live_segments.extend(corrected_core)
            corrected_live_ranges.append((core_start, core_end))

            corrected_transcript = compose_live_transcript(
                stable_live_segments,
                previous_live_segments,
                corrected_live_segments,
                corrected_live_ranges,
            )
            state = "stopped" if live_stop_event.is_set() else "listening"
            await broadcast_live_stt(
                session_id, corrected_transcript, state
            )
            print(
                f">>> [Whisper live correction] Đã sửa "
                f"{core_start:.1f}-{core_end:.1f}s trong "
                f"{time.monotonic() - started_at:.1f}s; "
                f"cập nhật {len(corrected_transcript.split())} từ."
            )
            if is_final_tail:
                return
            # Dịch trước các câu đã chốt (chạy nền, không chặn STT).
            translation_cache.prefetch(corrected_transcript)
            core_start += LIVE_CORRECTION_CORE_SECONDS

    interim_task = asyncio.create_task(transcribe_latest_audio())
    correction_task = asyncio.create_task(correct_live_chunks())
    limit_notified = False

    while True:
        packet = await audio_queue.get()
        if packet is None:
            break

        remaining = MAX_RECORDING_BYTES - len(audio_full_buffer)
        if remaining > 0:
            accepted_packet = packet[:remaining]
            audio_full_buffer.extend(accepted_packet)
        else:
            accepted_packet = b""
        if len(packet) > remaining and not limit_notified:
            limit_notified = True
            await send_live_text(
                websocket,
                session_id,
                "ERR",
                f"Gioi han thu am {MAX_RECORDING_SECONDS} giay",
                send_lock,
                cancel_event,
            )

    live_stop_event.set()
    if cancel_event.is_set():
        interim_task.cancel()
        correction_task.cancel()
        await asyncio.gather(interim_task, return_exceptions=True)
        await asyncio.gather(correction_task, return_exceptions=True)
        await broadcast_live_stt(
            session_id, live_stt_state["text"], "cancelled"
        )
        return
    interim_task.cancel()
    await asyncio.gather(interim_task, return_exceptions=True)
    await asyncio.gather(correction_task, return_exceptions=True)

    live_stable_text = " ".join(
        segment["text"] for segment in stable_live_segments
    )
    live_transcript = " ".join(
        part for part in (live_stable_text, live_interim_text) if part
    ).strip()
    await broadcast_live_stt(
        session_id, live_stt_state["text"], "stopped"
    )
    total_seconds = len(audio_full_buffer) / (SAMPLE_RATE * SAMPLE_WIDTH)
    corrected_until = max(
        (end for _, end in corrected_live_ranges), default=0.0
    )
    corrected_complete = (
        corrected_live_ranges
        and corrected_until >= total_seconds - FINAL_MIN_TAIL_SECONDS
    )
    if audio_full_buffer:
        log_audio_quality(audio_full_buffer)
    if corrected_complete:
        # Đường nhanh: các đoạn medium.en đã sửa trong lúc nói + phần đuôi
        # vừa chạy beam lớn đã phủ toàn bộ audio, không cần chạy lại từ đầu.
        transcript = compose_live_transcript(
            stable_live_segments,
            previous_live_segments,
            corrected_live_segments,
            corrected_live_ranges,
        )
        print(
            f">>> [STT CHỐT] Phiên {session_id}: {len(transcript.split())} "
            f"từ ghép từ các đoạn đã sửa ({total_seconds:.1f}s audio, "
            f"live có {len(live_transcript.split())} từ)."
        )
    elif audio_full_buffer:
        try:
            transcript = await await_with_progress(
                transcribe_final_audio_async(bytes(audio_full_buffer)),
                "Whisper nhận dạng toàn bộ audio",
            )
        except Exception as error:
            print(f">>> [Whisper chốt] Lỗi nhận dạng phiên {session_id}: {error}")
            await send_live_text(
                websocket, session_id, "ERR",
                "Loi nhan dang cuoi Whisper",
                send_lock, cancel_event
            )
            await send_live_text(
                websocket, session_id, "DONE",
                "Loi nhan dang cuoi Whisper",
                send_lock, cancel_event
            )
            return
        print(
            f">>> [STT CHỐT] Phiên {session_id}: "
            f"{len(transcript.split())} từ từ toàn bộ audio "
            f"(live có {len(live_transcript.split())} từ)."
        )
    else:
        transcript = live_transcript

    if transcript:
        target_lang = translation_cache.target_lang
        await send_live_text(
            websocket, session_id, "FINAL_EN", transcript, send_lock, cancel_event
        )
        try:
            translation_started = time.monotonic()
            units = split_translation_units(transcript)
            cached = translation_cache.cached_count(units)
            translated = await translate_units_in_order(
                translation_cache, units
            )
            translated = translated.strip()
            if not translated:
                raise RuntimeError(
                    "Translator returned an empty translation"
                )
            print(
                f">>> [DỊCH {target_lang.upper()}] Hoàn tất sau "
                f"{time.monotonic() - translation_started:.2f}s "
                f"({cached}/{len(units)} câu đã dịch sẵn): {translated}"
            )
            await send_live_text(
                websocket,
                session_id,
                "VI_PART",
                translated,
                send_lock,
                cancel_event,
            )
            esp_translation = prepare_esp_text(
                translated, target_lang
            )
            await send_live_text(
                websocket, session_id, "VI", esp_translation,
                send_lock, cancel_event
            )
            await send_live_text(
                websocket, session_id, "DONE", esp_translation,
                send_lock, cancel_event
            )
            if cancel_event.is_set():
                return

            log_data = {
                "en": transcript,
                "vi": translated,
                "lang": target_lang,
                "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            }
            voice_history.append(log_data)
            for web_ws in list(web_clients):
                try:
                    await web_ws.send(json.dumps(log_data))
                except websockets.exceptions.ConnectionClosed:
                    web_clients.discard(web_ws)
        except Exception as error:
            print(f">>> [Dịch] Lỗi dịch transcript live: {error}")
            await send_live_text(
                websocket, session_id, "ERR", "Loi dich",
                send_lock, cancel_event
            )
            await send_live_text(
                websocket, session_id, "DONE", "Loi dich",
                send_lock, cancel_event
            )
    else:
        print(">>> Không nghe rõ hoặc không có âm thanh.")
        await send_live_text(
            websocket, session_id, "FINAL_EN", "Khong nghe ro",
            send_lock, cancel_event
        )
        await send_live_text(
            websocket, session_id, "DONE", "Khong nghe ro",
            send_lock, cancel_event
        )

async def handle_client(websocket):
    global web_clients
    client_type = "ESP32"
    is_recording = False
    audio_queue = None
    audio_worker = None
    cancel_event = None
    active_session_id = None
    session_tasks = set()

    def on_session_done(task):
        session_tasks.discard(task)
        if not task.cancelled() and task.exception() is not None:
            print(f">>> [ESP32] Lỗi phiên âm thanh: {task.exception()}")

    try:
        async for message in websocket:
            # 1. NHẬN LỆNH ĐIỀU KHIỂN DẠNG TEXT TỪ ESP32 HOẶC WEB DASHBOARD
            if isinstance(message, str):
                if message.startswith("{"):
                    try:
                        data = json.loads(message)
                        if not isinstance(data, dict):
                            continue
                        if data.get("type") == "config":
                            target_lang = data.get("target_lang")
                            if (
                                not isinstance(target_lang, str)
                                or target_lang not in SUPPORTED_TARGET_LANGUAGES
                            ):
                                await websocket.send(json.dumps({
                                    "type": "config_result",
                                    "status": "error",
                                    "message": "Ngôn ngữ dịch không được hỗ trợ.",
                                }))
                            else:
                                server_config["target_lang"] = target_lang
                                print(
                                    ">>> [WEB] Đổi ngôn ngữ dịch giọng nói: "
                                    f"{target_lang}"
                                )
                                await websocket.send(json.dumps({
                                    "type": "config_result",
                                    "status": "success",
                                    "target_lang": target_lang,
                                }))
                    except (json.JSONDecodeError, TypeError) as error:
                        print(f">>> [WEB] Cấu hình không hợp lệ: {error}")
                    continue
                    
                if message == "WEB_CLIENT":
                    client_type = "WEB"
                    web_clients.add(websocket)
                    print(f">>> [WEB] Dashboard đã kết nối. Số lượng client: {len(web_clients)}")
                    await websocket.send(json.dumps({
                        "type": "history",
                        "entries": list(reversed(voice_history)),
                        "target_lang": server_config["target_lang"],
                    }))
                    await websocket.send(json.dumps(live_stt_state))
                    continue
                    
                if message.startswith("START:"):
                    try:
                        session_id = int(message.split(":", 1)[1])
                    except ValueError:
                        print(">>> [ESP32] Mã phiên START không hợp lệ.")
                        continue
                    if session_id <= 0:
                        print(">>> [ESP32] Mã phiên START không hợp lệ.")
                        continue

                    if cancel_event is not None:
                        is_recording = False
                        cancel_event.set()
                        if audio_queue is not None:
                            while not audio_queue.empty():
                                audio_queue.get_nowait()
                            await audio_queue.put(None)

                    print("\n>>> [ESP32] Bắt đầu thu âm giọng nói...")
                    audio_queue = asyncio.Queue(maxsize=MAX_AUDIO_QUEUE_PACKETS)
                    cancel_event = asyncio.Event()
                    audio_worker = asyncio.create_task(
                        process_audio_queue(
                            websocket, audio_queue, cancel_event, session_id
                        )
                    )
                    session_tasks.add(audio_worker)
                    audio_worker.add_done_callback(on_session_done)
                    active_session_id = session_id
                    is_recording = True
                    await broadcast_live_stt(session_id, "", "listening")
                    await websocket.send(
                        f"[S:{session_id}][STATE]Listening for speech..."
                    )
                    
                elif message.startswith("STOP:"):
                    try:
                        session_id = int(message.split(":", 1)[1])
                    except ValueError:
                        print(">>> [ESP32] Mã phiên STOP không hợp lệ.")
                        continue
                    if session_id != active_session_id or audio_queue is None:
                        continue
                    is_recording = False
                    print("\n>>> [ESP32] Dừng thu, đang chốt nhận dạng và bản dịch...")
                    await audio_queue.put(None)
                    audio_queue = None
                elif message.startswith("CANCEL:"):
                    try:
                        session_id = int(message.split(":", 1)[1])
                    except ValueError:
                        print(">>> [ESP32] Mã phiên CANCEL không hợp lệ.")
                        continue
                    if session_id != active_session_id:
                        continue
                    is_recording = False
                    if cancel_event is not None:
                        cancel_event.set()
                    if audio_queue is not None:
                        while not audio_queue.empty():
                            audio_queue.get_nowait()
                        await audio_queue.put(None)
                    audio_queue = None
                    active_session_id = None
                    await broadcast_live_stt(
                        session_id, live_stt_state["text"], "cancelled"
                    )

            # 2. NHẬN LUỒNG ÂM THANH REAL-TIME (BYTES) TỪ ESP32
            elif isinstance(message, bytes) and is_recording and audio_queue is not None:
                await audio_queue.put(message)

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception as e:
        print(f"Lỗi Server: {e}")
    finally:
        for task in session_tasks:
            task.cancel()
        if session_tasks:
            await asyncio.gather(*session_tasks, return_exceptions=True)
        if client_type == "WEB" and websocket in web_clients:
            web_clients.remove(websocket)
        elif client_type == "ESP32":
            print("\n>>> [ESP32] Ngắt kết nối.")

UDP_DISCOVERY_PORT = 8766
WS_SERVER_PORT = 8765

def udp_discovery_worker():
    """Tự động phát và phản hồi UDP để kính ESP32 tự tìm thấy IP Laptop"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", UDP_DISCOVERY_PORT))
        sock.settimeout(1.0)
    except Exception as e:
        print(f"[UDP Discovery] Lỗi khởi động port {UDP_DISCOVERY_PORT}: {e}")
        return

    print(f">>> [UDP Discovery] Sẵn sàng tự động nhận diện kính ESP32 trên cổng UDP {UDP_DISCOVERY_PORT}...")
    last_broadcast = 0
    while True:
        now = time.time()
        # Định kỳ phát broadcast mỗi 2 giây
        if now - last_broadcast >= 2.0:
            try:
                msg = f"SMART_GLASSES_SERVER:{WS_SERVER_PORT}".encode("utf-8")
                sock.sendto(msg, ("255.255.255.255", UDP_DISCOVERY_PORT))
                last_broadcast = now
            except Exception:
                pass

        # Lắng nghe nếu kính chủ động hỏi tìm server
        try:
            data, addr = sock.recvfrom(256)
            text = data.decode("utf-8", errors="ignore").strip()
            if "DISCOVER" in text:
                reply = f"SMART_GLASSES_SERVER:{WS_SERVER_PORT}".encode("utf-8")
                sock.sendto(reply, addr)
                print(f">>> [UDP Discovery] Đã bắt tay và gửi IP cho kính ESP32 tại {addr[0]}")
        except socket.timeout:
            pass
        except Exception:
            pass

async def main():
    port = WS_SERVER_PORT
    # Chạy UDP discovery ở thread nền
    discovery_thread = threading.Thread(target=udp_discovery_worker, daemon=True)
    discovery_thread.start()

    async with websockets.serve(handle_client, "0.0.0.0", port):
        print(f">>> Whisper Realtime Server đang chạy ở ws://0.0.0.0:{port}...")
        print(
            f">>> Gói audio {AUDIO_PACKET_BYTES} byte "
            f"({AUDIO_PACKET_SECONDS:.2f}s), hàng đợi tối đa "
            f"{MAX_AUDIO_QUEUE_SECONDS}s; live STT mỗi "
            f"{STT_INTERVAL_SECONDS}s (bắt đầu kiểm tra sau "
            f"{STT_INITIAL_INTERVAL_SECONDS}s, cần "
            f"{STT_INITIAL_AUDIO_SECONDS}s audio ban đầu) bằng faster-whisper "
            f"'{WHISPER_LIVE_MODEL_NAME}' ({live_compute_type}) trên cửa sổ "
            f"{STT_WINDOW_SECONDS}s; sau STOP nhận dạng toàn bộ audio bằng "
            f"'{WHISPER_FINAL_MODEL_NAME}' rồi dịch (MyMemory, dịch sẵn từng câu)."
        )
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
