<p align="right">
  <a href="langstudy-app.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# LangStudy application

`PASSPORT ENGLISH` is an offline-first English study application for the AI
Passport hardware. Progress lives on the device and survives power loss. When a
Wi-Fi network is configured, the user may optionally refresh the word pack and
sync progress to Azure Blob Storage.

The application replaces the demo menu and demo pages. `main/main.c` only calls
`ls_app_start()`; all input and state handling lives in `langstudy_app.c`, and
the notebook-style screens live in `langstudy_ui.c`. Hardware access stays in
`components/bsp`.

## Constraints

- ESP32-C3, 8 MB flash, no PSRAM, ESP-IDF 5.5.3.
- 240 x 320 ST7789P3 display over SPI.
- Three ADC-ladder buttons on GPIO0 (UP, OK, DOWN); no touch screen.
- LVGL 9.5 with a single 40-line DMA buffer and a 24 KB pool.
- ASCII-only display font: content and every generated field reject non-ASCII
  text, `|`, and control characters.
- The built-in pack has exactly 100 words in 10 lessons. A generated pack
  requests 60 words in 6 lessons; the parser accepts any count from 1 to
  `LS_WORDS_MAX` (100) so the built-in pack and packs stored by older firmware
  remain valid.

## Architecture

| Layer | Location | Responsibility |
| --- | --- | --- |
| BSP | `components/bsp` | Display, buttons, battery, audio, I2C, storage wrappers, sleep preparation. |
| Pure logic | `main/langstudy_*.c` | Pack/progress parsers, scheduling, phonemes, synthesis, JSON, prompt extraction, setup-form validation. Host-testable without ESP-IDF. |
| Application | `main/langstudy_app.c`, `main/langstudy_ui.c` | Boot order, input task, screens, deep sleep, orchestration of online results. |
| Services | `main/langstudy_provision.c`, `langstudy_deepseek.c`, `langstudy_sync.c`, `langstudy_online.c` | SoftAP setup page, DeepSeek HTTP, Azure progress sync, background work queue. |

Pack and progress mutation happens on the application input task. Background
services post heap-owned results through `ls_online_poll()`, and the UI frees
them after merging.

## Controls

| Input | Action |
| --- | --- |
| UP short | Hear the current word again. |
| OK short | "I knew it": count as correct and advance. |
| DOWN short | "Show it again": count as wrong and advance. |
| DOWN long | Start or stop auto-play for the current lesson. |
| OK long | Open the menu. |

Auto-play loops only the words whose `lesson_id` matches the word on screen when
it starts, so the loop stays inside the current lesson even when the pack holds
several lessons. The header switches to `AUTO <n> / <m>` and a thin bar above
the help text drains over the configured dwell time; the next word replaces the
card when the bar empties. The dwell defaults to 5 seconds and is set with
`WORD DELAY` in `SETTINGS`. Any key press (including a short DOWN) leaves
auto-play without scoring; a long OK leaves auto-play and opens the menu. The
mode is never persisted across a reboot.

Menu entries: `REFRESH WORDS`, `PROGRESS`, `BUDDY`, `CREATE PROFILE`,
`CONNECTION CHECK`, `SETTINGS`, `PHONE SETUP`, `NEW SESSION`, `RESET PROGRESS`,
`BACK`. Choosing `REFRESH WORDS` opens a vocabulary-tier list (`CET4`, `CET6`,
`IELTS`). Entries that need a network show `(SETUP)` and explain what is missing
instead of failing silently.

`PROGRESS` is always available, even offline. It shows the completed session
count, the answered-card count, the accuracy percentage, how many words in the
current pack have been studied, and the completion percentage. `SYNC NOW` is
offered only when Wi-Fi, an Azure container SAS URL, and a profile exist;
otherwise the row reads `SYNC NOW (SETUP)` and explains what is missing instead
of starting a doomed request. Failed status screens show `DOWN: DETAIL` when the worker recorded a
diagnostic, and pressing DOWN opens an error-detail page with the HTTP status,
usable-byte count, repeated words, or HTTPS error.

