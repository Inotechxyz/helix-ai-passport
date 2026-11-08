// main/langstudy_tts_edge.c
//
// Edge streams MPEG-2 Layer III, 24 kHz mono, 48 kbit/s over a WebSocket: one
// frame is 576 samples and 144 bytes, sent in 720-byte audio payloads. The
// bytes are decoded as they arrive and written straight to the codec, so no
// whole-phrase receive buffer is needed. The codec is opened at the stream's own
// 24 kHz: converting it to the synthesizer's 16 kHz folded the top octave back
// into the voice as audible hiss. On the ESP32-C3 the heap is so tight that the
// decoder, its scratch and the codec's DMA all have to fit next to the open TLS
// session, which is why every buffer here is sized exactly.
#include "langstudy_tts_edge.h"

#include "langstudy_audio.h"
#include "langstudy_edge_token.h"
#include "langstudy_net.h"
#include "langstudy_resample.h"
#include "langstudy_ws_frame.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minimp3.h"
#include "mbedtls/base64.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "ls_edge_tts"

#define EDGE_HOST "speech.platform.bing.com"
#define EDGE_PORT 443
#define EDGE_PATH "/consumer/speech/synthesize/readaloud/edge/v1"
#define EDGE_TRUSTED_TOKEN "6A5AA1D4EAFF4E9FB37E23D68491D6F4"
// Must stay in step with EDGE_UA; the server validates the pair.
#define EDGE_GEC_VERSION "2-143.0.3650.75"
#define EDGE_UA                                                              \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "          \
    "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36 Edg/143.0.3650.75"
#define EDGE_ORIGIN "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold"
#define EDGE_VOICE "en-US-AvaNeural"
#define EDGE_AUDIO_FORMAT "audio-24khz-48kbitrate-mono-mp3"

#define EDGE_TLS_TIMEOUT_MS 6000
// Partial MP3 frame assembly. A 24 kHz / 48 kbit/s frame is 144 bytes; the
// buffer holds four of them so the tail of one 720-byte payload plus the start
// of the next still leaves room to validate and decode complete frames.
#define EDGE_MP3_MAX 640
#define EDGE_HEADER_MAX 256
#define EDGE_TEXT_MAX 96

// Play at the rate the service sends. Resampling 24 kHz down to the synthesizer's
// 16 kHz would fold everything above 8 kHz back into 4-8 kHz; the linear
// interpolator we have is not a filter, and measured against a band-limited
// reference the alias is as strong as the signal in the 6-8 kHz band (-0.6 dB),
// which is audible as a hiss riding on every word. At the native rate the
// resampler is a pass-through and the hiss disappears.
#define EDGE_PLAYBACK_RATE 24000
// One MPEG-2 Layer III frame is 576 samples of mono PCM; an MPEG-1 stream
// (1152 samples) also fits, so this buffer cannot overflow.
#define EDGE_PCM_MAX 1152
// 576 input samples at 24 kHz stay 576 (+2); a 1152-sample MPEG-1 frame at
// 44.1/22.05 kHz resamples to 627, which still fits.
#define EDGE_OUT_MAX 640

typedef struct {
    mp3dec_t decoder;
    int16_t pcm[EDGE_PCM_MAX];
    int16_t out[EDGE_OUT_MAX];
    ls_resampler_t resampler;
    uint32_t input_rate;
    size_t written;
    size_t mp3_len;
    uint8_t mp3[EDGE_MP3_MAX];
} edge_decoder_t;

// Both are taken together right after the WebSocket upgrade, where the heap
// still offers one ~32 KB block (scratch 16,256 + decoder ~9.8 KB). Taking them
// after the TLS session closes does not work: the handshake leaves a single
// 20 KB hole, too small for the scratch.
static edge_decoder_t *s_decoder;
static mp3dec_scratch_t *s_scratch;

