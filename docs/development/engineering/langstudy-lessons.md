<p align="right">
  <a href="langstudy-lessons.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Lessons learned building the LangStudy app

`PASSPORT ENGLISH` is an offline-first English study application for the AI
Passport board: an ESP32-C3 with 8 MB of flash, no PSRAM, a 240 x 320 SPI
display, and three buttons. It stores vocabulary packs and learning progress on
the device, optionally generates new packs through an HTTPS LLM API, speaks
words with online or offline synthesis, and optionally syncs progress to Azure
Blob Storage.

The feature reference is [langstudy-app.md](langstudy-app.md). This document
collects the engineering lessons that transfer to other constrained,
networked, content-generating device apps. Numbers come from the shipped
firmware and from failure captures on real hardware, not from estimates.

## 1. Let the hardware pick the architecture

Every early design choice follows from a constraint. Writing the budget down
first prevented most of the rework:

| Constraint | Consequence |
| --- | --- |
| Single core, no PSRAM, a few hundred KB of SRAM | No large buffers, no decoded images, no "allocate and sort later". Use fixed BSS objects, streaming, and one owner per buffer. |
| LVGL is not thread-safe | Every access outside the LVGL task takes `bsp_lvgl_lock()`; button callbacks never block. |
| 8 MB flash but tiny RAM | Fonts, splash art, and generated assets live in flash as C arrays; the active pack lives in flash storage, not in NVS. |
| 240 x 320, three buttons | One-line header, modal full-screen status pages, short labels, and explicit "what to press next" hints. |
| Battery power, no wall clock | Serialized state is written after every answer; scheduling is relative to completed sessions; SNTP is used only for TLS. |
| Offline-first product promise | A built-in pack and an offline speech synthesizer are mandatory fallbacks, not extras. |

**Lesson.** Write the memory and interaction budget before choosing libraries.
On this class of device, the feature list is bounded by the largest contiguous
free block, not by the flash size.

## 2. Keep the logic host-testable

The code is split into four layers:

- `components/bsp`: display, buttons, battery, audio, storage, sleep.
- Pure logic in `main/langstudy_*.c`: pack and progress formats, scheduling,
  phoneme handling, IPA conversion, synthesis, JSON extraction, prompt
  construction, setup-form validation.
- Application: boot order, input task, screens, and the state machine.
- Services: provisioning, HTTPS generation, Azure sync, background work queue.

The pure layer builds and runs on a workstation without ESP-IDF or LVGL. That
made the expensive parts cheap to test: format round-trips, CRC rejection,
scheduling edge cases, IPA conversion, token derivation, and sanitizer
behavior are covered by host tests. The device-specific code stays thin enough
to review by inspection.

**Lessons.**

- If code can be pure C, make it pure C. Host tests run in milliseconds and
  catch contract regressions that are painful to reproduce on a board.
- Give each background worker a message queue and a single owner for every
  heap buffer. The worker allocates and posts; the UI consumes and frees.
  Retry paths must free before they allocate again.
- Test the parser and the pre-validator with the same corpus. Here a fixed
  slice scanner and the full parser enforce identical rules in the same order,
  so "scan OK" guarantees "parse OK".

## 3. Memory is the hard problem

### 3.1 "Free heap" is not "allocatable memory"

A TLS session on this part leaves tens of kilobytes free but often only a few
kilobytes contiguous. The reported failure was:

```text
out of memory for parsed pack (15668 bytes, free=76684)
```

A 15.7 KiB allocation failed with 76 KiB free. Earlier firmware hit the same
wall at 25 KiB with 101 KiB free. Total free bytes are a misleading health
signal after TLS; the largest free block is the number that predicts success.

**Lessons.**

- Log both `esp_get_free_heap_size()` and
  `heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)` on every allocation
  failure.
- Treat longest-lived allocations as static. The active pack, scan indexes,
  and caches are BSS objects of known size.
- Never assume a successful handshake means the heap is healthy.

### 3.2 Do not allocate content-sized objects after TLS