The study screen header is a single line: `PASSPORT ENGLISH`, a vertical battery
bar (filled from the bottom in proportion to the charge; green, amber below 30%,
red below 15%, and an empty outline when the fuel gauge has never reported a
value), a Wi-Fi icon, and a `D` badge for DeepSeek. The Wi-Fi and `D` icons
appear once a check or refresh has proven each hop. Both stay hidden until a
result is known, and a later offline operation does not clear a green icon
learned from an earlier check. They report the last known state rather than a
live connection: the radio is torn down after every online operation.

Deep sleep is a full reboot, so these three values are kept in RTC memory and
redrawn immediately on wake: the link icons from the last successful online
operation and the battery fill from the reading taken just before the gauge was
put to sleep. The fill is then replaced by the first valid post-wake reading
(the UI retries every 2 s until the gauge finishes its first conversion). A
power cycle or reflash clears RTC memory, so the header starts from "unknown"
there. The gauge is trusted only when the measured cell voltage is plausible
(at least 2500 mV), so a sleeping or just-restarted chip that answers `0x00` is
not painted as `0%`.

## Profile and digital buddy

`CREATE PROFILE` generates a random profile identity and pairs it with a digital
buddy. The screen cycles through eight preset names with UP/DOWN; OK creates a
UUIDv4-style 36-character id and a buddy of a random species, stores both, and
opens the BUDDY screen. With an existing profile the screen only reports the id,
because it names the sync folder and replacing it silently would orphan the
remote copy; `RESET PROGRESS` removes the profile and its buddy.

The buddy has four species (`BLOB`, `KIT`, `PIPLING`, `DRAKE`) and four forms.
It starts at form 1 and evolves when XP reaches 300, 900, and 2000, for three
evolutions total. Each evolution unlocks a skill:

| Form | Skill | Effect |
| --- | --- | --- |
| 1 | none | — |
| 2 | `FOCUS` | +10% XP per correct answer |
| 3 | `ECHO` | repeats a missed word aloud |
| 4 | `INSIGHT` | +20% XP (stacking with `FOCUS`) |

A correct answer awards `10 x English level` XP, so words from higher levels
(1–10 in `SETTINGS`) are worth more. A happy buddy (energy at least 60) adds
25% on top. Correct answers restore 2 energy; wrong answers cost 2, and each
completed session costs 5. A session with at least 70% accuracy earns one snack
(up to 999). Feeding consumes one snack, restores 25 energy, and is done with
OK on the BUDDY screen; HOLD OK returns to the menu.

`BUDDY` draws the 24x24 palette-index sprite at 2x in a canvas, then shows the
name and species, current form, XP toward the next evolution, energy, snacks,
and the active skill. Answers, evolutions, and snacks for the current session
are summarized on the session-complete screen. Buddy state is saved after every
answer, so a power loss loses at most the current card.

