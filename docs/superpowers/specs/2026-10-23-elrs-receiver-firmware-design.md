# ELRS DK500 stick-viewer firmware for ESP32 (`esp32-elrs`) and its web UI

Status: design, not yet implemented. Date: 2026-10-23. Repository: Akceptor/esp-sdr (fork of ESPARGOS/esp-sdr, GPL-3.0).

Tags used below:
- **[U]** the user decided this.
- **[C]** the orchestrator decided this.
- **[D]** a design call made in this document; the builder may change it with a reason.
- **[O]** open; the builder decides, usually after a measurement.

## 1. Intent

The user wants to watch their ExpressLRS transmitter's sticks and switches live in a browser. Nothing flies on this, and missed packets don't matter. Today the stock ESP-SDR firmware sends 16380-sample (1.02 ms at 16 MS/s) raw I/Q snapshots over 2 Mbaud UART, about 160 ms each, and the host decodes them. The LO stays on one frequency, so only about 15% of hops fall inside the window. Result: about 3.3 snapshots/s, a 3–16% hit rate and roughly one update every 20 s.

Two steps, designed together **[U]**:
- **Step 1:** decode on the chip with the LO parked, and send only decoded packets. Expect 5–20 updates/s (§6.1).
- **Step 2:** predict the ELRS FHSS schedule and time captures to packet arrival. Expect 50+ updates/s (§6.2).

Step 1 must ship on its own **[U]**.

## 2. Goals, non-goals, anti-goals

**Goals**
- A separate firmware variant `esp32-elrs` for the user's ESP32-WROOM-32U. It receives ELRS 3.x DK500 (`RATE_FSK_2G4_DVDA_500HZ`) and streams decoded packets plus statistics over UART **[U]**.
- A new page `web/elrs-rx/` with layout "C". The existing `web/elrs/` raw-I/Q viewer and the upstream viewer keep working with the stock firmware **[U]**.
- A flasher that offers both firmwares **[U]**.
- Each firmware identifies itself, so pages can redirect the user to the right one **[U]**.

**Non-goals** (possible later work)
- Wi-Fi or BLE transport **[U]**.
- Other ELRS rates, LoRa, FLRC/SX128x, 900 MHz, and ELRS 4.x / OTA4. The OTA version stays a parameter.
- Wide switch mode decoding (the user runs Hybrid **[U]**). Wide packets are detected and reported, not displayed.
- CE/LBT behaviour, binding mode, model-match enforcement, telemetry, chips other than the ESP32, upstreaming **[D]**.

**Anti-goals** (never do these)
- **Never transmit.** No TX paths, no telemetry replies, no loopback IQ calibration. The firmware keeps the stock boot-time PHY init unchanged and adds nothing to it. `rx_recalibrate` already skips TX-tone steps (`main/common/rx_recalibration.c`).
- **Never claim to be flight-safe.** No CRSF/PWM/SBUS output and no failsafe. The UI and docs say "viewer only".
- **Never break the stock firmware or the existing pages.** Don't edit `main/targets/esp32/receiver.c`. The stock profile must build from unchanged sources. The only change to existing pages is the additive check in §10.2.
- **Never let the binding phrase leave the browser.** Never log or echo the full UID.
- **Never commit or push the real phrase or UID** to `main` or any public remote **[C]**. Branch `elrs-spike` contains the real phrase in `tools/elrs/test_synth.py`. Its code is *ported*, never merged. Tests use only UID[4..5] (as `web/elrs/test/decode.test.js` does) or public phrases such as `test`.

## 3. Constraints and established facts

| Area | Fact |
| --- | --- |
| TX hardware | LR1121, ELRS 3.x (OTA_VERSION_ID 3), DK500. 2-FSK 300 kb/s, fdev 100 kHz (measured ±~135 kHz), no shaping. 16-bit preamble 0x55. Sync word UID[5],UID[4] in that order. Inverted on air (higher frequency = 0). 14-byte payload = Hamming(7,4) plus interleave of the 8-byte packet. LR11xx whitening seed 0x100 with bit-reversed keystream. Radio CRC off. CRC14 poly 0x2E57, init `((UID4<<8)\|UID5)^3`, no nonce. Air time ~480 µs. |
| TX configuration **[U]** | Switch mode **Hybrid** (Hybrid8). Telemetry ratio **default 1:128**. Regulatory domain **ISM 2.4** (not CE LBT). The bound receiver is **usually off**, so the TX stays *disconnected* (§4). |
| ESP32 | Snapshot only, no ring mode. At most 16380 samples into the 64 KiB bank at 0x3ffe8000. 80/40/16 MS/s; 16 MS/s gives 1.024 ms. The receiver stops while the CPU reads. CFO about +230 kHz (~95 ppm). IQ mirror ghost with opposite polarity. Analog bandwidth 12–67 MHz. Stock config: unicore, 240 MHz. |
| Retune | Stock `tune_rx()` runs `rx_recalibrate()` (DC plus gain-table rebuild) on every MHz change, then `set_chanfreq` and `rom_set_rf_freq_offset`. Latency **not measured** (§9.3). |
| Link, build | UART0 at 2 Mbaud (~200 kB/s). Web Serial needs desktop Chromium. ESP-IDF `25fe69f946311abdaf9ad56591f25fedbc20ac98`. Build entry points: `firmware-targets.json`, `tools/build_firmware.py`, `.github/workflows/firmware.yml` (matrix built from the catalog). |
| Reference code | Python on branch `elrs-spike`: `tools/elrs/analyze.py` (`analyze_snapshot_dk500`), `dk500.py`. JS: `web/elrs/demod.js` (`decodeSnapshot`, `PolarityFilter`), `dk500.js`. Real fixture: `web/elrs/test/real_dk500.bin`, 6 snapshots of int8 I/Q, 16 MS/s, 2436 MHz centre; it needs only UID[4..5]. |
| Workflow **[C]** | The builder (Claude) flashes the board with esptool and drives it over serial directly. The board is attached to the user's Mac at `/dev/cu.usbserial-585C0583231`; stop any process holding the port first. The user is needed only for TX actions: power cycle, sticks/switches, distance, and settings. |

