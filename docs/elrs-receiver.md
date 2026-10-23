# ESP32 ELRS DK500 receiver (`esp32-elrs`)

## Purpose

This firmware profile decodes ExpressLRS DK500 packets (OTA3, 2.4 GHz) on the ESP32 itself and streams them as text lines over UART to the web page in `web/elrs-rx/`. It is a viewer only: it is not flight-safe, it must not be used to control anything, and it never transmits. There is no TX path, no telemetry reply and no loopback IQ calibration; PHY initialisation at boot is the stock one.

The UID is a secret derived from the binding phrase. It is never echoed, logged or sent; `UID <hex>` is silent apart from `OK`, SYNC `P` lines carry only the model-match byte, and the page shows "UID set" only.

## Flashing

Build with the catalog profile and flash the three images with `esptool`:

```sh
. ~/esp/esp-idf/export.sh
python3 tools/build_firmware.py --profile esp32-elrs --version $(git rev-parse HEAD) \
    --build-root <scratch>/build --output <scratch>/art-N --jobs 8
cd <scratch>/art-N/esp32-elrs
python -m esptool --chip esp32 --port <port> write-flash @flash_args
```

The profile builds only for the ESP32 with `ESP_SDR_STREAMING=OFF`. `INFO` answers `ESP32ELRS 1 elrs-rx 0`; the stock `esp32` profile is unchanged and answers `ESP32SDR 6 burst 16380`. Serial is UART0 at 2 Mbaud 8N1. The boot ROM prints at 115200 baud, which shows up at 2 Mbaud as a run of NUL bytes before the first reply; clients should ignore them.

## Serial protocol

Lines are ASCII, `\n`-terminated. Every command gets exactly one reply line. `VERSION?`, `TRANSPORT?`, `BAUD`, `RELEASE` and the lease rules behave as in `docs/protocol.md`. While `RX ON`, unsolicited lines starting with `P`, `S`, `L`, `W` or `F` are interleaved with replies; clients route by first token. The one reply that starts with a stream token is `STATS?`.

| Command | Reply / notes |
| --- | --- |
| `INFO` | `ESP32ELRS 1 elrs-rx 0` |
| `CAPS` | `CAPS VERSION ELRSRX DK500 OTA3 UIDNVS BENCH SNAP16 SPECTRUM`. `FHSS TIMED` are added with timed mode, `RETUNE` with the retune benchmark, `HOP` only once hop mode is qualified. |
| `UID <12 hex>` | `OK`, or `ERR uid` / `ERR args`. RAM only; resets stats. Never echoed. |
| `UID?` | `UID NONE`, `UID RAM` or `UID NVS`. No fingerprint. |
| `UID SAVE` | Writes NVS namespace `elrsrx`, key `uid` (6-byte blob); `OK`, or `ERR no_uid` with nothing set. `UID?` then reports `NVS`. |
| `UID CLEAR` | Stops RX, wipes RAM and NVS; `OK`. |
| `MODE AUTO\|SCAN` | `OK`. Only SCAN exists, so the active mode is always `SCAN`. |
| `MODE TIMED` | `ERR timed_unavailable` |
| `MODE HOP` | `ERR hop_unqualified` |
| `MODE?` | `MODE <requested> <active>` |
| `PARK <2402..2478>`, `PARK?` | Parked LO in MHz. While RX is on, it retunes between captures. |
| `GAIN HARDWARE\|MANUAL <i>`, `GAIN?` | As stock. |
| `POLARITY INV\|AUTO` | `OK`. Default `INV`. |
| `RX ON` | `OK`; `ERR no_uid` without a UID. Repeating it is `OK`. |
| `RX OFF` | `OK`, sent after the last streamed line. |
| `STATS?` | One `S` line. |
| `BENCH <n>` | `BENCH n= cap_us= arm_us= read_us= det_us= demod_us= total_us= hits=` as p50/p99. Uses the configured UID, or the public test UID `00000000133e` when none is set. `ERR rx_on` while RX is on. |
| `SNAP16 <samples>` | `DATA <n> <crc32> <us>` then 2n bytes of raw samples. `ERR rx_on` while RX is on. |
| `SPECTRUM ON\|OFF` | `OK`. RAM only, default `OFF` at boot, allowed in any RX state; `F` lines are sent only while `RX ON` and `SPECTRUM ON`. |
| `SYNC <n>` | Echoes `SYNC <n>`; used by hosts to find the end of the stream. |

Boot state is `RX OFF` with the UID loaded from NVS if one was saved.

### Streamed lines

- `P <t_us> <ch> <R|S|M> - <cfo_khz> <snr_db> <fec> <data>`. `t_us` is the capture trigger time plus the end of the last sync bit; the nonce is always `-` in scan mode. For `R` and `M`, `data` is the 8-byte packet as 16 hex characters; for `S`, `fhss,nonce,rate,tlm,swmode,mm`.
- `S up= mode= state= snaps= bursts= ok= rc= sync= crcfail= mirror= dec_us= cap_us= ret_us= lock_err_us= cfo_khz= gain= drop=` once per second. `state=ACQ`, `mode=SCAN`. `cfo_khz` is an EMA (alpha 0.2) of accepted packets, rejecting jumps over 50 kHz. `drop` counts lines lost because the output ring was full.
- `W <text>`: `wide_mode` (Wide packets or SYNC with `swmode` 0, no `P` is emitted; at most once per 5 s), `rate <n>` (SYNC with a rate index other than 14, at most once per 5 s, the `P` is still emitted), `clip` (more than 50 clipped samples in a capture, once per s), `stale` (recalibration was stale for the capture, once per 5 s), `capture_timeout` / `capture_memory` (capture failed; once per 5 s).
- `F <t_us> <lo_mhz> <128 hex>` (with `SPECTRUM ON`): the detector's 64-bin peak-hold spectrum, 141 bytes for a 4-digit `t_us`. Hex byte j (lowercase) is the bin centred at LO + (j - 32) * 250 kHz, so byte 0 is LO - 8 MHz and byte 32 is LO. Value is `clamp(round(20*log10(max(|X|^2, 1))), 0, 255)` (0.5 dB steps). Each line is the per-bin maximum over all frames of all snapshots in a window that closes once a snapshot's capture trigger time is at least 100000 us after the window's first (trigger time, not wall clock), so at most 10 lines/s (measured about 8.4, about 1.2 kB/s); `t_us` is the capture trigger time of the first snapshot in the window and consecutive lines are at least 100 ms apart. If the LO changes while a window accumulates, it restarts at the new LO (no mixed-LO rows). `SPECTRUM ON|OFF`, `RX ON` and `RX OFF` clear the window. A dropped `F` line counts in `drop`.
- A SYNC whose UID bytes do not match the configured UID counts as `crcfail` and produces no `P`.