The profile id names a virtual folder in Azure Blob Storage. `SYNC NOW` writes
`profiles/<profile id>/progress.lsprog` in the container configured on the Phone
Setup page; see [Progress sync](#progress-sync).

## Data formats

Both formats are line-oriented text with a CRC32 (reflected, polynomial
`0xEDB88320`) in the `END` line. Writers always recompute the CRC; parsers
reject a bad checksum, a missing final newline, `CR`, `NUL`, duplicate IDs, and
out-of-range values.

### Word pack

```text
LSPACK|2|<lesson_count>|<word_count>
LESSON|<id>|<title>
WORD|<id>|<lesson_id>|<text>|<pos>|<meaning>|<translation>|<example>|<pron>
END|<CRC32>
```

`<translation>` is UTF-8 Chinese and is required; the pack is rejected if a
word's translation is missing or contains no non-ASCII code point. All other
fields stay printable ASCII. `<pron>` is a space-separated ARPAbet string such
as `HH AH L 'OW`; a leading apostrophe marks primary stress. Version 1 packs
are rejected, and the device falls back to the built-in CET pack.

The study card renders `<pron>` as IPA (`/həˈloʊ/`) with
`main/langstudy_ipa_16.c`, a generated 16 px font covering ASCII plus the IPA
symbols the converter emits. Regenerate it with `tools/gen_ipa_font.py`
(DejaVu Sans by default). `ls_pron_to_ipa()` in `langstudy_phoneme.c` moves the
stress mark to the syllable onset and maps unstressed `AH` to `ə`.
For legibility at 16 px, the near-close front vowel `ɪ` (U+026A) is displayed as
plain `i`, so `AY`/`EY`/`OY` read as `ai`/`ei`/`ɔi` and `IH` as `i`; `IY`
still shows the length mark (`iː`). `ER` is always displayed as `ər` rather than
`ɝ`/`ɚ`: those rhotic schwa glyphs are only 9 px tall and read as an empty bar at
this size, which is what `desert` (`/ˈdɛzərt/`) and `glacier` (`/ˈgleiʃər/`)
exposed. Without these substitutions, DejaVu's small-capital I and rhotic schwa
look like missing glyphs. Long transcriptions scroll horizontally instead of
wrapping.

Translations are rendered with `main/langstudy_cjk_16.c`, a generated 4 bpp
LVGL font covering ASCII plus every GB2312 character (7540 glyphs, ~906 KiB of
bitmap data). Regenerate it with `tools/gen_cjk_font.py`; the default source is
Noto Sans SC under the SIL Open Font License. The bitmap-index limit is 1 MiB,
so the generator refuses any inventory that would overflow the 20-bit glyph
index.

### Progress

```text
LSPROG|1|<session_count>|<answered_total>|<correct_total>|<record_count>
WORD|<word_id>|<box>|<correct>|<wrong>|<revision>|<due_session>
END|<CRC32>
```

Up to 256 records are kept. Records are keyed by the stable word ID so that
reordering lessons or editing a word's text does not lose history.

## Persistence

- NVS namespace: `langstudy`.
- Keys: `wifi_ssid`, `wifi_pass`, `api_key`, `sync_url` (Azure container SAS
  URL), `exam`, `level`, `bright`, `volume`, `word_delay`, `avoid_hist`,
  `prof_id`, `buddy`.
- Pack storage uses two slots. A new pack is written to the inactive slot,
  committed, and then the index flips, so a power loss leaves the previous pack
  authoritative. A corrupt slot is skipped on boot.
- If both slots fail, the device falls back to the built-in 100-word pack
  (`langstudy_builtin.c`).
- Progress is written after every answer and after each completed session.
- The buddy record follows the same line-plus-CRC convention:
  `BUDDY|1|<species>|<name>|<form>|<energy>|<snacks>|<xp>|<revision>` then
  `END|<CRC32>`. It is written after every answer and every feed. A corrupt
  record is ignored; if the profile id survives, a fresh buddy is generated so
  the menu is never stuck.

## Scheduling

Spaced repetition uses boxes 0 to 4 with intervals `{0, 1, 3, 7, 14}`:

```text
due_session = session_count + 2 + interval[box]
```

A correct answer advances one box (up to 4); a wrong answer resets to box 0. A
session deals 10 to 20 cards and increments `session_count` only when it
finishes. Because there is no real-time clock, all scheduling is relative to
completed sessions rather than wall-clock dates.

The builder ranks three groups: due reviews (oldest due first), unseen words
(taken from a rotating window so consecutive sessions do not start at the same
word), and words that are not due yet. `NEW SESSION` passes the previous
session's word IDs as an exclusion list, so two consecutive sessions contain
different sets. Previous words are reused only when fewer than 10 other words
remain, which keeps the session usable instead of returning a short set or
`NO CARDS`. The exclusion list lives in RAM, so a reboot starts a fresh
rotation.

## Pronunciation

`ls_voice_speak()` (`langstudy_voice.c`) picks the best available voice for each
word:

- **Online (preferred).** The word text is sent to Microsoft's Edge Read Aloud
  neural TTS and the returned MP3 is decoded and streamed as it arrives
  (`langstudy_tts_edge.c`). The voice is `en-US-AvaNeural` at
  `audio-24khz-48kbitrate-mono-mp3`, the only format this endpoint accepts.
  Because the application normally tears the radio down after each online
  operation, the voice worker reconnects with the stored Wi-Fi credentials on
  the first tap and keeps the link up for 60 seconds after the last request, so
  consecutive words do not pay the connect cost again.
- **Offline fallback.** When no Wi-Fi is configured, the link or the request
  fails, or the word has no text, the local formant synthesizer
  (`langstudy_synth.c`) speaks the stored ARPAbet phonemes instead. Words
  without a stored pronunciation are silent offline but still spoken online,
  because the neural path uses the word text.

The online path runs on a dedicated worker task (12 KB stack) so the UI never
blocks. Tapping another word cancels the current sentence and replaces the
queued one; the offline audio worker drops its queued word while an online
stream owns the hardware.

Online transport details, all in `langstudy_tts_edge.c`:

- A manual WebSocket client over `esp_tls` (`langstudy_ws_frame.c` builds and
  parses the RFC 6455 frames), because IDF 5.5 does not bundle
  `esp_websocket_client`.
- The `Sec-MS-GEC` token is the v2 CS64 + modified-SHA-256 value from
  edge-tts 7.3.1. `langstudy_edge_token.c` reproduces it bit-exactly and is
  covered by fixed test vectors; it is tied to the client version
  `2-143.0.3650.75` and the matching `Edg/143.0.3650.75` user agent, so both
  must be bumped together if Microsoft changes the check.
- The MP3 stream is decoded by the vendored minimp3 decoder
  (`components/minimp3/`, CC0), mixed to mono, and resampled 24 kHz → 16 kHz by
  the streaming linear resampler in `langstudy_resample.c`.
- TLS is established *before* the ~16.5 KB decode buffer is allocated so the
  handshake gets the largest contiguous heap block. Any failure returns `false`
  and the caller falls back to the offline voice.

The offline synthesizer uses a spectrally tilted glottal source (rather than a
raw sawtooth), four resonators, wider bandwidths and a direct noise component
for fricatives, stop closure/burst/aspiration, smoother formant and pitch
glides, final lengthening, and a DC blocker. UP replays the current word; the
application stops playback before sleeping.

## Phone provisioning

The `PHONE SETUP` menu item starts a SoftAP:

1. The device shows the SSID `LangStudy-XXXX`, a password `learnXXXX`, and the
   page address `http://192.168.4.1`.
2. The phone joins the AP and opens the page.
3. The page collects Wi-Fi SSID/password, the DeepSeek API key, and an optional
   HTTPS container SAS URL for Azure Blob Storage. The SAS field accepts up to
   512 characters and is write-only, like the API key: leaving it blank keeps
   the saved URL, and a checkbox deletes it. The URL is a credential and is
   never logged or shown back.
4. The device verifies the station connection before writing anything to NVS.
   A failed test reports the reason and stores nothing.
5. When Wi-Fi was configured, the device immediately runs the connection check
   so the phone setup ends with visible Wi-Fi/DeepSeek feedback.
6. The AP password is shown on the screen but is never written to the log.

A blank password for an existing SSID keeps the stored password; a blank
password for a new SSID means an open network.

## Online word generation

When the user chooses `REFRESH WORDS` and a network plus API key are available:

1. The prompt pins the reply contract: one JSON object with a single `pack`
   string, no markdown or prose, exactly 6 `LESSON` lines and 60 complete
   `WORD` lines in `LSPACK` format, with 9 pipe-separated fields per word.
   It asks for a random, topic-varied sample (not alphabetical, not a repeat of
   the previous list) and gives compact per-field targets so the whole pack fits
   in one response. It also lists every word in the current pack (studied words
   first) plus a rolling history of words from earlier generated packs, and
   forbids reusing them, including plural and tense forms. Every request carries
   a fresh random seed and an English level from 1 (starter) to 10
   (professional). The level starts at the value chosen in `SETTINGS` and rises
   with the session count plus answered cards, so repeated refreshes vary the
   topic mix, starting letters and word frequency. After a malformed reply the
   retry mixes the attempt number into the seed and adds the strict warning, so
   the second sample is not just the first one again.
2. The request goes to `https://api.deepseek.com/chat/completions` with model
   `deepseek-chat` and `response_format` set to a JSON object.
3. The response is extracted and validated before it replaces the active pack.
   A short reply is not discarded: if the model stopped early or appended prose,
   `ls_gen_sanitize_pack()` drops the partial tail and model-supplied `END`,
   keeps the complete `LESSON`/`WORD` lines, rewrites the header counts to match
   and appends a fresh CRC. It also repairs each field instead of letting one
   deviation reject the whole reply: fields are trimmed to their format limits,
   common Unicode punctuation is converted to ASCII, and a `pron` value that is
   not valid ARPAbet is first run through `ls_ipa_to_arpabet()`: IELTS replies
   often supply IPA (`/ˈtenənt/`, `/ˈpredʒədɪs/`) despite the prompt, and that
   conversion keeps those words pronounceable instead of clearing the field.
   Only a transcription the converter cannot map is cleared, so the word is
   still kept without a spoken pronunciation. A word with an unusable
   required field (empty word/meaning/Chinese translation, or a line with
   missing fields) is dropped. A pack with at least 30 usable words is accepted,
   which leaves room for two sessions (20 + 10 distinct words) and keeps an
   IELTS refresh from failing on a slightly short or non-conforming reply.
   Fewer than 30 usable words is still an error.
4. Progress is carried across the refresh by matching word text; records whose
   words disappear are dropped.
5. The accepted pack's words are appended to a persistent avoid history in NVS
   (`avoid_hist`, 1600 bytes). It is stored oldest-first, deduplicated
   case-insensitively, and trimmed from the front when a new pack no longer
   fits, so replacing the active pack does not make its vocabulary eligible for
   reuse again. On the first boot after this list was introduced it is seeded
   with the active pack. RESET clears the history together with progress; a
   device without storage still refreshes, but only the active pack is
   excluded.

To keep the SRAM peak low on the ESP32-C3, the HTTP worker consumes the reply
as server-sent events (`stream: true`): each `data:` line is decoded and its
`delta.content` fragment is appended directly into one pack-sized buffer
(`LS_GEN_PACK_TEXT_MAX + 512`, 16 KiB plus headroom for the `{"pack":"..."}`
wrapper) as TLS records arrive. The raw JSON body is never resident. The
request body is never materialized either: its Content-Length is computed with
the same escaping rules and the bytes are streamed to the socket from a
256-byte stack buffer, so only the prompt lives in RAM during the handshake.
Streaming keeps bytes flowing during a long completion, so the 120 s per-read
timeout only has to cover the gap between deltas instead of the whole
generation. DeepSeek queues requests while busy, so the first delta can take
well over a minute; the longer timeout keeps that wait from being reported as a
failure. `sdkconfig.defaults` enables mbedTLS dynamic record buffers (so the
RX/TX record buffers are freed after each record instead of being held for the
whole session), keeps the RX record at 8 KB because a larger certificate flight
can arrive in one record, and trims the Wi-Fi static RX buffers: a static 8 KB
RX buffer held for the whole session fragmented the post-handshake heap enough
that the pack allocation failed even with tens of kilobytes reported free. A
60-word pack also keeps the streamed text and the parsed model below the SRAM
available on the ESP32-C3. The online worker parses into a staging
`ls_gen_pack_t` (60 words, about 15 KiB) after TLS is closed and the Wi-Fi stack
is shut down; the full 25 KiB `ls_pack_t` is only filled by the UI thread when
the result is promoted into `state->pack`. Earlier firmware allocated the full
25 KiB model next to the 16 KiB reply buffer, which fragmented the post-TLS heap
and produced the "out of memory for parsed pack (25428 bytes, free=101504)"
failure on IELTS refreshes. Wi-Fi power save is disabled
while the radio is up because modem-sleep latency spikes could look like read
timeouts mid-generation. A truncated generation (`finish_reason: "length"`) is
trimmed to its complete lines instead of being rejected. A reply that is still
malformed or below 30 usable words is retried once before the status screen
reports failure; the retry uses a stricter prompt and a re-mixed seed. Reused
vocabulary is **not** a failure: the pack is accepted, the status screen says
`SOME WORDS REPEATED`, and `DOWN: DETAIL` lists the repeated words. Keeping the
new pack is better than stalling at 100% completion while another generation is
requested, and the next refresh excludes those words again through the rolling
history. The success message uses the accepted word count, so a salvaged short
pack reads e.g. `45 new words ready`. Log lines report free heap, largest free
block, and `errno` when the HTTPS open, write, or read fails.

When a session ends and every word in the active pack has a progress record
(100% completion), the device automatically starts a refresh if a Wi-Fi network
and DeepSeek key are stored. Each completed session raises the learned count,
which in turn raises the effective English level for the next request, so the
pack is replaced by a harder one instead of being cycled indefinitely. A failed
automatic refresh shows its normal status screen; the learner can continue or
retry from the menu.

The prompt also states that uniqueness is enforced by the device: duplicate
words are dropped, and a match with the exclusion list is reported to the
learner instead of silently passing, so the model checks all 60 lines before
replying instead of relying on a post-hoc filter.

The API key is entered on the phone setup page and stored in NVS. For developer
reference, `.env.example` documents `DEEPSEEK_API_KEY`; the firmware itself does
not read that file.

## Connection check

`CONNECTION CHECK` (also run automatically after phone setup) reports each hop
separately instead of a single opaque error:

1. `Wi-Fi: OK  <ssid>` after the station joins, or `Wi-Fi: FAIL (<err>)`.
2. `Clock: OK` after SNTP, because TLS certificate validation needs a plausible
   clock.
3. `DeepSeek: OK` when `GET https://api.deepseek.com/models` returns HTTP 200
   with the stored key. The probe does not consume generation quota; other
   statuses are shown as `DeepSeek: HTTP <code>` or `DeepSeek: FAIL (<err>)`.

A failed refresh now maps `ESP_ERR_HTTP_CONNECT` to `Cannot reach DeepSeek
(Wi-Fi/TLS)` and the serial log carries the `errno` and heap watermark.

## Progress sync

Sync is disabled unless all three are present: a Wi-Fi network, a container SAS
URL saved on the Phone Setup page, and a profile created with `CREATE PROFILE`.
It is started from the `PROGRESS` screen; otherwise the row reads
`SYNC NOW (SETUP)` and the status page names what is missing.

### Azure layout

The configured URL is a container SAS URL such as
`https://ACCOUNT.blob.core.windows.net/CONTAINER?sv=...&sig=...`. Blob Storage
has no real folders: the device writes a blob whose name looks like a path, so
the portal shows a `profiles/<profile id>/` folder:

```text
CONTAINER/
  profiles/
    12345678-abcd-4f01-9ead-beef0badf00d/
      progress.lsprog
```

`progress.lsprog` is the same `LSPROG` text document used locally
(`text/plain`). The container must already exist; the SAS needs read, create,
and write permission (`sp` includes `r`, `c`, and `w`) and should be HTTPS-only
(`spr=https`). Generate it in the portal under **Container → Shared access
token**, or with:

```sh
az storage container create --account-name ACCOUNT --name CONTAINER
SAS=$(az storage container generate-sas --account-name ACCOUNT \
  --name CONTAINER --permissions rcw --https-only \
  --expiry 2027-01-01 -o tsv)
echo "https://ACCOUNT.blob.core.windows.net/CONTAINER?$SAS"
```

Paste the echoed URL into Phone Setup. Re-generating the token yields a full
URL that replaces the saved one.

The URL is a bearer credential. It is stored in NVS like the API key, never
printed by the firmware, and never included in a status screen or log line;
failures report `HTTP 403` (expired or wrong SAS), `HTTP 404` (container
missing), or the transport error instead.

### Merge order

A sync is read-merge-write, not a server-side merge:

1. `GET profiles/<id>/progress.lsprog` with `x-ms-version` and `x-ms-date`
   (SNTP runs first because Azure rejects stale dates and TLS needs a clock).
2. A `404` means the first sync for this profile; the merged document is the
   local snapshot. Any other non-200 status stops the sync with the status code.
3. Merge the remote document into a copy of the local snapshot. The local side
   stays authoritative: a word record wins when its `revision` is higher, ties
   keep the local copy, new word IDs are appended while the table has room, and
   `session_count`/counters follow the side with more completed sessions.
4. `PUT` the merged document back as a `BlockBlob` with
   `x-ms-blob-type: BlockBlob`. Azure returns `201` when the blob is created and
   `200` when it is overwritten.
5. The UI merges the returned snapshot into live state (idempotent) and saves it.

One 20 KiB document buffer is reused for the GET response and the PUT body so
the sync does not need two large allocations while TLS is active. A single
device is assumed per profile; there is no ETag/`If-Match` loop. If two devices
sync the same profile at the same moment, the later write wins but the next sync
merges the other device's records by revision.

## Power management

- The screen dims after 30 seconds without input.
- The device enters deep sleep after 300 seconds without input.
- Deep sleep refuses to arm while a key is held, so a stuck button cannot cause
  an immediate wake loop.
- On wake, the key guard waits for release before accepting new presses.
- Sleep order follows the BSP contract: stop audio and services, prepare the
  button hand-off, enable GPIO wake, then sleep battery, audio, I2C, and the
  display.

## Validation

```sh
./tools/validate.sh --static    # repository checks, host tests, Python tests
./tools/validate.sh --firmware  # ESP-IDF 5.5.3 build, merged image, verifier
```

The static checks include `tests/test_langstudy_logic.c` (pack/progress,
scheduling, phonemes, synthesis, JSON, generation extraction, setup forms),
`tests/test_langstudy_buddy.c`, and `tests/test_langstudy_azure.c` (SAS URL
validation, blob path building, and the `x-ms-date` spelling).

`tools/langstudy_demo_pack.txt` is a finalized copy of the built-in pack. It is
useful for inspecting the wire format or for feeding a known-good pack to
tooling without a device.

## Device acceptance checklist

1. Boot with no network: the built-in pack loads and the first of 20 cards
   appears.
2. Answer several cards, power-cycle, and confirm the same boxes and counters
   return.
3. Hold OK: the menu opens; UP/DOWN move the highlight and OK selects.
4. Press UP: the current word is audible; audio stops cleanly on sleep.
5. Choose `PHONE SETUP`: the AP appears, the page loads, and a valid network is
   stored only after the device joins it; the connection check follows
   automatically. A failed test names the cause (`Wi-Fi not found`, `Wrong Wi-Fi
   password`, or the driver error) and stores nothing.
6. Choose `CONNECTION CHECK`: the screen shows Wi-Fi, clock, and DeepSeek status
   separately, and `DeepSeek: OK` appears with a valid API key. After a
   successful check the Wi-Fi and `D` icons appear at the right of the study
   screen header; the same line shows `PASSPORT ENGLISH` and the vertical
   battery bar (an empty outline when the gauge has never reported a value).
   Both icons and the last battery fill survive deep sleep; they are redrawn
   on wake before any online operation runs.
7. On a study card the order is word, IPA, then Chinese translation: the
   translation wraps to two lines without overlapping the part of speech.
8. The pronunciation line renders IPA (for example `/həˈloʊ/`), not raw
   ARPAbet; a long transcription scrolls horizontally. `climate` shows
   `/ˈklaimət/` and `recycle` shows `/riːˈsaikəl/`, with a visible `i` inside
   the diphthong rather than an empty bar. `desert` shows `/ˈdɛzərt/` and
   `glacier` shows `/ˈgleiʃər/`; the `ər` ending is visible.
9. With Wi-Fi connected, tap a word: the neural voice plays the word text and
   the serial log shows `ls_edge_tts` heap and playback lines. Disable Wi-Fi (or
   move out of range) and tap again: the same word is spoken by the offline
   synthesizer instead. Tapping several words quickly stops the previous word
   instead of queuing them.
10. Choose `REFRESH WORDS`: a second list appears for `CET4`, `CET6`, and
   `IELTS`. If no Wi-Fi network or DeepSeek key is stored, the device instead
   shows `WI-FI OR DEEPSEEK NOT SET UP` immediately and does not start a
   network request. Selecting a tier requests a random new set of 60 words in
   6 lessons that shares no word with the current pack or the recent-generation
   history (studied words are excluded first), using a fresh seed and the
   English level from `SETTINGS`, raised by progress. A short reply with at
   least 30 complete words is trimmed and accepted; a malformed reply is retried once
   with a stricter prompt. If the accepted reply reused words from the avoid
   list it is still applied: the status reads `SOME WORDS REPEATED` and
   `DOWN: DETAIL` lists them. The success message reports the accepted count
   (normally `60 new words ready`). A failure status offers `DOWN: DETAIL`
   with the worker's diagnostic text.
   Once every word in the active pack has been studied (the `COMPLETION` row
   in `PROGRESS` reaches `100%`) and a session ends, a refresh starts
   automatically when Wi-Fi and a DeepSeek key are stored; the status screen
   reads `ALL WORDS STUDIED / REFRESHING <tier>`. The next pack uses the higher
   effective level implied by the extra completed session.
11. Choose `SETTINGS`: UP/DOWN moves between `ENGLISH LEVEL`, brightness,
    volume, and `WORD DELAY`; OK enters adjustment mode, UP/DOWN changes the
    value, and HOLD OK leaves or saves. All four values survive a power cycle.
    `ENGLISH LEVEL` runs from 1 (starter) to 10 (professional), defaults to 4,
    and is the starting point for word retrieval; progress raises the effective
    level above it. `WORD DELAY` is the auto-play dwell in seconds (default 5).
12. On a study card, HOLD DOWN to start auto-play for the current lesson: the
    progress line reads `AUTO n / m` and the thin bar drains over the dwell
    time before the card advances. HOLD DOWN again (or press any other key)
    stops it; HOLD OK stops it and opens the menu. The loop must not leave the
    current lesson and must not change the session score.
13. Choose `PROGRESS`: the screen shows sessions, answered cards, accuracy,
    studied words, and completion. With Wi-Fi, a SAS URL, and a profile
    configured, `SYNC NOW` creates
    `profiles/<profile id>/progress.lsprog` in the container and reports
    `Progress synced`; a second sync overwrites the same blob. Without them the
    row reads `SYNC NOW (SETUP)`.
14. Complete two sessions in a row and confirm the second set of cards does not
    repeat the first when the pack has enough words.
15. Leave the device idle: the screen dims at 30 seconds and the device sleeps
    at 5 minutes; a button press wakes it.
16. Choose `RESET PROGRESS`: all learning records, the recent-generation avoid
    history, the profile, and its buddy are cleared and a fresh session starts;
    the active word pack is kept.
17. Choose `CREATE PROFILE`: UP/DOWN cycles the preset names, OK shows the
    BUDDY screen with a sprite, form, XP, energy, and snacks. Answer cards and
    confirm the buddy gains XP (more at a higher English level, and 25% more
    while energy is at least 60). Feed with OK when a snack is held; HOLD OK
    returns to the menu. Power-cycle and confirm the same profile id, name,
    species, and XP return. A session with at least 70% accuracy adds a snack.

## Known limitations

- DeepSeek output is probabilistic; the pack validator is the only guarantee of
  content quality, not of pedagogy.
- Progress sync relies on a container SAS. The token is the only credential:
  scope it to one container, keep it HTTPS-only, set a finite expiry, and rotate
  it (re-save on Phone Setup) before it expires. The firmware keeps only the URL
  in NVS, so a factory reset or `RESET PROGRESS` does not revoke the SAS.
- The buddy record is device-local; only `progress.lsprog` is uploaded. A second
  device with the same profile id shares learning history but starts with its
  own buddy.
- The formant synthesizer is clearer than earlier versions but still synthetic;
  it is only the offline fallback, not a replacement for the neural voice.
- Online pronunciation depends on an undocumented Microsoft endpoint. The
  `Sec-MS-GEC` token, client version and user agent are tied together; a server
  change silently drops the device back to the offline voice until the
  constants in `langstudy_tts_edge.c` are updated.
- Online pronunciation keeps Wi-Fi enabled for 60 seconds after the last word
  to avoid a reconnect on every tap. Studying with the neural voice therefore
  costs noticeably more power than the offline-only path.
- Real-device timing for TLS, audio start-up, and deep-sleep wake must be
  measured on hardware; host tests cannot cover it.