## 4. ELRS 3.x facts used

Source: ExpressLRS tag **3.6.4** **[C]**. Every item was diffed against 3.5.3 and is identical. Each item cites its file and function.

**Rate**
- `src/src/common.cpp`, `ExpressLRS_AirRateConfig[14]` (LR1121): GFSK 2.4, hop interval **2**, interval **1000 µs**, 8-byte OTA packet, **numOfSends 2**, `TLM_RATIO_1_128`.
- `ExpressLRS_AirRateRFperf[14]`: TOA 690 µs (includes margin), **SyncPktIntervalDisconnected 3 ms**, SyncPktIntervalConnected 5000 ms.

**UID, seed and CRC**
- `common.cpp:uidMacSeedGet()`: `seed = UID2<<24 | UID3<<16 | UID4<<8 | (UID5^3)`.
- `OTA.cpp:OtaUpdateCrcInitFromUid()`: `crcInit = ((UID4<<8)|UID5)^3`.
- `OTA.cpp:GeneratePacketCrcStd`: CRC14 runs over bytes 0–6 with byte 0's `crcHigh` bits zeroed (Hybrid). In Wide mode, RCDATA uses `(nonce%2)+1` instead. The decoder therefore tries 0 first and 1–2 only to detect Wide and warn.

**FHSS**
- DK500 uses the 2.4 GHz "dual-band" table (`tx_main.cpp:SetRFLinkRate`: `FHSSusePrimaryFreqBand = false` for `RADIO_TYPE_LR1121_GFSK_2G4`).
- `FHSS.cpp:domainsDualBand`: 80 channels at **2400.4 + k MHz**. Sync channel **41 = 2441.4 MHz**; the spike README's "2440" is wrong.
- Sequence length 240 (`FHSS.h:FHSSgetSequenceCount`, `(256/80)*80`).
- `FHSS.cpp:FHSSrandomiseFHSSsequenceBuild`:
  1. `rngSeed(seed)`.
  2. Fill: `seq[i] = i%80==0 ? 41 : i%80==41 ? 0 : i%80`.
  3. Shuffle: for i with `i%80 != 0`, `swap(seq[i], seq[(i/80)*80 + rngN(79)+1])`.
- `random.cpp:rng()`: `seed = (214013*seed + 2531011) % 2^31`, with uint32 wrap-around in the multiply (JS ports need `Math.imul`). It returns `seed>>16`. `rngN(m) = rng()%m`.
- Each 80-entry block is a permutation with channel 41 at positions 0, 80 and 160.
- Python re-implementation check: phrase `test` gives UID `4f04fd822155`, seed `0xfd822156`, and the sequence starts `41 27 47 26 44 13 22 18 52 15 38 21`. Regenerate this from compiled ELRS code in tests.

**Timing**
- `tx_main.cpp:timerCallback` runs every 1000 µs and does `OtaNonce++`, then `SendRCdataToRF`.
- `TXdoneISR` hops (`FHSSgetNextFreq`, `FHSSptr++`) when `(nonce+1)%2 == 0`. So **nonces (2m, 2m+1) share a channel**, 1 ms apart: the DVDA pair.
- Pointer for nonce n: `ptr(n) = ptr(n0) + floor(n/2) − floor(n0/2)`, using unwrapped counters. The pointer runs mod 240 and the nonce is a uint8.
- About 1 slot in 128 is a telemetry slot with no TX packet.

**SYNC cadence with the receiver off** **[U]** (`tx_main.cpp:SendRCdataToRF`, `UpdateConnectedState`)
- Without telemetry the TX stays `disconnected`, so SyncInterval is 3 ms.
- A SYNC goes out only while `FHSSonSyncChannel()`, and only when `syncSlot/2 <= nonce%2`. `syncSlot` cycles 0–3.
- So **exactly one SYNC per sync-channel visit**, i.e. every 80 hops = **160 ms (~6.25/s)**. It falls on the first packet of the pair on two of every four visits and on the second packet on the other two.
- A config or model change additionally spams 3 SYNCs on any channel (`syncSpamCounter`).
- With a bound, powered RX, the TX is connected and sends SYNC only every 5 s.