The first design parsed the reply into a content-sized staging model after the
radio was torn down. The second used a ~15.7 KiB `ls_gen_pack_t`. Both failed
on long replies because the allocation happened at the most fragmented moment.

The final design makes the reply buffer the **only** content-sized
allocation. It is created while TLS is live and the response is streamed into
it (16 KiB plus 512 bytes of wrapper headroom). Once the reply is complete, the
worker validates it with a fixed ~2 KiB slice index in BSS and hands the text
buffer itself to the UI. The UI re-scans, remaps progress by word text, parses
directly into the already-resident 25 KiB pack, and frees the text.

**Lessons.**

- Allocate the big buffer before the transport can fragment the heap, and
  stream into it. Do not build a second model next to it.
- Pass ownership of the one buffer through the queue; free it exactly once.
- Leave room for the protocol wrapper (`{"pack":"..."}`) in the buffer size.

### 3.3 Stream in both directions

The HTTP worker requests server-sent events and appends each `delta.content`
fragment directly into the pack buffer. The raw JSON body is never resident.
The request body is never materialized either: its `Content-Length` is computed
with the same escaping rules and the bytes are written from a 256-byte stack
buffer.

Streaming also changes timeout semantics: the per-read timeout only has to
cover the gap between deltas, not the whole completion. The server can queue a
request for more than a minute before the first event, so the timeout is 120 s
and still does not cap a generation that is making progress.

**Lesson.** For any response that is "large relative to RAM", stream it and
validate incrementally. For requests, compute the length and stream the body
instead of building a string.

### 3.4 Wi-Fi and mbedTLS compete for the same heap

This was the hardest bug in the project. A CET6 refresh failed mid-stream with
`ESP_FAIL` after an arbitrary number of content bytes (5677 and 2242 observed),
and `errno` was 0. The serial log showed:

```text
esp-aes: Failed to allocate memory
esp-tls-mbedtls: read error :-0x7180
```

mbedTLS reported `MBEDTLS_ERR_SSL_INVALID_MAC`, but the root cause was an
allocation failure in the AES-DMA decrypt path: the heap was too squeezed to
give the driver a DMA-capable buffer. The read error was a downstream symptom.

The fixes that mattered:

- Enable mbedTLS dynamic record buffers so RX/TX record storage is freed after
  each record instead of held for the whole session.
- Keep the RX record at 8 KiB: a larger certificate flight can arrive in one
  record, but 8 KiB is enough for this endpoint.
- Trim the Wi-Fi static RX pool (4 buffers here, stock 10). Static buffers are
  allocated at `esp_wifi_init` and never freed, so they are permanently
  unavailable to TLS, the AES-DMA staging path, and the pack buffer. With 8,
  an IELTS refresh had 6,272 B free / 2,048 B largest and 4,068 B DMA free /
  1,152 B DMA-largest; the decrypt path needed a 1,600-byte staging block, and
  a second attempt could not allocate the 16,896-byte pack buffer from a
  14,336-byte largest block. With 4, four consecutive IELTS refreshes opened
  TLS with ~57-58 KB free / 30,720 B largest, allocated the pack buffer every
  time, and kept 8-9 KiB DMA-largest during streaming.
- Cap the Wi-Fi dynamic RX/TX pools below the stock 32 (8 + 8 here). While an
  SSE reply streams, the live set is the 16.9 KiB pack buffer, the dynamic TLS
  record buffer, and one Wi-Fi RX buffer per in-flight frame; the AES-DMA
  driver needs its own allocations. Stock pool sizes starved the decrypt path.
- Disable Wi-Fi power save while the radio is up; modem-sleep latency looked
  like read timeouts mid-generation.
- On a read failure, log the raw `esp_http_client_read()` return value,
  `esp_http_client_get_errno()`, and
  `esp_http_client_get_and_clear_last_tls_error()`, plus free and largest
  DMA-capable blocks.

**Lessons.**

- `errno == 0` does not mean "no error". For mbedTLS failures, fetch the TLS
  error code explicitly.