static void log_alloc_failure(const char *what, size_t bytes)
{
    ESP_LOGW(TAG, "%s 分配失败 (%u 字节), 剩余堆=%u 最大连续=%u", what,
             (unsigned)bytes, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

bool ls_edge_tts_reserve(void)
{
    // Largest first: the decoder then lands in the block the scratch could not
    // fully consume.
    if (!s_scratch) {
        s_scratch = mp3dec_scratch_alloc();
        if (!s_scratch) {
            log_alloc_failure("MP3 解码缓冲", 16256);  // sizeof(mp3dec_scratch_t)
            return false;
        }
    }
    if (!s_decoder) {
        s_decoder = (edge_decoder_t *)calloc(1, sizeof(*s_decoder));
        if (!s_decoder) {
            log_alloc_failure("MP3 解码状态", sizeof(*s_decoder));
            return false;
        }
    }
    return true;
}

void ls_edge_tts_unreserve(void)
{
    free(s_decoder);
    s_decoder = NULL;
    if (s_scratch) {
        mp3dec_scratch_free(s_scratch);
        s_scratch = NULL;
    }
}

static void random_bytes(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) out[i] = (uint8_t)esp_random();
}

static void random_hex(char *out, size_t bytes, bool upper)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper_chars[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_chars : lower;
    uint8_t raw[32];
    if (bytes > sizeof(raw)) bytes = sizeof(raw);
    random_bytes(raw, bytes);
    for (size_t i = 0; i < bytes; i++) {
        out[i * 2] = digits[raw[i] >> 4];
        out[i * 2 + 1] = digits[raw[i] & 0x0F];
    }
    out[bytes * 2] = '\0';
}

static bool tls_write_all(esp_tls_t *tls, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t done = 0;
    while (done < len) {
        ssize_t written = esp_tls_conn_write(tls, bytes + done, len - done);
        if (written <= 0) return false;
        done += (size_t)written;
    }
    return true;
}

static bool tls_read_exact(esp_tls_t *tls, void *data, size_t len)
{
    uint8_t *bytes = (uint8_t *)data;
    size_t done = 0;
    while (done < len) {
        ssize_t got = esp_tls_conn_read(tls, bytes + done, len - done);
        if (got <= 0) return false;
        done += (size_t)got;
    }
    return true;
}

static bool send_text_frame(esp_tls_t *tls, const char *payload)
{
    uint8_t mask[4];
    random_bytes(mask, sizeof(mask));
    size_t len = strlen(payload);
    uint8_t frame[768];
    size_t total = ls_ws_build_frame(LS_WS_OPCODE_TEXT, payload, len, mask,
                                     frame, sizeof(frame));
    return total > 0 && tls_write_all(tls, frame, total);
}

// One WebSocket frame whose payload has not been read yet. Binary audio frames
// can be larger than any buffer we own, so the payload is consumed in chunks.
typedef struct {
    esp_tls_t *tls;
    size_t remaining;
    size_t offset;
    bool masked;
    uint8_t mask[4];
} ws_payload_t;

static bool ws_read_frame_header(esp_tls_t *tls, ws_payload_t *payload,
                                 uint8_t *opcode)
{
    uint8_t head[14];
    if (!tls_read_exact(tls, head, 2)) return false;

    size_t need = 2;
    uint8_t len7 = head[1] & 0x7F;
    if (len7 == 126) need += 2;
    else if (len7 == 127) need += 8;
    if (head[1] & 0x80) need += 4;
    if (need > 2 && !tls_read_exact(tls, head + 2, need - 2)) return false;

    ls_ws_header_t header;
    if (!ls_ws_parse_header(head, need, &header)) return false;
    if (header.payload_len > (uint64_t)SIZE_MAX) return false;

    payload->tls = tls;
    payload->remaining = (size_t)header.payload_len;
    payload->offset = 0;
    payload->masked = header.masked;
    memcpy(payload->mask, header.mask, sizeof(payload->mask));
    *opcode = header.opcode;
    return true;
}

// Read up to `len` payload bytes, unmasking as we go.
static bool ws_payload_read(ws_payload_t *payload, uint8_t *buf, size_t len)
{
    if (len > payload->remaining) len = payload->remaining;
    if (len == 0) return true;
    if (!tls_read_exact(payload->tls, buf, len)) return false;
    if (payload->masked) {
        for (size_t i = 0; i < len; i++) {
            buf[i] ^= payload->mask[(payload->offset + i) % 4];
        }
    }
    payload->offset += len;
    payload->remaining -= len;
    return true;
}

// Consume and discard `len` payload bytes (all of them when len is larger than
// what is left).
static bool ws_payload_discard(ws_payload_t *payload, size_t len)
{
    uint8_t scratch[64];
    if (len > payload->remaining) len = payload->remaining;
    while (len > 0) {
        size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
        if (!ws_payload_read(payload, scratch, chunk)) return false;
        len -= chunk;
    }
    return true;
}

static size_t xml_escape(const char *text, char *out, size_t cap)
{
    size_t used = 0;
    for (const char *p = text; *p; p++) {
        const char *replacement = NULL;
        switch (*p) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&apos;"; break;
        default: break;
        }
        size_t need = replacement ? strlen(replacement) : 1;
        if (used + need >= cap) return 0;
        if (replacement) {
            memcpy(out + used, replacement, need);
            used += need;
        } else {
            out[used++] = *p;
        }
    }
    out[used] = '\0';
    return used;
}