**Packet layout** (`OTA.h`, LSB-first bitfields)
- Byte 0: type in bits 0–1 (RC 0, MSP 1, SYNC 2, TLM 3) and crcHigh in bits 2–7. Byte 7: crcLow.
- RCDATA: bytes 1–5 hold 4×10-bit channels, OTA order roll, pitch, throttle, yaw (`PackUInt11ToChannels4x10`). Byte 6 holds `switches:7` and `ch4:1` (AUX1/arm).
- SYNC (`OTA_Sync_s`): byte 1 fhssIndex, byte 2 nonce, byte 3 = switchEncMode bit 0 (0 Wide, 1 Hybrid) / newTlmRatio bits 1–3 / rateIndex bits 4–7 (expect 14), bytes 4–6 UID3, UID4, `UID5 ^ (~modelId & 0x3f)`.
- The spike's Python/JS SYNC parsers use the 4.x layout. The new code uses the layout above.

**Hybrid8** (`OTA.cpp:GenerateChannelDataHybrid8`)
- `sw = tlm<<6 | idx<<3 | val`. Indices 0–5 carry AUX2–AUX7 as 3-bit values (`switch3bToCrsf`). Index 6 carries AUX8 as a 4-bit value (16 positions).
- The index advances on every packet, so all of AUX2–AUX8 refresh every 7 packets (≈7 ms). No nonce is needed.

RX behaviour worth mirroring: `rx_main.cpp:ProcessRfPacket_SYNC`, `HandleFHSS`, `updatePhaseLock`.

## 5. Firmware architecture

### 5.1 Layout **[D]**

All new code goes in new files; the stock backend stays untouched, which also avoids upstream merge conflicts.

| Path | Contents |
| --- | --- |
| `main/elrs/` (C11, no IDF, builds on the host) | `ota3.c/.h` (whitening, FEC with tail erasures, CRC14, 3.x parse, Hybrid8 map). `fhss.c/.h` (rng, sequence, prediction). `dsp.c/.h` (detector, channelizer, discriminator, slicer, sync search). `lock.c/.h` (acquisition and tracking). `frames.c/.h` (line formatting). |
| `main/targets/esp32/elrs/app.c` | `app_main`, command parser, scheduler, NVS, stats. |
| `main/targets/esp32/elrs/radio.c` | **Copies** of `prepare_rx`, `apply_gain`, `tune_rx` and filter apply/restore from `receiver.c` (cite the source commit), `acquire_iq` split into arm/trigger/finish, plus a new `tune_fast()`. |
| Reused unchanged | `common/burst_serial.c`, `burst_version.c`, `rx_recalibration.c`, `rx_calibration_state.c`, `rx_lo.h`, `rx_bandwidth.h`, `targets/esp32/sram_guard.ld`, esp-dsp. |

### 5.2 Tasks and cores **[D]**

- **Baseline is single-core, as in stock.** A high-priority radio task loops capture → decode → emit. A serial RX task parses commands. The UART TX path drains a ring buffer.
- **M5 [O]:** build with `CONFIG_FREERTOS_UNICORE=n`, run the DSP on core 1, and hand buffers off between cores. Traps: on dual-core ESP32 `DPORT_REG_READ` stalls the other CPU, and core 1 must not touch 0x3ffe8000–0x3fff7fff while the dump engine owns it.

### 5.3 Capture primitive **[D]**

`acquire_iq` is split into three parts:
1. **`arm()`:** sentinel fill, filter, bank owner and lanes. This is the slow part (~100–300 µs **[O]**) and runs before the target time.
2. **`trigger(t)`:** busy-wait on CCOUNT, pulse bit 19, and record `t_trig` as the time of sample 0.
3. **`finish()`:** wait for done, restore, then check the sentinel. The check may be reduced to the `DUMP_STATUS` count plus the first unwritten word **[O]**; `BENCH` keeps the full check for comparison.

Packet arrival time = `t_trig + i_syncEnd/fs`. If `rx_recalibration_stale()` trips, re-tune, emit `W stale`, and go to COAST in timed modes.

### 5.4 Decoder pipeline **[D]**, parameters **[O]**

The pipeline ports the proven JS/Python algorithm and replaces its costly front end (13 full-length 132-tap channelizers, about 14 M MAC).

1. **Convert.** Turn the 10-bit I/Q into int16 and subtract the mean. Step 1 reads the bank directly.
2. **Sparse spectrogram detector.** Use `dsps_fft2r_sc16`, already used by `main/common/spectrum.c`.
   - 64-point FFT every 16 µs gives 64 frames with 250 kHz bins.
   - Sum 4 bins per in-band channel (|f−LO| ≤ 6 MHz).
   - A burst is ≥20 frames (≥320 µs) at ≥8 dB above that channel's median, and must be the local maximum against neighbouring channels.
   - Coarse CFO = power-weighted bin centroid.
3. **Per burst only.** NCO-mix at `f_ch − LO + CFO` over the burst plus a 260-sample margin (~9000 samples). Decimate by 8 to **2 MS/s** (6.67 samples/bit) with boxcar-4 followed by a ≤15-tap low-pass at 400 kHz.
4. **Discriminator.** Conjugate product `I[n-1]Q[n] − Q[n-1]I[n]`. Refine the CFO with `twoMeansCenter` and subtract it.
5. **Timing.** Integrate-and-dump with an 8-phase search for the largest mean |soft| (`recoverTiming`).
6. **Sync and payload.** Look for the 16-bit UID5,UID4 pattern, **inverted polarity only**, with ≤1 bit error. Then take 112 bits, de-whiten (0x100, bit-reversed) and FEC-decode, using erasure decoding when ≤30 tail bits are missing. Reject if there are more than 5 corrections. Then check CRC14 (§4).