- A read failure can be an allocation failure in a lower layer. Log DMA-class
  heap, not just the default heap.
- Buffer tuning is part of protocol correctness on a narrow-memory MCU.

### 3.5 Allocation order matters

For Edge TTS, TLS is established *before* the ~16.5 KiB MP3 decode buffer is
allocated, so the handshake gets the largest contiguous block. For the pack
reply, the buffer is allocated before streaming, so the transport writes into
it without growing the heap. The right order depends on which operation needs
contiguity at which moment.

**Lesson.** Draw the heap timeline for each network operation: handshake,
headers, body, decode, parse, UI handoff. Allocate at the low-fragmentation
points, free at the earliest safe point, and keep everything else fixed-size.

## 4. Treat generated content as untrusted input

An LLM reply is probabilistic input from a remote service. The validator, not
the prompt, is the guarantee.

The request pins the contract: one JSON object with a single `pack` string, six
`LESSON` lines and sixty complete `WORD` lines, nine pipe-separated fields per
word, ASCII fields, a Chinese translation, ARPAbet pronunciation, a varied
sample, and an explicit exclusion list. Every request carries a fresh seed and
an English level that rises with progress.

The device then repairs instead of rejecting wholesale:

- Trim each field to its format cap.
- Convert common Unicode punctuation to ASCII.
- Drop incomplete lines and model-supplied `END` lines.
- Rewrite the header counts and append a fresh CRC.
- Repair pronunciation: if the value is not ARPAbet, try IPA-to-ARPAbet; if it
  is still unusable, drop the WORD line.
- Drop words with an empty word, meaning, Chinese translation, or
  pronunciation.
- Accept the reply if at least 30 usable words remain; retry once only for a
  malformed or truncated reply, with a stricter prompt and a re-mixed seed.

**Lessons.**

- Design for partial success. A short reply that still yields a usable session
  is better than making the learner wait for another generation.
- Keep the scanner and the parser in lockstep, and re-validate on the UI side
  before touching the active pack. A rejected reply must leave the current
  pack and progress untouched.
- Distinguish "format-valid" from "usable". `ls_pron_validate("")` deliberately
  returns true for backward compatibility with older packs, but an empty
  transcription produces a blank phonetic line and a silent card; the
  generation path now drops such lines. Do not let a compatibility rule leak
  into new data.
- Report quality problems instead of hiding them. Reused vocabulary is not a
  correctness failure, so it is accepted and surfaced as
  `SOME WORDS REPEATED`; the avoid history prevents the same words from being
  offered again. One capture still showed 60 of 60 words repeated, which is a
  product-quality signal that deserves its own metric, not a silent retry.
- Retry policy should match the failure class: malformed output is worth one
  more call; connectivity, authentication, and memory failures are not.

## 5. Persistence: choose the atomicity unit up front

- Packs and progress are line-oriented text with a reflected CRC32. Parsers
  reject a bad checksum, a missing final newline, `CR`, `NUL`, duplicate IDs,
  and out-of-range values.
- The pack uses two flash slots: write the inactive slot, commit, then flip the
  index. A power loss leaves the previous pack authoritative; a corrupt slot is
  skipped; if both fail, the built-in pack loads.
- Progress records are keyed by stable word IDs, so reordering lessons or
  editing text does not lose history. A refresh remaps records by trimmed word
  text and drops words that disappeared.
- Progress and buddy state are written after every answer, so a power loss
  loses at most the current card.
- NVS holds small settings and a 1600-byte avoid history. Bulk content lives in
  flash storage. NVS is a key-value store, not a document store.
- Sync is read-merge-write with per-record revisions; the local side wins ties,
  and one 20 KiB buffer is reused for GET and PUT. The single-device assumption
  is documented rather than hidden behind an ETag loop.
- Only the battery state of charge crosses deep sleep (RTC memory); it is
  trusted only when the measured voltage is plausible, so a sleeping gauge
  answering `0x00` is not painted as `0%`.

