// main/langstudy_tts_edge.h
// Online pronunciation through Microsoft Edge's Read Aloud endpoint.
//
// The service speaks with a neural voice and streams 24 kHz mono MP3 over a
// WebSocket. We decode it with minimp3, resample to the 16 kHz playback rate
// and stream straight to the codec, so no whole-phrase PCM buffer is needed.
//
// The protocol is undocumented and the "Sec-MS-GEC" token is tied to a client
// version. ls_edge_tts_play() therefore returns false on every failure and the
// caller must fall back to the offline synthesizer.
#pragma once

#include <stdbool.h>

// Optional cancellation probe, checked between frames. NULL means never
// cancelled.
typedef bool (*ls_edge_cancel_fn)(void);

// ls_edge_tts_play() manages these itself: the minimp3 scratch and the decoder
// are both taken right after the WebSocket upgrade, while the heap still has
// one block large enough for them (~29 KB). Callers only need
// ls_edge_tts_unreserve(), which is safe when nothing is reserved.
bool ls_edge_tts_reserve(void);
void ls_edge_tts_unreserve(void);

// Speak one short phrase. Blocks until playback finishes, fails, or the cancel
// callback returns true. Returns true only when PCM reached the speaker.
bool ls_edge_tts_play(const char *text, ls_edge_cancel_fn cancelled);