**Budget** (estimates for LX6 at 240 MHz; confirm with `BENCH`):

| Stage | Estimate |
| --- | --- |
| Convert | ~0.3 ms |
| Detector | ~0.8 ms |
| Each burst | ~0.6–1 ms |
| Snapshot with 1 burst | ≈2 ms DSP |
| Capture plus arm/finish | ≈1.3–1.5 ms |
| Result | ~250 snapshots/s best case; plan for 100+ |

When locked (§7), steps 2–3 shrink to a known channel, time and CFO, about 1 ms per capture. The FPU is fine for per-burst float math; the front end stays int16.

**Fallback F1 (K1):** run only steps 1–3 on the chip and send the decimated burst (~2.2 KB) for decoding in JS at 2 MS/s.

### 5.5 CFO and mirror handling **[D]**

- **CFO:** keep a per-packet EMA (α = 0.2) and reject jumps above 50 kHz. Locked captures mix at the EMA value. `CFO/f_c` seeds the timing-drift estimate; this assumes each side's RF and timers share one crystal, which needs verifying **[O]**.
- **Mirror ghosts:** accept only the on-air polarity "inv"; a mirrored ghost decodes with opposite polarity and fails sync. `POLARITY AUTO` (majority vote, like `PolarityFilter`) stays as a debug option. In SCAN, of two time-overlapping bursts at about ±f, drop the one more than 10 dB weaker before demodulating. Timed modes demodulate only the predicted frequency.
- **Fixture check:** both of the fixture's 2 ghost packets must be rejected.

## 6. Operating modes

### 6.1 Step 1: SCAN (parked LO, free-running)

- Default `PARK 2438` **[D]** covers 2432.4–2443.4 MHz, including the sync channel at about +3.6 MHz after CFO. Grid channels land at about −0.37 and +0.63 MHz after CFO, off DC.
- The loop is arm → trigger now → finish → decode → emit.
- Hit rate per snapshot ≈ 0.15 × (544 µs slack + ~100 µs erasure tolerance)/1000 µs ≈ **9–10% at good SNR**. That gives **≈10–25 RC updates/s at good SNR and about 5 when weak**.

### 6.2 Step 2: TIMED, then HOP

- **2a TIMED [D] (deliver first).** The LO stays parked and the schedule (§7) predicts the in-band hops, about 12 of 80, i.e. ~75 per second. Each capture starts ~250 µs before the predicted packet, leaving ±250 µs of slack in the window. Expect **50–70 updates/s with no retuning**.
- **2b HOP.** Retune before each capture and target the **second packet of the DVDA pair**: retune after packet 1's predicted end, leaving about 1.5 ms of lead. Capture every k-th hop, with k = ceil((retune + arm + capture + finish [+ decode if single-core]) / 2 ms). At k = 2, expect about 225 updates/s.
  - LO placement: an integer-MHz LO at `ceil(f_ch) − 2` puts the signal at about +1.6 MHz **[O]**.
  - Fast retune **[O]**: calibrate once at 2440 MHz, then hop with `rom_set_rf_freq_offset` alone, after releasing the capacitor-code pin as `tune_rx` does. Trap: the PLL can stay unlocked if that pin isn't released, so qualify every channel.
- **AUTO** (default): SCAN → TIMED once locked → HOP once qualified (§9.3), falling back down the chain on loss.

## 7. Timing lock (step 2)

**States:**

| State | Meaning |
| --- | --- |
| `ACQ` | SCAN, collecting observations |
| `LOCK` | TIMED or HOP active |
| `COAST` | 40 scheduled captures in a row with no valid packet; keep predicting for ≤2 s |
| `LOST` | Back to `ACQ` |

Every transition emits an `L` line.

### 7.1 Acquisition

1. **Primary: SYNC packet [U-driven, D].**
   - Because the user's RX is normally off, a SYNC arrives every 160 ms on 2441.4 MHz, which is inside the default park window.
   - Validate it: `seq[fhssIndex] == 41`, `rateIndex == 14`, UID3/UID4 match, and switchEncMode is Hybrid (otherwise `W wide_mode`).
   - The SYNC fixes pointer, nonce and pair phase in one packet.
   - Step 1 catches about 10% of SYNCs, so **expected lock ≈1.5 s, and ≤5 s in 95% of cases**.
   - SCAN can bias toward SYNC: once one sync-channel packet is seen, the next visit is due exactly 160 ms later, so schedule a capture for it **[O]**.
2. **Secondary: RC triplet**, for when a bound RX is powered and SYNC comes only every 5 s.
   - Each decoded RC packet gives (channel c, arrival t), and c sits at exactly 3 sequence positions.
   - Hop delta: `Δh = round((t2 − t1 − δ)/2000 µs)` with δ ∈ {−1000, 0, +1000} µs.
   - Keep hypotheses (p1, δ) where `seq[(p1+Δh) mod 240] == c2`; a wrong hypothesis passes with probability 1/80.
   - Confirm with a 3rd and 4th observation (about 0.1% false locks). Keep gaps under 5 s.
   - The nonce stays unknown until a SYNC arrives. Hybrid display doesn't need it.