static void timestamp_utc(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, cap, "%a %b %d %Y %H:%M:%S GMT+0000 (Coordinated Universal Time)",
             &tm_utc);
}

// Decode every complete MP3 frame currently in dec->mp3, play it, and keep the
// incomplete tail for the next chunk. Nothing is allocated here.
//
// Frame boundaries come from validated headers, never from minimp3's return
// value: when the buffer ends mid-frame, mp3dec_decode_frame_with_scratch()
// reports frame_bytes as the offset of an *unvalidated* candidate sync word.
// Advancing there desynchronises the stream — measured on the device, it threw
// away about half of every word.
static void drain_mp3(edge_decoder_t *dec, mp3dec_scratch_t *scratch,
                      ls_edge_cancel_fn cancelled)
{
    size_t offset = 0;
    while (dec->mp3_len - offset >= 4) {
        if (cancelled && cancelled()) break;

        const uint8_t *frame = dec->mp3 + offset;
        size_t avail = dec->mp3_len - offset;
        int mono = 0;
        int frame_len = mp3dec_frame_length(frame, &mono);
        if (frame_len <= 0 || !mono) {
            // Payload bytes that merely look like a sync word, or a stereo
            // frame the mono PCM buffer cannot hold. Step over one byte and
            // rescan; a real boundary is never further away than that.
            offset++;
            continue;
        }
        if (avail < (size_t)frame_len) break;   // wait for the rest of it

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame_with_scratch(
            &dec->decoder, scratch, frame, (int)avail, dec->pcm, &info);
        // A validated header fixes this frame's length, whether minimp3
        // managed to decode it or not.
        offset += (size_t)frame_len;
        if (samples <= 0) continue;
        if (info.channels != 1) continue;   // defensive: header said mono

        if ((uint32_t)info.hz == EDGE_PLAYBACK_RATE) {
            // Native rate: the codec is open at it, so the samples go straight
            // through and nothing is resampled.
            if (ls_audio_stream_write(dec->pcm, (size_t)samples) != ESP_OK) {
                offset = dec->mp3_len;
                break;
            }
            dec->written += (size_t)samples;
            continue;
        }
        if ((uint32_t)info.hz < EDGE_PLAYBACK_RATE) {
            // Upsampling needs an output buffer larger than EDGE_OUT_MAX. The
            // service is asked for 24 kHz, so this means the request changed.
            if (dec->input_rate != (uint32_t)info.hz) {
                ESP_LOGW(TAG, "采样率 %d Hz 低于播放率 %d Hz, 跳过", info.hz,
                         EDGE_PLAYBACK_RATE);
                dec->input_rate = (uint32_t)info.hz;
            }
            continue;
        }

        if (dec->input_rate != (uint32_t)info.hz) {
            if (!ls_resampler_init(&dec->resampler, (uint32_t)info.hz,
                                   EDGE_PLAYBACK_RATE)) {
                offset = dec->mp3_len;
                break;
            }
            dec->input_rate = (uint32_t)info.hz;
        }

        size_t out = ls_resampler_process(&dec->resampler, dec->pcm,
                                          (size_t)samples, dec->out,
                                          sizeof(dec->out) / sizeof(dec->out[0]));
        if (out > 0) {
            if (ls_audio_stream_write(dec->out, out) != ESP_OK) {
                offset = dec->mp3_len;
                break;
            }
            dec->written += out;
        }
    }

    if (offset > 0) {
        memmove(dec->mp3, dec->mp3 + offset, dec->mp3_len - offset);
        dec->mp3_len -= offset;
    }
}

