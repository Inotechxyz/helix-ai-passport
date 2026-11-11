<p align="right">
  <a href="langstudy-todo.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# TODO — langstudy buddy branch

Working notes for the `feature/langstudy-buddy` branch. Two commits are local to
the branch (`a1f019e`, `87f5141`); the current working-tree changes are
uncommitted, and the branch has not been pushed.

Status: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

---

## P0 — Edge TTS sometimes returns a short word ("prosperity is not completely played")

**Symptom.** On the device, some words are spoken only for the first ~0.6 s. The
same word on a later press is often played whole. Reported on `prosperity`,
`conservation`, `deforestation`, `inflation`, `budget`, `capital`, `vaccine`.

**Evidence so far (serial logs `build/wsl-flash/audio-debug7.log`).**

| Attempt | Peak download | Decoded | Complete |
| --- | --- | --- | --- |
| prosperity #1 | 3600 B | 14400 samples | 1 |
| prosperity #2 | 3600 B | 14400 samples | 1 |
| prosperity #3 | 8064 B | 32256 samples | 1 |

The server sends `turn.end` (`complete=1`) even for the short responses, so the
device is not cancelling and TLS is not dropping records.

**Measured healthy response** (host probe, identical wire bytes):

```
payloads [850 ×11, 274, 105]
headers  [128 ×12, 103]
audio    [720 ×11, 144, 0] = 8064 B
```

So every audio frame is `720 B` of MP3 (`2 B length + 128 B Edge header + 720 B`).
A 3600-byte device download equals exactly **5 × 720**, i.e. one frame of the
six logged frames contributed no audio — the accounting does not match a shorter
sentence, it matches a dropped frame.

**Ruled out.**
- WebSocket fragmentation: every frame logged a final frame with the first-frame
  flag set, no continuations.
- Request shape: a host client sending our exact URL params, headers, config JSON
  (no trailing CRLF) and SSML gets 8064 B on every one of 12 tries.
- Backend IPs: `150.171.27.10` and `150.171.28.10` each returned 8064 B ×10.
- Clean server flakiness: host `edge-tts` 7.3.1 returns a constant size per word
  (prosperity 8064, conservation 8352, inflation 7776), 8/8 each.
- Heap: store-and-forward + early 16 KiB scratch reservation removed all
  allocation failures (the MP3 decode-buffer allocation failure no longer
  appears).

**Not yet tested.** Whether the 6th frame is a non-audio binary frame that the
device discards (e.g. `header_len > EDGE_HEADER_MAX (256)` pushes `Path:audio`
out of the scanned prefix), or whether the server genuinely ends the turn early
on a slow client.

**Current state.** A diagnostic build is flashed (audio-debug8 build) that logs,
per binary frame: header length, bytes read, is-audio and payload remaining,
plus append/total counters, every text frame, the resolved server IP, and the
timestamp/SSML length we send. Capture file: `build/wsl-flash/audio-debug8.log`.
**Not yet tested on device** — the tap sequence was never run, so the log is
empty.

**Resume by.** Start `build/wsl-flash/watch.py > build/wsl-flash/audio-debug8.log`,
press UP ~4× on each of prosperity / conservation / deforestation / inflation,
then read the log:
- If the server sends 6 frames and the last one has no audio with a long header,
  raise `EDGE_HEADER_MAX` (or scan the header tail) — parser bug, real fix.
- If the server sends 6 audio frames and `turn.end`, it is server-side. Add a
  client-side guard: a single English word of ≥6 characters cannot be 0.6 s, so
  re-request once when `phrase.len` is implausibly short (e.g. `< 4800 B`), and
  only then fall back to the local synthesizer.
- Consider reusing one WebSocket for consecutive words: it removes the 1.5–2.8 s
  handshake+upgrade that makes learners press UP again, and would also tell us
  whether truncation correlates with fresh connections.

**Housekeeping.** Before any commit, remove the temporary per-frame header,
append, text-frame, server-IP and timestamp diagnostics; keep the heap and
voice-data/complete summary lines.

---

## P1 — Other work still unverified on device

- [ ] **Navigation must not replay the previous word.** `ls_voice_stop()` now
      sets `s_cancel`, bumps `s_request_seq`, resets the queue and stops audio at
      every navigation/menu/provisioning site in `langstudy_ui.c`. Not yet
      confirmed on hardware (same tap test as P0).
- [ ] **Repeat presses.** Duplicate-tap absorption plus one replay-on-failure is
      in `langstudy_voice.c`; behaviour is mediocre until P0 is fixed.
- [ ] **Azure sync retry.** `langstudy_sync.c` now opens/handshakes before
      allocating and streams GET into a growing buffer. Run
      `PROGRESS → SYNC NOW` and confirm `profiles/<uuid>/progress.lsprog`
      appears in the blob container.
- [ ] **Buddy flow.** CREATE PROFILE, sprite, XP/EVOLVE, feed/condition,
      persistence across reboot — none verified on hardware yet.
- [~] **Sleep/wake status header.** The Wi-Fi/`D` icons are no longer persisted:
      every boot and wake starts a silent `LS_ONLINE_PROBE` that re-checks both
      hops, so they appear a few seconds after the UI and are blank until then.
      Only the battery SoC crosses deep sleep (RTC cache). Still needs a
      device check of reboot → icons, key wake → icons, and the probe staying
      out of the first word's Edge TTS path.
- [ ] **Launch screen.** Five-second splash on cold boot/reboot (not key wake),
      generated art via `tools/gen_splash.py`; check the dedication text, the
      handover with no black frame, and that input during the splash is inert.

---

## Notes for whoever resumes

- Flash segmented, never full-chip erase (NVS holds Wi-Fi and profile data):
  ```
  esptool --chip esp32c3 -p COM11 -b 460800 write_flash --flash_mode dio \
    --flash_size 8MB --flash_freq 80m \
    0x0 build/wsl-flash/bootloader.bin \
    0x8000 build/wsl-flash/partition-table.bin \
    0x30000 build/wsl-flash/FoloToy-AI-Passport.bin
  ```
- `build/wsl-flash/watch.py` holds COM11 — stop it before flashing.
- WSL build: remount `/mnt/d`, rsync to `/home/jason/helix-build` (exclude
  `managed_components`, or delete it and let the component manager re-fetch),
  strip CR from `tools/*.sh`, then
  `su - jason -c /home/jason/run_build.sh` → `FIRMWARE_COMPILE_PASS`.
- Host tests: `ACTIONLINT_BIN=/tmp/actionlint-stub tools/validate.sh --static`
  (recreate the stub each WSL session; the actionlint download returns 504).