### 7.2 Tracking **[D]**, gains **[O]**

- Model: `t(N) = T0 + N·P`, where N counts packets since lock.
- Initialise `P = 1000 µs·(1 − CFO/f_c)` and refine it with an α-β filter (α 0.1, β 0.005) or least squares over the last 64 packets. Gate residuals beyond 150 µs.
- Predict packet N: pointer `p0 + floor((N+φ)/2) mod 240`, channel `seq[ptr]`, nonce `(n0+N) mod 256`.
- Every SYNC (one per 160 ms) cross-checks the prediction; on mismatch, relock at once (TX reboot or rate change).
- Three RC packets in a row at an unexpected channel or time lead to `LOST`.
- Expect drift of ±100 µs/s before correction and a few µs/s after.
- Scheduler: sleep by FreeRTOS tick until 1.2 ms before the target, then busy-wait on CCOUNT.

## 8. Serial protocol (`ESP32ELRS`, protocol 1) **[D]**

- ASCII lines at 2 Mbaud 8N1, terminated by `\n`.
- `SYNC <nonce>`, `VERSION?`, `TRANSPORT?`, `BAUD`, `RELEASE` and the lease rules behave as in `docs/protocol.md`.
- Each command gets one reply line.
- While `RX ON`, the firmware also streams unsolicited lines whose first token is `P`, `S`, `L` or `W`. Clients route lines by first token; replies always start with something else.

| Command | Reply / notes |
| --- | --- |
| `INFO` | `ESP32ELRS 1 elrs-rx 0`. Stock answers `ESP32SDR 6 burst 16380`. Pages check only the first token. |
| `CAPS` | `CAPS VERSION ELRSRX DK500 OTA3 FHSS TIMED HOP UIDNVS BENCH RETUNE SNAP16`. `HOP` is listed only once qualified. |
| `UID <12 hex>` | `OK` / `ERR uid`. Never echoed. Resets lock. |
| `UID?` | `UID NONE` / `UID RAM` / `UID NVS`. No fingerprint; the page re-sends the UID on every connect. |
| `UID SAVE`, `UID CLEAR` | NVS namespace `elrsrx`, key `uid` (6-byte blob). `CLEAR` wipes RAM and NVS. |
| `MODE AUTO\|SCAN\|TIMED\|HOP`, `MODE?` | `HOP` returns `ERR hop_unqualified` until qualified. |
| `PARK <2402..2478>`, `PARK?` | Parked LO in MHz. |
| `GAIN HARDWARE\|MANUAL <i>`, `GAIN?` | As stock. |
| `RX ON`, `RX OFF` | `OK`. For `RX OFF`, the `OK` comes after the last streamed line. Boot state is `RX OFF`, with the UID loaded from NVS if one is saved. |
| `STATS?` | One `S` line. |
| `BENCH <n>` | Needs `RX OFF`. Runs live captures at `PARK`, timed with CCOUNT: `BENCH n= cap_us= arm_us= read_us= det_us= demod_us= total_us= hits=` (p50/p99). |
| `RETUNE <mhzA> <mhzB> <n> FULL\|FAST` | `RETUNE call_us=p50/p99/max fails=k` |
| `LEAD <us>` | HOP lead-time override for the sweep; `0` means auto. |
| `SNAP16 <samples>` | `DATA <n> <crc32> <us>` followed by 2n bytes, the same frame as `CAP16`. Used for new fixtures. Deliberately not named `CAP16`. |

**Streamed lines**
- `P <t_us> <ch> <type R|S|M> <nonce|-> <cfo_khz> <snr_db> <fec> <data>`
  - For `R`, `data` is the 8-byte packet as 16 hex characters. The browser re-checks CRC14, which also catches UART corruption.
  - For `S`, `data` is `fhss,nonce,rate,tlm,swmode,mm`. UID bytes are never sent.
- `S` once per second, as `key=value`: `up mode state snaps bursts ok rc sync crcfail mirror dec_us cap_us ret_us lock_err_us cfo_khz gain drop`.
- `L <ACQ|LOCK|COAST|LOST> <reason>` and `W <text>` (for example `stale`, `clip`, `wide_mode`).
- Load: at most ~30 kB/s, about 15% of the UART.

## 9. Test strategy

### 9.1 Host tests

These run in CI via `python3 -m unittest discover -s tests`, following the `tests/test_pll_calibration.py` pattern: `cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined`, in `tests/test_elrs.py` plus `tests/elrs_*_test.c`.

- **OTA3:**
  - Encoder/decoder round-trip.
  - CRC with Hybrid slot 0; slots 1–2 detected as Wide.
  - 3.x SYNC layout.
  - Hybrid8 map for all seven indices, including the AUX8 16-position case.
  - Erasure decode up to 30 bits.
- **FHSS:**
  - Golden sequences for public phrases (`test`, `correct horse battery`), produced by a committed script that compiles the 3.6.4 `rng` and `FHSSrandomiseFHSSsequenceBuild` natively. Commit the script, not ELRS code.
  - Permutation property and sync positions.
  - Pointer/nonce mapping.
  - SYNC-cadence model: one SYNC per sync visit, with the slot alternating as in §4.