// Attach the Edge audio bytes of one binary frame to the MP3 assembler.
static bool stream_audio(ws_payload_t *payload, edge_decoder_t *dec,
                         ls_edge_cancel_fn cancelled)
{
    while (payload->remaining > 0) {
        size_t space = EDGE_MP3_MAX - dec->mp3_len;
        if (space == 0) {
            ESP_LOGW(TAG, "MP3 组帧缓冲溢出");
            return false;
        }
        size_t chunk = payload->remaining < space ? payload->remaining : space;
        if (!ws_payload_read(payload, dec->mp3 + dec->mp3_len, chunk)) {
            return false;
        }
        dec->mp3_len += chunk;
        drain_mp3(dec, s_scratch, cancelled);
    }
    return true;
}

// Open the WebSocket, speak `ssml`, and decode the audio while it arrives. The
// TLS session is always destroyed before returning, whether the exchange
// succeeded or not.
static bool edge_fetch(const char *request, size_t request_len,
                       const char *config, const char *ssml,
                       ls_edge_cancel_fn cancelled)
{
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = EDGE_TLS_TIMEOUT_MS,
        .non_block = false,
    };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return false;
    if (esp_tls_conn_new_sync(EDGE_HOST, (int)strlen(EDGE_HOST), EDGE_PORT,
                              &cfg, tls) != 1) {
        ESP_LOGW(TAG, "TLS 连接失败, errno=%d, 剩余堆=%u", errno,
                 (unsigned)esp_get_free_heap_size());
        esp_tls_conn_destroy(tls);
        return false;
    }

    bool complete = false;
    bool cancel_hit = false;
    bool stream_open = false;
    edge_decoder_t *dec = NULL;

    if (!tls_write_all(tls, request, request_len)) goto done;

    // Read the HTTP upgrade response. The server sends nothing else until we
    // speak, so there are no stray frame bytes to preserve.
    char response[768];
    size_t response_len = 0;
    while (response_len < sizeof(response) - 1) {
        ssize_t got = esp_tls_conn_read(tls, response + response_len,
                                        sizeof(response) - 1 - response_len);
        if (got <= 0) {
            ESP_LOGW(TAG, "握手响应读取失败");
            goto done;
        }
        response_len += (size_t)got;
        response[response_len] = '\0';
        if (strstr(response, "\r\n\r\n")) break;
    }
    if (!strstr(response, " 101 ")) {
        ESP_LOGW(TAG, "WebSocket 升级被拒绝: %.48s", response);
        goto done;
    }

    // Certificate verification is the handshake's peak memory point, so the
    // handshake above ran with the codec released and no decode buffers held.
    ESP_LOGI(TAG, "WS 升级完成,分配解码缓冲: 剩余堆=%u 最大连续=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    if (!ls_edge_tts_reserve() || !s_decoder || !s_scratch) goto done;
    ESP_LOGI(TAG, "解码缓冲已分配: 剩余堆=%u 最大连续=%u, DMA 堆=%u 最大连续=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    dec = s_decoder;
    memset(dec, 0, sizeof(*dec));
    mp3dec_init(&dec->decoder);

    // The codec was released so its DMA buffers did not crowd the handshake.
    // It has to be open before the first frame arrives.
    ls_audio_resume();
    if (ls_audio_stream_begin_rate(EDGE_PLAYBACK_RATE) != ESP_OK) {
        ESP_LOGW(TAG, "音频设备不可用: 剩余堆=%u 最大连续=%u, DMA 堆=%u 最大连续=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        goto done;
    }
    stream_open = true;

    if (!send_text_frame(tls, config)) goto done;
    if (!send_text_frame(tls, ssml)) goto done;

    for (;;) {
        if (cancelled && cancelled()) {
            ESP_LOGI(TAG, "在线语音已取消");
            cancel_hit = true;
            break;
        }

        ws_payload_t payload;
        uint8_t opcode = 0;
        if (!ws_read_frame_header(tls, &payload, &opcode)) break;

        if (opcode == LS_WS_OPCODE_BINARY) {
            // Payload is [2-byte header length][Edge header][MP3 bytes...].
            uint8_t len_bytes[2];
            if (!ws_payload_read(&payload, len_bytes, 2)) break;
            size_t header_len = ((size_t)len_bytes[0] << 8) | len_bytes[1];

            uint8_t edge_hdr[EDGE_HEADER_MAX];
            size_t want = header_len < sizeof(edge_hdr) ? header_len
                                                        : sizeof(edge_hdr);
            if (!ws_payload_read(&payload, edge_hdr, want)) break;
            bool is_audio = false;
            for (size_t i = 0; i + 10 <= want; i++) {
                if (memcmp(edge_hdr + i, "Path:audio", 10) == 0) {
                    is_audio = true;
                    break;
                }
            }
            if (!ws_payload_discard(&payload, header_len - want)) break;

            if (is_audio) {
                if (!stream_audio(&payload, dec, cancelled)) break;
            } else if (!ws_payload_discard(&payload, payload.remaining)) {
                break;
            }
        } else if (opcode == LS_WS_OPCODE_TEXT) {
            uint8_t text[EDGE_HEADER_MAX];
            size_t want = payload.remaining < sizeof(text) ? payload.remaining
                                                           : sizeof(text);
            if (!ws_payload_read(&payload, text, want)) break;
            if (!ws_payload_discard(&payload, payload.remaining)) break;
            bool end = false;
            for (size_t i = 0; i + 8 <= want; i++) {
                if (memcmp(text + i, "turn.end", 8) == 0) {
                    end = true;
                    break;
                }
            }
            if (end) {
                complete = true;
                break;
            }
        } else if (opcode == LS_WS_OPCODE_PING) {
            uint8_t ping[128];
            size_t want = payload.remaining < sizeof(ping) ? payload.remaining
                                                           : sizeof(ping);
            if (!ws_payload_read(&payload, ping, want)) break;
            if (!ws_payload_discard(&payload, payload.remaining)) break;
            uint8_t mask[4];
            uint8_t pong[160];
            random_bytes(mask, sizeof(mask));
            size_t total = ls_ws_build_frame(LS_WS_OPCODE_PONG, ping, want, mask,
                                             pong, sizeof(pong));
            if (total > 0) (void)tls_write_all(tls, pong, total);
        } else if (opcode == LS_WS_OPCODE_CLOSE) {
            complete = true;
            break;
        } else if (!ws_payload_discard(&payload, payload.remaining)) {
            break;
        }
    }

done:
    if (stream_open) {
        // Last chance for a frame that only became decodable at the very end.
        drain_mp3(dec, s_scratch, cancelled);
        ls_audio_stream_end();
    }

    // Best-effort close frame before tearing the TLS session down.
    uint8_t mask[4];
    uint8_t close_frame[8];
    random_bytes(mask, sizeof(mask));
    size_t close_len = ls_ws_build_frame(LS_WS_OPCODE_CLOSE, NULL, 0, mask,
                                         close_frame, sizeof(close_frame));
    if (close_len > 0) (void)tls_write_all(tls, close_frame, close_len);
    esp_tls_conn_destroy(tls);

    if (dec && dec->written > 0) {
        ESP_LOGI(TAG, "在线语音播放完成 (%u 采样)", (unsigned)dec->written);
    }
    return complete && !cancel_hit && dec && dec->written > 0;
}

static bool secure_time_ready(void)
{
    time_t now = time(NULL);
    if (now >= 1600000000) return true;
    if (!ls_net_sta_connected()) return false;
    return ls_net_sync_time(8000) == ESP_OK && time(NULL) >= 1600000000;
}

static bool edge_play_once(const char *text, ls_edge_cancel_fn cancelled)
{
    if (!text || text[0] == '\0') return false;
    if (!ls_net_sta_connected()) return false;
    if (cancelled && cancelled()) return false;
    if (!secure_time_ready()) {
        ESP_LOGW(TAG, "系统时间未同步,无法生成在线语音令牌");
        return false;
    }

    char token[LS_EDGE_GEC_TOKEN_LEN + 1];
    if (!ls_edge_gec_token((uint64_t)time(NULL), token)) return false;

    char escaped[EDGE_TEXT_MAX * 6 + 1];
    if (xml_escape(text, escaped, sizeof(escaped)) == 0) return false;

    char conn_id[33];
    char muid[33];
    char request_id[33];
    random_hex(conn_id, 16, false);
    random_hex(muid, 16, true);
    random_hex(request_id, 16, false);

    uint8_t key_raw[16];
    char key_b64[32];
    size_t key_len = 0;
    random_bytes(key_raw, sizeof(key_raw));
    if (mbedtls_base64_encode((unsigned char *)key_b64, sizeof(key_b64),
                              &key_len, key_raw, sizeof(key_raw)) != 0) {
        return false;
    }
    key_b64[key_len] = '\0';

    char request[1280];
    int request_len = snprintf(
        request, sizeof(request),
        "GET " EDGE_PATH
        "?TrustedClientToken=" EDGE_TRUSTED_TOKEN
        "&Sec-MS-GEC=%s&Sec-MS-GEC-Version=%s&ConnectionId=%s HTTP/1.1\r\n"
        "Host: " EDGE_HOST "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: " EDGE_ORIGIN "\r\n"
        "User-Agent: " EDGE_UA "\r\n"
        "Pragma: no-cache\r\n"
        "Cache-Control: no-cache\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Cookie: muid=%s;\r\n\r\n",
        token, EDGE_GEC_VERSION, conn_id, key_b64, muid);
    if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
        return false;
    }

    char timestamp[80];
    timestamp_utc(timestamp, sizeof(timestamp));

    char config[384];
    int config_len = snprintf(
        config, sizeof(config),
        "X-Timestamp:%s\r\n"
        "Content-Type:application/json; charset=utf-8\r\n"
        "Path:speech.config\r\n\r\n"
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":"
        "{\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":"
        "\"false\"},\"outputFormat\":\"" EDGE_AUDIO_FORMAT "\"}}}}",
        timestamp);
    if (config_len <= 0 || (size_t)config_len >= sizeof(config)) return false;

    char ssml[512];
    int ssml_len = snprintf(
        ssml, sizeof(ssml),
        "X-RequestId:%s\r\n"
        "Content-Type:application/ssml+xml\r\n"
        "X-Timestamp:%sZ\r\n"
        "Path:ssml\r\n\r\n"
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
        "xml:lang='en-US'><voice name='" EDGE_VOICE "'><prosody pitch='+0Hz' "
        "rate='+0%%' volume='+0%%'>%s</prosody></voice></speak>",
        request_id, timestamp, escaped);
    if (ssml_len <= 0 || (size_t)ssml_len >= sizeof(ssml)) return false;

    ESP_LOGI(TAG, "连接前堆=%u 最大连续=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    bool spoken = edge_fetch(request, (size_t)request_len, config, ssml,
                             cancelled);
    ESP_LOGI(TAG, "TLS 关闭后堆=%u 最大连续=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return spoken;
}

bool ls_edge_tts_play(const char *text, ls_edge_cancel_fn cancelled)
{
    // Hand the codec's DMA memory (~8 KB) back for the TLS handshake. The
    // wrapper resumes on every exit so the offline fallback can still speak.
    (void)ls_audio_release();
    bool ok = edge_play_once(text, cancelled);
    // Give the buffers back before the next word: the following TLS handshake
    // needs the same contiguous space again.
    ls_edge_tts_unreserve();
    ls_audio_resume();
    return ok;
}