### Task layout

The command task (`app_main`, priority 5) parses lines. The radio task (priority 4) loops arm, trigger, finish, convert, decode, emit, and sleeps one tick per loop. The TX task (priority 3) drains an 8 KB no-split ring through `burst_serial_send`. Streamed lines never block and are dropped (and counted) if the ring is full; replies block until queued and so follow every streamed line queued before them. Raw binary output (`VERSION?`, `SNAP16`) waits for the ring to drain and holds the TX lock.

## Notes

- `ELRS_INBAND_KHZ` is 7500, wider than the 6 MHz in the design: the real fixture has a packet at +7.4 MHz from the LO. Bursts outside it are ignored.
- Overlap drop: when two detected bursts overlap in time by at least half of the shorter one and differ by more than 10 dB, the weaker one is discarded as a mirror or ghost before decoding. If a real packet is that much weaker than a simultaneous neighbour, it is lost. `bursts` in the `S` line counts detections before the drop.
- A capture is 16380 samples at 16 MS/s (1.02 ms); decoding takes about 10 ms, so roughly 10% of the air time is observed in scan mode. That is why the rate is compared against the older Python receiver's 3.3 snapshots/s rather than against the 1 kHz packet rate.

## Design decisions recorded from earlier tasks

- **Detector noise floor.** The spec said median power and an 8 dB threshold. The detector uses the 25th percentile of the per-channel frame power, with +-2 frame smoothing, and a 3 dB threshold (Tasks 5-7). Against the JS reference decoder on the real capture, the C decoder matched 86% of its packets at 5 dB and 95% at 3 dB, so the threshold was lowered. `snr_db` is measured on the smoothed power.
- **Regression fixture.** `tests/fixtures/elrs/caps_rc.bin` (60 snapshots of 16380 int8 I/Q pairs at 2436 MHz, UID `00000000133e`) and `caps_rc.json` come from the 300-snapshot real capture `caps.npz`, written by `tools/elrs/export_fixture.py` (`--snapshots` selects and orders them) and decoded by the JS reference through `tools/elrs/golden_from_js.mjs`, which writes the golden packets. Three groups: snapshots with RC packets and no SYNC (the golden packets), eight snapshots where both JS and C find zero bursts (`zeroBursts`), and eight where both find bursts but decode nothing (`noDecode`). Regenerate with `export_fixture.py <npz> <out.bin> --snapshots ...` followed by `node golden_from_js.mjs <bin> 16380 2436`, then update the JSON. The `.bin`/`.json` files need `git add -f` because of the global ignore.

## Measurement log

Task 15, 17 and 19 append here.

| Date | What | Result |
| --- | --- | --- |
| Task 9 | `BENCH 500` at `PARK 2438`, µs p50/p99 | `n=500 cap_us=1224/1263 arm_us=179/211 read_us=2544/2736 det_us=4606/5133 demod_us=2688/8089 total_us=11025/16698 hits=143`. total p99 is below 15 ms, so K1 = continue. |
| Task 10 | `BENCH 500` with the output ring and radio task present (RX off) | `n=500 cap_us=1203/1222 arm_us=163/175 read_us=2536/2725 det_us=4537/5148 demod_us=2557/9826 total_us=10634/18327 hits=113` (hits vary with the air traffic and with the test UID fallback) |
| Task 10 | SCAN rate, 601 s, real UID, DK500 at ~1 m, sticks centred, `PARK 2438`, hardware AGC | RC updates/s 10.10 avg (6067 RC); ok 12559 (20.9/s); sync 302 (all passed the UID check, 0.5/s); crcfail 288 (2.3% of ok); drop 0; 51551 snapshots = 85.8/s vs 3.3/s for the old Python receiver (RC in 11.8% of snapshots vs its 3-16% hit rate); `W clip` 579 times (about once per s, the rate limit). crcfail cannot be split between decoder CRC failures and SYNC UID failures from the `S` line; the test-UID run (every SYNC fails the UID check) had about 1 crcfail/s against 0.48/s here, so SYNC UID failures with the real UID are about zero. |
| Task 10 | Gain comparison, ~19 s each, same setup (rates from `S` counter deltas) | HARDWARE: rc 10.05/s, ok 22.9/s, crcfail 7, `W clip` 19 (gain field ~76). MANUAL 20: rc 10.84/s, ok 22.2/s, crcfail 0, no clip. MANUAL 40: rc 15.47/s, ok 31.1/s, crcfail 19, no clip. MANUAL 60: rc 2.05/s, ok 3.8/s, clip 20. Single short runs; defaults unchanged. |