**Lessons.** Decide the recovery story before writing the first byte: what is
atomic, what is authoritative after a power loss, and what the fallback is.
Then test the corrupt and truncated cases on the host.

## 6. UX for a tiny screen and three buttons

- Button callbacks enqueue work; they never block. Long operations show a
  full-screen status page with `PLEASE WAIT...` and a clear next action.
- Check prerequisites before starting a doomed request. Missing setup produces
  `WI-FI OR DEEPSEEK NOT SET UP` (or `(SETUP)` on the menu) instead of a
  network timeout.
- Report connectivity per hop: Wi-Fi, clock, then the API. A single opaque
  failure wastes the learner's time and the developer's.
- Show raw diagnostics behind `DOWN: DETAIL` rather than on the happy path.
- Link icons show the last proven state, never a live socket. They start
  hidden after every boot and are re-proven by a silent probe, because a
  pre-reset result says nothing about the current network.
- The five-second launch screen covers initialization and hands over to the
  study screen without a black frame; a key wake skips it.
- Auto-play loops only within the current lesson, shows `AUTO n / m`, exits on
  any key without scoring, and never persists across a reboot.
- Text is short and explicit; the header is one line. Verify every new label on
  the device.

**Font lessons.** The default display font is ASCII-only. Chinese translations
need a generated font covering every GB2312 glyph (7540 glyphs, ~906 KiB of
bitmap data) plus the right widget font. IPA needs its own generated font.
At 16 px, some DejaVu glyphs (small-capital I, rhotic schwa) look like missing
glyphs, so `ɪ` is displayed as `i` and `ER` as `ər`; a successful UTF-8 build
does not prove the glyph is legible. Long transcriptions scroll instead of
wrapping.

## 7. Networking details that bite

- Run SNTP before TLS: certificate validation and Azure's `x-ms-date` header
  both need a plausible clock.
- Prefer a cheap probe for status. `GET /models` proves the key and connectivity
  without consuming generation quota.
- Every network feature needs an offline fallback: built-in pack, local speech
  synthesizer, local progress.
- Radio teardown after each operation saves power but makes every online action
  pay the connect cost. Edge TTS keeps the link for 60 seconds after the last
  request so consecutive words do not reconnect each time.
- Capability gaps are normal. IDF 5.5 did not bundle a WebSocket client here,
  so the app carries a small RFC 6455 implementation over `esp_tls`. The Edge
  token algorithm is tied to a specific client version; the token code and user
  agent must be bumped together, and fixed test vectors protect the
  implementation.
- Credentials are write-only fields: blank keeps the saved secret, a checkbox
  deletes it, and the value is never logged or shown back. The Azure SAS is
  scoped to one container, HTTPS-only, and has a finite expiry.
- Accept that media servers can return short or incomplete audio; keep the
  offline synthesizer as a real fallback and treat implausible response sizes
  as a diagnostic signal.

## 8. Debugging and testing on real hardware

- Reproduce before changing code. The CET6 failure was only explainable after
  capturing the full serial log: the read error, the AES allocation line, and
  the TLS code together identified the layer that actually failed.
- Instrument component boundaries: request size, HTTP status, bytes received,
  raw read return value, TLS error, heap and DMA free/largest, decoded sample
  count, and pack CRC. One informative log line is worth a day of guessing.
- Keep host tests for all pure logic and parity tests for scanners and parsers.
  Add fixed vectors for cryptographic or token algorithms.
- Use a device acceptance checklist per feature, and verify persistence by
  flashing segmented images that preserve NVS (`0x0` bootloader, `0x8000`
  partition table, `0x30000` application on this layout). After flashing,
  check the boot log for splash timing, stored pack CRC, and buffer counts.
- Watchers and capture scripts make hardware work repeatable: wait for the USB
  port, reset through DTR/RTS, stop at a known marker, and keep the whole log.
- Report `Build`, `Host tests`, `Device tests`, and `Unverified` separately. A
  successful build is not hardware validation.