- **Real fixture:** `web/elrs/test/real_dk500.bin`, scaled from int8 to the 10-bit range, with UID `{0,0,0,0,0x13,0x3e}`. Assertions mirror `decode.test.js`:
  - ≥5 RC packets with sticks `[992, 996|997, 174, 992]`.
  - CFO 240 ± 20 kHz.
  - Both ghosts rejected.
  - A wrong UID decodes nothing.
- **Synthetic vectors (C, no numpy):**
  - 2-FSK at ±135 kHz, CFO −50…+300 kHz, any in-band channel, SNR 6–30 dB, a −25 dBc mirror.
  - Bursts truncated at the start and the end; DVDA pairs.
  - Assert decode rate against SNR, and zero false CRCs over 10⁴ noise-only snapshots.
- **Lock simulation:**
  - Simulated TX: UID schedule, ±100 ppm drift, 1/128 TLM gaps, 0–90% loss.
  - Run once with SYNC every 160 ms (primary) and once with SYNC every 5 s (triplet path).
  - Include a reboot and a nonce jump.
  - Assert time to lock, false locks under 0.1%, prediction error under 50 µs after 1 s, and COAST → LOST → reacquire.
- **Web:** `node --test web/elrs-rx/test/*.test.js` (line router, OTA3 and Hybrid8 parsing, firmware detection), added to `pages.yml`.

### 9.2 On-device: measure, decide, then ask the user **[C]**

The builder runs everything through esptool and serial; the user only handles the TX.

- **Timing:** `BENCH 500` at `PARK 2438`. **Target** p50 ≤ 6 ms total; this is a target, not a gate **[C]**. K1/F1 applies only above 15 ms.
- **Rate:** SCAN for 10 min, recording updates/s, hit rate and crcfail. Compare with the earlier Python live rate (3.3 snapshots/s, 3–16% hit rate).
- **Correctness:** *Ask the user* to:
  - move each stick to both extremes and centre;
  - flip every switch AUX1–AUX8 through all positions;
  - set a multi-position switch on AUX8.

  Compare with the radio's channel monitor. Over 10 min there must be no implausible values (CRC check plus agreement within each DVDA pair).

### 9.3 Retune latency (gates HOP)

1. `RETUNE 2402 2478 1000 FULL` and `... FAST` give the software call time, swept across the band.
2. **Settling:** in a TIMED lock, run HOP with a `LEAD` sweep from 100 to 3000 µs. Measure decode success and the CFO difference between each packet's first and last 48 bits. The knee of that curve is the retune-plus-settle latency.
3. **Per channel:** each channel must reach ≥90% of the TIMED decode rate with consistent CFO, or HOP excludes it.

### 9.4 Stock regression

- The stock `esp32` profile builds with unchanged sources.
- Stock firmware with `web/elrs/`: decodes as before, and `INFO` still answers `ESP32SDR 6 burst 16380`.
- `esp32-elrs` with `web/elrs/`: the page shows the redirect message.
- Stock firmware with `web/elrs-rx/`: the page shows the redirect message.

## 10. Web UI and site

### 10.1 New page `web/elrs-rx/` **[U]** layout C; details **[D]**

- **Files:**
  - `index.html` and `app.js`.
  - `link.js`: a line router over Web Serial (wrap or copy `SdrLink` from `../elrs/serial.js` **[O]**).
  - `ota3.js` and `test/`.
- **Reuse read-only:** import `../elrs/dk500.js` (`uidFromPhrase`, `unpack4x10`, `uint10ToCrsf`, `crc14`) and `../elrs/elrs.css`, `../style.css`.
- **Top row:**
  - Password field for the phrase, with **Set**.
  - "Remember phrase in this browser" (localStorage) and "Store UID on the device" (`UID SAVE` / `UID CLEAR`).
  - **Connect**.
  - Status: `updates/s · hit rate · channel · last packet age`, plus a badge `SCAN | LOCKED·TIMED | LOCKED·HOP | COAST`.
- **Gimbals:** Mode 2, CRSF 172–1811.
  - Left: yaw X, throttle Y (OTA indices 3 and 2).
  - Right: roll X, pitch Y (indices 0 and 1).
  - Grey out after 2 s without a packet.
- **AUX bars:** AUX1 is the arm bit. AUX2–AUX8 come from Hybrid8, and each bar updates when its slot comes round. On `W wide_mode`, show "TX uses Wide switch mode; set Hybrid".
- **Privacy:** never display the UID; show only "UID set".
- **Connect flow:**
  1. Open at 2 Mbaud and send `SYNC`.
  2. Send `INFO`:
     - first token `ESP32SDR`: show "This board runs stock ESP-SDR. Use the raw-I/Q viewer (`../elrs/`) or flash the ELRS receiver firmware (`../flash/`)";
     - any other token except `ESP32ELRS`: show "Unknown firmware".
  3. Send `UID <hex>`, `MODE AUTO`, `RX ON`.
  4. On disconnect, send `RX OFF` and `RELEASE`.
- **Stats:** updates/s comes from RC lines in a 10 s window; hit rate comes from `S ok/snaps`.

### 10.2 Existing pages

- **`web/elrs/app.js` [C]:** add an `INFO` check of about 10 lines in `connect()`, right after `synchronize()`. If the reply starts with `ESP32ELRS`, show "This board runs the ELRS receiver firmware; use the ELRS receiver page (`../elrs-rx/`)", then close. Nothing else changes, and stock behaviour is unaffected.
- **Upstream espargos.net viewer:** it can't be changed and will reject the unknown `INFO`, which is acceptable.
- **`web/index.html`:** three cards.
  - "ELRS stick viewer (raw I/Q) — needs **stock ESP-SDR** firmware".
  - "ELRS receiver (on-chip decode, fast) — needs **ELRS receiver** firmware".
  - "Flasher (both)".
- **`web/README.md`:** updated to match.

### 10.3 Flasher `web/flash/` **[U]** two choices; mechanics **[D]**

- **Two buttons, two manifests.** ESP Web Tools 10.4.0 takes one manifest per button.
  - `manifest.json`: stock, unchanged.
  - `manifest-elrs.json`: `chipFamily ESP32`, `firmware/esp32-elrs/0-bootloader.bin@0x1000`, `1-partition-table.bin@0x8000`, `2-esp_sdr.bin@0x10000`.
  - Each section shows its commit, its SHA-256 table and the source link.
- **Page note:** switching firmware means reflashing. The UID saved in NVS survives unless the user chooses erase.
- **Binaries: committed, but built by CI.** `tools/elrs/publish_web_firmware.py <run-id>`:
  1. `gh run download` the `firmware-esp32-elrs` artifact from a green `firmware.yml` run.
  2. Check that `build-info.json` `version` equals the commit, and verify SHA-256/MD5.
  3. Copy the files to `web/flash/firmware/esp32-elrs/`.
  4. Write `manifest-elrs.json` and the hash table.

  Why: this matches the committed stock binaries. Pages stays a static copy with no expiring cross-workflow artifacts or tokens. What is served is exactly what was reviewed, and the page names the GPL source commit. The cost is about 1 MB per release.

## 11. Build and CI **[D]**

- **`firmware-targets.json`:** add `{"id":"esp32-elrs","target":"esp32","label":"ESP32 · ELRS DK500 stick receiver (UART)","idf_ref":"25fe69f946311abdaf9ad56591f25fedbc20ac98","allow_larger_flash":true,"preview":true,"variant":"elrs","sdkconfig":"sdkconfig.defaults.esp32-elrs"}`. The `firmware.yml` matrix and `collect_firmware.py` pick it up.
- **`tools/build_firmware.py`:** pass `-DESP_SDR_ELRS=ON|OFF` from `variant`, with the component manager off.
- **Top-level `CMakeLists.txt`:** add `option(ESP_SDR_ELRS)`; require target `esp32`; exclude streaming.
- **`main/CMakeLists.txt`:** add an early branch, like the streaming one. It registers `elrs/*.c`, `targets/esp32/elrs/*.c` and the reused common sources, with the same `REQUIRES`, `sram_guard.ld`, `--wrap=chip_v7_set_chan_ana` and `rx_calibration_link.cmake`. It calls `generate_version.py --profile esp32-elrs`. The stock path below it stays untouched.
- **`sdkconfig.defaults.esp32-elrs`:** a copy of the ESP32 defaults; unicore stays until M5.
- **`pages.yml`:** the test step adds `web/elrs-rx/test/*.test.js`.
- **Commits** go directly to `main` on the fork as Akceptor noreply **[U]**. Nothing from `elrs-spike` is merged **[C]**.

## 12. Risks, mitigations, kill criteria

| # | Risk | Mitigation | Kill / fallback |
| --- | --- | --- | --- |
| R1 | DSP slower than estimated | FFT detector, int16 front end, locked fast path, M5 dual core | **K1:** `BENCH` p50 > 15 ms after optimisation → F1 (§5.4) |
| R2 | Fast retune leaves the PLL unlocked, or settling is slow | Per-channel qualification; full `tune_rx` fallback per channel | **K3:** settle > 15 ms or >2% of channels fail → TIMED only (still 50+/s) |
| R3 | AGC on timed captures (noise-level gain, clipping when the packet arrives) | Watch the gain index in word bits 20–27; `W clip`; `GAIN MANUAL` sweep against distance | Manual-gain default in timed modes **[O]** |
| R4 | False lock or drift | SYNC every 160 ms cross-checks; triplet confirmation; residual gating | **K2:** at the end of M3, lock error > 250 µs or false locks > 1% → ship step 1 |
| R5 | Background PHY activity replaces calibration | Check `rx_recalibration_stale` every cycle; re-tune; COAST | — |
| R6 | TX settings change (Wide, bound RX on, other rate) | SYNC field checks, `W` lines, UI messages; triplet path for a bound RX | — |
| R7 | Phrase or UID leakage | Never echoed; masked in SYNC lines; tests use UID[4..5] or public phrases; `elrs-spike` never merged | — |
| R8 | Stock breakage | Stock backend untouched; §9.4 | Revert the variant commits |
| R9 | Dual-core hazards | Single-core baseline; M5 gated by measurement | Stay single-core |

## 13. Milestones: measure → decide loops **[C]**

Each milestone ends with the builder measuring on the attached board, deciding against the exit criteria, and asking the user only for TX actions.