**Workflow gotchas.** A shared Windows/WSL tree can fail on CRLF shebangs; mount
the Windows drive explicitly, normalize shell scripts, and keep the build tree
owned by the build user. Keep a TODO/status document with evidence tables and a
"resume by" procedure so a context switch does not lose the investigation.

## 9. Documentation and generated assets

- Keep one source of truth for AI and contributor rules, and link to it instead
  of duplicating it.
- Maintain English and Chinese documents as aligned pairs with reciprocal
  links; when a user-visible string changes, update both.
- Record failure signatures verbatim: `esp-aes: Failed to allocate memory`,
  `read error :-0x7180`, the byte counts, the free/largest numbers. These
  details are what make a later diagnosis fast.
- Generate fonts, splash art, and other binary assets with checked-in scripts
  (`tools/gen_*.py`), and regenerate rather than hand-edit the generated C.
  Document the source font and its license.
- Never put credentials, QR secrets, personal data, or unsanitized logs in the
  repository or documentation.

## 10. What we would do differently

- Measure generated-content quality, not just format validity. The repeated
  vocabulary rate deserves a counter and a threshold; accepting a pack that
  repeats the entire avoid list is correct for availability but poor for
  learning.
- Write a heap budget per phase (boot, idle, TLS handshake, streaming, TTS,
  sync) and assert it in a diagnostic build. The largest-block failure would
  have been caught before the first device test.
- Make required fields required from day one. The empty-pronunciation acceptance
  was a backward-compatibility decision that later produced silent cards.
- Build a repeatable multi-refresh soak test with heap watermarks instead of
  discovering stream failures one refresh at a time.
- Consider reusing one WebSocket for consecutive TTS words, and add a
  plausibility check for short audio responses. This remains an open issue.

## Checklist for a similar app

1. Write the phase-by-phase RAM budget and the largest-block target.
2. Define which objects are static, which are streamed, and who frees them.
3. Make all protocol and format logic host-testable before touching hardware.
4. Treat every generated document as untrusted: repair, drop, count, and
   validate with the same rules on both sides of the handoff.
5. Choose the persistence atomicity unit and the power-loss recovery path.
6. Instrument each network hop and each allocation failure at the boundary.
7. Provide offline fallbacks for content, voice, and sync.
8. Verify strings, glyphs, and layout on the actual panel.
9. Keep an acceptance checklist and report build, host, device, and unverified
   results separately.
10. Record the numbers and failure signatures that mattered; they are the most
    reusable part of the experience.

## Numbers that mattered

| Value | Why it mattered |
| --- | --- |
| 15,668 B / 76,684 B free | The parsed-pack allocation that failed; proof that total free memory is not the metric. |
| 16,896 B | Pack reply buffer: 16 KiB plus 512 B wrapper headroom, streamed over SSE. |
| ~2 KiB | Fixed slice index used to validate the reply without a model allocation. |
| 25 KiB | Resident active pack parsed directly by the UI thread. |
| 8,192 / 4,096 B | mbedTLS RX/TX record lengths. |
| 5,760 B | TCP receive window; bounds how many Wi-Fi frames can be in flight. |
| 4 + 8 | Wi-Fi static + dynamic RX buffer caps (TX is also 8). Static buffers are never freed, so cutting them 8 → 4 returned 6,400 B. |
| 1,152 B | DMA-largest that starved the AES-DMA staging path before the static RX pool was trimmed. |
| 30,720 / 8,192 B | Largest free block before TLS / DMA-largest after the pack allocation, across four consecutive successful IELTS refreshes. |
| 60 / 6 / 30 | Generated words, generated lessons, and minimum usable words accepted. |
| 30 s / 300 s | Screen dim and deep-sleep idle thresholds. |
| 60 s | Edge TTS link keep-alive after the last request. |
| 16.5 KiB | MP3 decode buffer, allocated after the TLS handshake. |
| 1,600 B | NVS avoid-history budget. |
| 7,540 glyphs | GB2312 CJK font coverage, ~906 KiB of bitmaps in flash. |