| M | Build | Measure, decide, then ask the user | Exit |
| --- | --- | --- | --- |
| **M0** | Variant skeleton: catalog, CMake, build script, sdkconfig. App answers `SYNC`/`INFO`/`CAPS`/`VERSION?`/`RELEASE`. Host test harness. | Flash `esp32-elrs`, check `INFO`. Flash stock, check `web/elrs/` still works. | CI builds both profiles; stock sources unchanged |
| **M1** | Host C `ota3`, `dsp`, `fhss` with fixture, synthetic and golden tests | — | Host tests green in CI |
| **M2 — Step 1 (ships)** | SCAN mode, P/S lines, `UID`/NVS, `BENCH`, `SNAP16`. `web/elrs-rx/`, `INFO` check in `web/elrs/`, landing cards, two-button flasher, published binaries, `docs/elrs-receiver.md`. | `BENCH`, then decide on optimisation or F1 (K1). *Ask the user:* TX on DK500 at 1 m. Run SCAN for 10 min. *Ask the user:* sticks to extremes, then **every AUX1–AUX8 switch through all positions** (Hybrid8). Verify each against the channel monitor. | **≥5 updates/s** at 1 m [C]; all sticks and AUX1–AUX8 correct; no implausible values in 10 min; §9.4 passes |
| **M3 — Step 2a** | `lock.c` (SYNC primary, triplet secondary) with simulation tests; TIMED mode; L lines | Measure time to lock from SCAN 20 times. *Ask the user* to power-cycle the TX 5 times and measure reacquire. Hold the lock for 10 min. Optional: *ask the user* to power the bound RX to exercise the triplet path. | Lock < 2 s typical; holds 10 min; **≥40 updates/s**; reacquires after a TX power cycle |
| **M4 — Step 2b** | `tune_fast`, `RETUNE`, `LEAD`, per-channel qualification, HOP, AUTO | Run §9.3, then decide K3 go or no-go and record it in the docs | If go: ≥100 updates/s |
| **M5** | Dual core if needed; polish | Re-run `BENCH` and the update rate | ≥1.5× gain, or revert |

## 14. Decision record

- **User [U]:**
  - ESP32-WROOM-32U only; UART/Web Serial at 2 Mbaud; no Wi-Fi.
  - The phrase is hashed in the browser; only the UID goes to the device, with optional NVS.
  - Layout C.
  - Existing pages kept; separate new page; flasher with two choices; distinct `INFO`.
  - Steps 1 and 2 designed together, with step 1 shipping alone.
  - Direct commits to `main`.
  - TX settings: Hybrid switches, TLM 1:128, ISM 2.4, bound RX usually off.
- **Orchestrator [C]:**
  - The builder runs the on-device work directly; the user handles only the TX.
  - 6 ms is a target; M2 ships at ≥5 updates/s; K1/F1 above 15 ms.
  - ELRS 3.6.4 as reference.
  - Add the `INFO` check to `web/elrs/app.js`.
  - `elrs-spike` is ported, never merged; the real phrase is never committed or pushed.
- **Design [D]:**
  - New files only, with copied ESP32 primitives.
  - FFT detector plus 2 MS/s per-burst demodulator.
  - Fixed-polarity mirror rejection.
  - TIMED before HOP.
  - SYNC-primary lock with triplet fallback.
  - Text line protocol with raw RC bytes re-checked in the browser.
  - `INFO` = `ESP32ELRS 1 elrs-rx 0`.
  - Committed CI-built binaries.
  - Names: `web/elrs-rx/`, `esp32-elrs`, `PARK 2438`.
- **Open [O]:** FFT size and pitch, filter taps, fixed versus float per stage, sentinel-check reduction, HOP LO offset, tracker gains, manual gain in timed modes, `SdrLink` reuse, dual core.
- **Questions for the user:** none outstanding.

**2026-10-23 — SDR waterfall (Task 13b, user request).**

§8 gains the command `SPECTRUM ON|OFF`: `OK`, RAM only, default `OFF`, allowed in any RX state. `CAPS` gains `SPECTRUM`.

There is a fifth streamed line, `F <t_us> <lo_mhz> <128 hex>`. It carries the detector's 64-bin peak-hold spectrum, DC-centred: hex byte j = bin centred at LO + (j - 32) * 250 kHz, value `clamp(round(20*log10(max(|X|^2, 1))), 0, 255)` in 0.5 dB steps. Each line is the max over all frames of all snapshots in a window of >= 100 ms measured on capture trigger timestamps (not wall clock), so at most 10 lines/s (measured about 8.4, about 1.2 kB/s); `t_us` is the window's first trigger time. The window restarts when the LO changes. Lines are sent only while `RX ON` and `SPECTRUM ON`. Clients route by first token, so `F` joins `P`/`S`/`L`/`W`.

§10: both pages show a waterfall panel next to the AUX bars, with 120 rows, newest on top, an auto-levelled 30 dB range, a MHz axis covering +-8 MHz around the LO, and decoded-packet channels marked. `web/elrs/` computes the same spectrum in the browser from each raw I/Q snapshot, with no firmware change. `web/elrs-rx/` sends `SPECTRUM ON` at connect and draws `F` lines; an `ERR` reply only disables the panel. The shared renderer lives in `web/elrs/waterfall.js` (elrs-rx -> elrs import direction, as for `dk500.js`).
