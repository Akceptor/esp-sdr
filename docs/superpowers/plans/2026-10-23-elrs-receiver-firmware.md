# ELRS DK500 Receiver Firmware (`esp32-elrs`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A separate ESP32 firmware variant that decodes ExpressLRS 3.x DK500 packets on the chip and streams them over UART to a new Web Serial page, first with a parked LO (step 1, ships alone), then with schedule-timed and frequency-hopping captures (step 2).

**Architecture:** Pure C11 DSP/protocol code in `main/elrs/` that builds both on the host (tests, macOS `cc`) and in ESP-IDF; a thin ESP32-only layer in `main/targets/esp32/elrs/` (copied radio primitives, command dispatch, scheduler, NVS, output ring); a new static page `web/elrs-rx/` that re-checks every packet's CRC in the browser. The stock backend and pages stay untouched apart from the additive `INFO` check in `web/elrs/app.js`.

**Tech Stack:** C11 (host `cc` + ESP-IDF `25fe69f946311abdaf9ad56591f25fedbc20ac98`, FreeRTOS, NVS), Python 3 stdlib `unittest` test drivers, numpy only for the one-off fixture exporter, ES modules + `node --test` (Node 22) for the page, ESP Web Tools 10.4.0.

**Spec:** `docs/superpowers/specs/2026-10-23-elrs-receiver-firmware-design.md` (section numbers below, e.g. §5.4, refer to it). Read it before any task.

## Global Constraints

- Never transmit: no TX paths, no telemetry replies, no loopback IQ calibration; boot-time PHY init is the stock one, unchanged (§2 anti-goals).
- Never edit `main/targets/esp32/receiver.c` or anything under `main/common/`; the stock `esp32` profile must build from unchanged sources. `main/CMakeLists.txt` and the top-level `CMakeLists.txt` only gain an additive early branch / option.
- Stock `INFO` must still answer `ESP32SDR 6 burst 16380`. The new firmware answers `ESP32ELRS 1 elrs-rx 0`.
- Never log, echo or send the full UID. `UID <hex>` is never echoed; SYNC `P` lines carry `mm` only, never UID bytes. Never display the UID in the page ("UID set" only).
- Never copy anything from branch `elrs-spike` that contains the real binding phrase (`tools/elrs/test_synth.py`). Port code by reading; never merge, cherry-pick or push `elrs-spike`. Tests use only UID `{0,0,0,0,0x13,0x3e}` (real fixtures) or public phrases `test` (UID `4f04fd822155`) and `correct horse battery` (UID `d755d89d3f5b`).
- ELRS reference is tag **3.6.4** at `/private/tmp/claude-502/-Users-vostapiv/f0b1eb44-2381-4dae-b924-a11da92ed9f3/scratchpad/elrs`. Commit no ELRS source code; only derived numbers.
- `main/elrs/*.c` is C11 with no ESP-IDF includes; host flags `cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -O2 ... -lm`. Front end in int16, per-burst math in `float` (never `double` in `dsp.c`).
- Host tests are Python `unittest` files in `tests/` (CI runs `python3 -m unittest discover -s tests`), C test programs are `tests/elrs_*_test.c`. Web tests: `node --test web/elrs-rx/test/*.test.js`.
- Serial: UART0, 2 Mbaud 8N1, `\n`-terminated ASCII lines, one reply line per command; streamed lines start with token `P`, `S`, `L` or `W` (§8).
- Profile id `esp32-elrs`, page `web/elrs-rx/`, default `PARK 2438`, NVS namespace `elrsrx`, key `uid` (6-byte blob).
- Code comments only where a reader would otherwise fall into a trap (hardware quirk, non-obvious reason); no narration comments.
- Git: commit directly on `main` as `3187448+Akceptor@users.noreply.github.com` (check `git config user.email` first). A global gitignore hides `*.json`/`*.html`: use `git add -f` for such files outside `web/`. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Pushing to `origin` happens only in steps that say so, and only after the orchestrator confirms.
- Build environment (verified): ESP-IDF `v6.1-dev-6376-g25fe69f946` at `~/esp/esp-idf`, ninja from Homebrew. Run `git submodule update --init` once per worktree first (`components/esp-dsp` v1.8.2 is otherwise missing). Firmware build command, with `$SCRATCH` = the session scratchpad and a fresh output directory each time (the script refuses an existing one): `. ~/esp/esp-idf/export.sh && python3 tools/build_firmware.py --profile <esp32|esp32-elrs> --version $(git rev-parse HEAD) --build-root $SCRATCH/build --output $SCRATCH/art-<n> --jobs 8`. Build trees and artifacts never go inside the repo.
- Hardware: ESP32-WROOM-32U at `/dev/cu.usbserial-585C0583231`. Stop any process holding the port before flashing. Steps marked **[HIL]** need the board; steps marked **[USER]** need the user to act on the transmitter. ESP-IDF: `. ~/esp/esp-idf/export.sh` (must be at the pinned ref; `tools/build_firmware.py` checks).

## Review Focus

1. **Garbage at connect / mid-line corruption** (board resets when the port opens; UART bit errors): the page must ignore malformed lines and reject any `P` line whose CRC14 fails or whose hex is not 16 chars, never render it. Test: `link.test.js` "malformed lines are ignored" and `ota3.test.js` "corrupted hex is rejected" (Task 11).
2. **Reconnect while the firmware is still streaming** (tab closed without `RX OFF`): `SYNC`/`INFO`/`UID` must still get their replies amid `P`/`S` lines, and `command()` must skip streamed lines. Test: `link.test.js` "command reply is found among streamed lines" (Task 11); HIL step in Task 10.
3. **Wrong binding phrase** (bursts seen, zero decodes): the user must be told, not left staring at a grey gimbal. Test: `app-state.test.js` "hint for bursts without packets" (Task 12) and C `wrong_uid_decodes_nothing` (Task 6).
4. **Malformed or out-of-state commands** (`UID` with 11/13 chars or non-hex, `PARK 2401`, `RX ON` without a UID, `BENCH`/`SNAP16` while `RX ON`, overlong line): specific `ERR` token, no state change, no echo. Tests: `elrs_cmd_test.c` (Task 2), HIL command checks in Task 10.
5. **Strong signal at 1 m (ADC clipping)**: decoding must still work on clipped bursts and the firmware must say `W clip`. Test: `synth_clipped_still_decodes` (Task 6) and the clip counter in `elrs_convert` (Task 5).

---

## File Structure

| Path | Responsibility | Task |
| --- | --- | --- |
| `firmware-targets.json` | Add the `esp32-elrs` profile | 1 |
| `tools/build_firmware.py` | Pass `-DESP_SDR_ELRS=ON\|OFF` from `variant` | 1 |
| `tools/export_web_firmware.py` | Accept hyphenated profile ids in the embedded version record | 1 |
| `tests/test_elrs_variant.py` | Catalog, build-command and CMake-branch checks | 1, 2 |
| `CMakeLists.txt` | `option(ESP_SDR_ELRS)`, target/streaming guards, build property | 2 |
| `main/CMakeLists.txt` | Early `ESP_SDR_ELRS` branch with `return()`; stock path below unchanged | 2 |
| `sdkconfig.defaults.esp32-elrs` | Copy of ESP32 defaults plus ELRS task settings | 2 |
| `main/elrs/cmd.c/.h` | Pure command-line parser (all §8 commands) | 2 |
| `main/elrs/ota3.c/.h` | Whitening, FEC (with tail erasures), CRC14 + slot, 3.x SYNC, 4x10, Hybrid8 | 3 |
| `main/elrs/fhss.c/.h` | rng, seed, 240-entry sequence, pointer/nonce mapping, channel frequency | 4 |
| `tools/elrs/fhss_golden.py` | Compiles ELRS 3.6.4 rng + sequence build natively, writes the golden header | 4 |
| `tests/elrs_fhss_golden.h` | Generated golden sequences for public phrases | 4 |
| `main/elrs/dsp.c/.h` | Convert, 64-pt FFT, detector, overlap drop, per-burst demod, `elrs_decode_snapshot` | 5, 6 |
| `tests/elrs_synth.h` | Test-only TX/channel model producing bank-format words | 5 |
| `tests/elrs_fixture.h` | Test-only loader: int8 fixture → bank-format words | 5 |
| `tools/elrs/export_fixture.py`, `tools/elrs/golden_from_js.mjs` | One-off fixture exporter and JS golden list | 7 |
| `tests/fixtures/elrs/caps_rc.bin`, `caps_rc.json` | Committed RC-only subset of the real 300-snapshot capture | 7 |
| `main/elrs/frames.c/.h` | `P`/`S`/`L`/`W` line formatting | 8 |
| `main/targets/esp32/elrs/radio.c/.h` | Copied ESP32 RX primitives, arm/trigger/finish, later `tune_fast` | 9, 18 |
| `main/targets/esp32/elrs/out.c/.h` | Output line ring, TX task, direct-write lock | 10 |
| `main/targets/esp32/elrs/app.c` | `app_main`, command dispatch, radio task, modes, NVS, stats | 2, 9, 10, 17, 19 |
| `docs/elrs-receiver.md` | Protocol, usage, measurements, decisions (K1/K2/K3) | 10, 15, 17, 19 |
| `web/elrs-rx/ota3.js` | P/S/L/W line parsing, CRC re-check, Hybrid8 state | 11 |
| `web/elrs-rx/link.js` | Line router over `SdrLink`, firmware detection | 11 |
| `web/elrs-rx/state.js` | Pure UI state: rates, badge, ages, hints | 12 |
| `web/elrs-rx/index.html`, `app.js`, `elrs-rx.css` | Layout C UI | 12 |
| `web/elrs-rx/test/*.test.js` | Node tests | 11, 12 |
| `web/elrs/app.js` | Additive `INFO` check only | 13 |
| `web/index.html`, `web/README.md`, `.github/workflows/pages.yml` | Three cards, docs, test step | 11, 13 |
| `web/flash/index.html`, `web/flash/manifest-elrs.json`, `web/flash/firmware/esp32-elrs/` | Second install button, CI-built binaries | 14 |
| `tools/elrs/publish_web_firmware.py`, `tests/test_publish_elrs_firmware.py` | Publish CI artifact into the flasher | 14 |
| `main/elrs/lock.c/.h` | Acquisition (SYNC primary, RC triplet), tracking, prediction | 16 |
| `tests/elrs_txsim.h` | Test-only ELRS 3.6.4 TX timing model (SYNC cadence, TLM gaps, drift) | 4, 16 |

C test programs: `tests/elrs_cmd_test.c`, `elrs_ota3_test.c`, `elrs_fhss_test.c`, `elrs_dsp_test.c`, `elrs_decode_test.c`, `elrs_caps_test.c`, `elrs_frames_test.c`, `elrs_lock_test.c`, all driven from `tests/test_elrs.py`.

---

## M0 — Variant skeleton

### Task 1: Catalog, build script and exporter accept `esp32-elrs`

**Files:**
- Modify: `firmware-targets.json` (append one profile)
- Modify: `tools/build_firmware.py:52` (command list)
- Modify: `tools/export_web_firmware.py:39` (profile regex in `firmware_version`)
- Create: `tests/test_elrs_variant.py`
- Modify: `tests/test_firmware_export.py` (one new test)

**Interfaces:**
- Produces: catalog profile exactly `{"id":"esp32-elrs","target":"esp32","label":"ESP32 · ELRS DK500 stick receiver (UART)","idf_ref":"25fe69f946311abdaf9ad56591f25fedbc20ac98","allow_larger_flash":true,"preview":true,"variant":"elrs","sdkconfig":"sdkconfig.defaults.esp32-elrs"}`; `build_firmware.build_command(profile: dict, build: Path, sdk: str) -> list[str]` (extracted from `main()` so it is testable); every non-streaming command contains `-DESP_SDR_ELRS=ON` iff `profile.get('variant') == 'elrs'`, else `-DESP_SDR_ELRS=OFF`.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_elrs_variant.py
class ElrsVariant(unittest.TestCase):
    def test_catalog_profile(self):
        p = {x['id']: x for x in json.loads((ROOT/'firmware-targets.json').read_text())['profiles']}['esp32-elrs']
        self.assertEqual(p, {"id":"esp32-elrs","target":"esp32","label":"ESP32 · ELRS DK500 stick receiver (UART)",
            "idf_ref":"25fe69f946311abdaf9ad56591f25fedbc20ac98","allow_larger_flash":True,"preview":True,
            "variant":"elrs","sdkconfig":"sdkconfig.defaults.esp32-elrs"})
    def test_build_command_flags(self):
        elrs = builder.build_command(profiles['esp32-elrs'], Path('/b'), '/sdk')
        stock = builder.build_command(profiles['esp32'], Path('/b'), '/sdk')
        self.assertIn('-DESP_SDR_ELRS=ON', elrs)
        self.assertIn('-DESP_SDR_ELRS=OFF', stock)
        self.assertIn('-DSDKCONFIG_DEFAULTS=' + str(ROOT/'sdkconfig.defaults.esp32-elrs'), elrs)
        self.assertIn('-DIDF_TARGET=esp32', elrs)
```

In `tests/test_firmware_export.py` add `test_hyphenated_profile_record`: embed `ESP-SDR-VERSION:{"revision":"<40 hex>","build_date":"2026-10-23","build_timestamp":"2026-10-23T00:00:00Z","profile":"esp32-elrs"}\0` in the fixture's `app.bin`, export with board `esp32-elrs` and target `esp32`, assert `manifest['variants']['esp32-elrs']['git_revision']` equals the revision. Also assert profile `esp32s3-` (trailing hyphen) still raises `ValueError`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tests -k ElrsVariant -k hyphenated -v`
Expected: FAIL (`KeyError: 'esp32-elrs'`, `AttributeError: build_command`, export `ValueError: Invalid embedded firmware version`).

- [ ] **Step 3: Implement**

Append the profile to `firmware-targets.json` (keep `"unavailable": {}` last). Extract `build_command()` in `tools/build_firmware.py` and add the `ESP_SDR_ELRS` define next to `ESP_SDR_STREAMING`. Change the profile regex in `firmware_version` to `esp32[a-z0-9]*(?:-[a-z0-9]+)*`.

- [ ] **Step 4: Run all host tests**

Run: `python3 -m unittest discover -s tests`
Expected: all PASS (stock tests unchanged).

- [ ] **Step 5: Commit**

```bash
git add firmware-targets.json tools/build_firmware.py tools/export_web_firmware.py tests/test_elrs_variant.py tests/test_firmware_export.py
git commit -m "build: add esp32-elrs catalog profile and ELRS build flag"
```

### Task 2: Firmware skeleton and command parser

**Files:**
- Modify: `CMakeLists.txt` (after the streaming block)
- Modify: `main/CMakeLists.txt` (new block at the very top, ends in `return()`)
- Create: `sdkconfig.defaults.esp32-elrs`
- Create: `main/elrs/cmd.h`, `main/elrs/cmd.c`
- Create: `main/targets/esp32/elrs/app.c`
- Create: `tests/test_elrs.py`, `tests/elrs_cmd_test.c`
- Modify: `tests/test_elrs_variant.py`

**Interfaces:**
- Produces (`main/elrs/cmd.h`):

```c
typedef enum { ELRS_MODE_AUTO, ELRS_MODE_SCAN, ELRS_MODE_TIMED, ELRS_MODE_HOP } elrs_mode_t;
typedef enum { ELRS_POL_INV, ELRS_POL_AUTO } elrs_polarity_t;
typedef enum {
    ELRS_CMD_INVALID, ELRS_CMD_INFO, ELRS_CMD_CAPS, ELRS_CMD_SYNC, ELRS_CMD_TRANSPORT,
    ELRS_CMD_UID_SET, ELRS_CMD_UID_QUERY, ELRS_CMD_UID_SAVE, ELRS_CMD_UID_CLEAR,
    ELRS_CMD_MODE, ELRS_CMD_MODE_QUERY, ELRS_CMD_PARK, ELRS_CMD_PARK_QUERY,
    ELRS_CMD_GAIN_HARDWARE, ELRS_CMD_GAIN_MANUAL, ELRS_CMD_GAIN_QUERY,
    ELRS_CMD_RX_ON, ELRS_CMD_RX_OFF, ELRS_CMD_STATS, ELRS_CMD_BENCH, ELRS_CMD_RETUNE,
    ELRS_CMD_LEAD, ELRS_CMD_SNAP16, ELRS_CMD_POLARITY
} elrs_cmd_kind_t;
typedef struct {
    elrs_cmd_kind_t kind;
    const char *err;          /* INVALID only: "uid", "args" or "command" */
    union {
        uint8_t uid[6]; elrs_mode_t mode; elrs_polarity_t polarity; uint64_t nonce;
        unsigned park_mhz, gain, bench_n, lead_us, snap_samples;
        struct { unsigned mhz_a, mhz_b, n; bool fast; } retune;
    } u;
} elrs_cmd_t;
void elrs_cmd_parse(const char *line, elrs_cmd_t *out);
const char *elrs_mode_name(elrs_mode_t m);   /* "AUTO" ... "HOP" */
```

- Produces (`app.c`): `static void reply(const char *fmt, ...)` and `static void command(char *line)` served by `burst_serial_serve()`; replaced by the output ring in Task 10.
- Ranges: `PARK` 2402..2478; `GAIN MANUAL` 0..127 (runtime check against `gain_max` in app); `BENCH` 1..2000; `SNAP16` 256..16380; `RETUNE` both MHz in 2402..2478, n 1..10000, `FULL|FAST`; `LEAD` 0..5000; `UID` exactly 12 hex digits, either case; `POLARITY INV|AUTO` (debug, §5.5). Trailing garbage → `INVALID`/`"args"`. Unknown verb → `INVALID`/`"command"`. Bad UID → `"uid"`.

- [ ] **Step 1: Write the failing test harness and parser test**

`tests/test_elrs.py`: class `ElrsHost(unittest.TestCase)` skipped unless `shutil.which('cc')`; helper `run_c(self, test_name: str, sources: list[str], args: list[str] = ())` compiles `tests/elrs_<test_name>_test.c` plus `main/elrs/<src>.c` with the Global Constraints flags and `-Imain/elrs -Itests`, runs it with `args`, asserts exit 0 and returns stdout. Method `test_cmd` → `run_c('cmd', ['cmd'])`.

`tests/elrs_cmd_test.c` uses `assert` and covers at least:

```c
elrs_cmd_t c;
elrs_cmd_parse("INFO", &c);                 assert(c.kind == ELRS_CMD_INFO);
elrs_cmd_parse("UID 00000000133E", &c);     assert(c.kind == ELRS_CMD_UID_SET && c.u.uid[4] == 0x13 && c.u.uid[5] == 0x3e);
elrs_cmd_parse("UID 00000000133", &c);      assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, "uid"));
elrs_cmd_parse("UID 00000000133e0", &c);    assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, "uid"));
elrs_cmd_parse("UID 0000000013xe", &c);     assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, "uid"));
elrs_cmd_parse("UID?", &c);                 assert(c.kind == ELRS_CMD_UID_QUERY);
elrs_cmd_parse("PARK 2402", &c);            assert(c.kind == ELRS_CMD_PARK && c.u.park_mhz == 2402);
elrs_cmd_parse("PARK 2401", &c);            assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, "args"));
elrs_cmd_parse("PARK 2479", &c);            assert(c.kind == ELRS_CMD_INVALID);
elrs_cmd_parse("MODE HOP", &c);             assert(c.kind == ELRS_CMD_MODE && c.u.mode == ELRS_MODE_HOP);
elrs_cmd_parse("RETUNE 2402 2478 1000 FAST", &c); assert(c.kind == ELRS_CMD_RETUNE && c.u.retune.fast && c.u.retune.n == 1000);
elrs_cmd_parse("SNAP16 255", &c);           assert(c.kind == ELRS_CMD_INVALID);
elrs_cmd_parse("SYNC 18446744073709551615", &c); assert(c.kind == ELRS_CMD_SYNC && c.u.nonce == UINT64_MAX);
elrs_cmd_parse("RX ON x", &c);              assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, "args"));
elrs_cmd_parse("CAP16 16380 6", &c);        assert(c.kind == ELRS_CMD_INVALID && !strcmp(c.err, "command"));
```

`tests/test_elrs_variant.py::test_cmake_branch_is_early_and_returns`: the first non-comment line of `main/CMakeLists.txt` is `idf_build_get_property(elrs ESP_SDR_ELRS)`, the block contains `generate_version.py`, `--profile`, `esp32-elrs`, `sram_guard.ld`, `--wrap=chip_v7_set_chan_ana`, `rx_calibration_link.cmake`, and its `return()` precedes `set(esp_sdr_supported_targets`.

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m unittest discover -s tests -k test_cmd -k test_cmake_branch -v`
Expected: FAIL (missing `cmd.c`, missing CMake block).

- [ ] **Step 3: Implement `elrs_cmd_parse` and `elrs_mode_name` in `main/elrs/cmd.c`**

`sscanf` with a trailing `%c` sentinel for "no extra tokens", as `burst_commands.h` does.

- [ ] **Step 4: Implement the build branch and skeleton app**

- `CMakeLists.txt`: `option(ESP_SDR_ELRS "Build the ESP32 ELRS DK500 receiver" OFF)`; if ON and `IDF_TARGET` is not `esp32` or `ESP_SDR_STREAMING` is ON → `FATAL_ERROR`; `idf_build_set_property(ESP_SDR_ELRS "${ESP_SDR_ELRS}")` next to the streaming property.
- `main/CMakeLists.txt` top block: register `elrs/*.c` (glob or explicit list), `targets/esp32/elrs/*.c`, and the reused `common/burst_serial.c common/burst_gpio.c common/burst_version.c common/rx_recalibration.c common/rx_calibration_state.c` (`burst_gpio.c` is required because `burst_serial_init()` calls `burst_gpio_init()`); `PRIV_INCLUDE_DIRS "elrs" "common" "targets/esp32" "targets/esp32/elrs"`; `REQUIRES esp_driver_gpio esp_hw_support esp_phy esp_wifi nvs_flash esp_timer esp_driver_uart`; same version custom target with `--profile esp32-elrs`; `-T…/targets/esp32/sram_guard.ld`; `-Wl,--wrap=chip_v7_set_chan_ana`; `include("common/rx_calibration_link.cmake")`; `return()`.
- `sdkconfig.defaults.esp32-elrs`: byte copy of `sdkconfig.defaults.esp32` plus the line from `sdkconfig.defaults` (`CONFIG_ESP_SDR_UART_ENABLED=y`, `CONFIG_ESP_SDR_UART_BAUD=2000000`), plus `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n` (the radio task in Task 10 runs continuously). Unicore stays.
- `app.c`: `app_main` performs the exact stock boot sequence from `receiver.c` (`nvs_flash_init` with erase-retry, default event loop, `esp_wifi_init`, RAM storage, `WIFI_MODE_NULL`, start, PS none, promiscuous, channel 1), then `burst_serial_init()` and `burst_serial_serve(command)`. `command()` handles `VERSION?` via `burst_version_command`, then `elrs_cmd_parse`: `INFO` → `ESP32ELRS 1 elrs-rx 0`; `CAPS` → `CAPS VERSION ELRSRX DK500 OTA3`; `SYNC n` → `SYNC n`; `TRANSPORT?` → `TRANSPORT UART <baud>`; `INVALID` → `ERR <err>`; every other kind → `ERR unsupported` for now. `RELEASE` and `BAUD` are already handled inside `burst_serial`.

- [ ] **Step 5: Run host tests**

Run: `python3 -m unittest discover -s tests`
Expected: all PASS.

- [ ] **Step 6: Build both ESP32 profiles locally**

Run: `git submodule update --init && . ~/esp/esp-idf/export.sh && python3 tools/build_firmware.py --profile esp32 --version $(git rev-parse HEAD) --build-root $SCRATCH/build --output $SCRATCH/art-1 --jobs 8 && python3 tools/build_firmware.py --profile esp32-elrs --version $(git rev-parse HEAD) --build-root $SCRATCH/build --output $SCRATCH/art-1 --jobs 8`
Expected: both print an output path, and `$SCRATCH/art-1/esp32-elrs/manifest.json` contains `"git_revision"` (the embedded `esp32-elrs` record passed the exporter's profile check).

- [ ] **Step 7: [HIL] Flash and check identity**

Run: `python -m esptool --chip esp32 --port /dev/cu.usbserial-585C0583231 write-flash @flash_args` (run from inside `$SCRATCH/art-1/esp32-elrs/..`, i.e. the profile's output folder that holds `flash_args`), then a 2 Mbaud serial script sends `SYNC 7`, `INFO`, `CAPS`, `VERSION?`, `UID 0000000013`, `RELEASE`.
Expected: `SYNC 7`, `ESP32ELRS 1 elrs-rx 0`, `CAPS VERSION ELRSRX DK500 OTA3`, `VERSION {..."profile":"esp32-elrs"...}`, `ERR uid`, `OK`.

- [ ] **Step 8: [HIL] Stock regression**

Flash the stock profile's `flash_args` from its own output folder; `INFO` → `ESP32SDR 6 burst 16380`; open `web/elrs/` (served with `python3 -m http.server` from `web/`) in Chrome and confirm bursts are counted. Leave the board on `esp32-elrs` afterwards.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt main/CMakeLists.txt sdkconfig.defaults.esp32-elrs main/elrs/cmd.c main/elrs/cmd.h main/targets/esp32/elrs/app.c tests/test_elrs.py tests/elrs_cmd_test.c tests/test_elrs_variant.py
git commit -m "esp32-elrs: variant skeleton answering INFO/CAPS/SYNC/VERSION"
```

---

## M1 — Host decoder

### Task 3: OTA3 packet layer

**Files:**
- Create: `main/elrs/ota3.h`, `main/elrs/ota3.c`, `tests/elrs_ota3_test.c`
- Modify: `tests/test_elrs.py` (`test_ota3`)

**Interfaces:**
- Produces (`ota3.h`):

```c
enum { ELRS_PT_RC = 0, ELRS_PT_MSP = 1, ELRS_PT_SYNC = 2, ELRS_PT_TLM = 3 };
#define ELRS_AIR_BYTES 14
void elrs_whiten(uint8_t *buf, size_t n);                     /* 9-bit LFSR x^9+x^5+1, seed 0x100, bit-reversed keystream; involution */
void elrs_fec_encode(const uint8_t pkt[8], uint8_t air[14]);  /* Hamming(7,4) + interleave, as dk500.js fecEncode */
unsigned elrs_fec_decode(const uint8_t air[14], unsigned missing_tail_bits, uint8_t pkt[8]); /* returns corrected-bit count; erased bits ignored */
void elrs_air_encode(const uint8_t pkt[8], uint8_t air[14]);  /* fec then whiten */
unsigned elrs_air_decode(const uint8_t air[14], unsigned missing_tail_bits, uint8_t pkt[8]); /* whiten then fec */
uint16_t elrs_crc14(const uint8_t *data, size_t n, uint16_t init);  /* poly 0x2E57 */
uint16_t elrs_crc_init(const uint8_t uid[6]);                 /* ((uid[4]<<8)|uid[5]) ^ 3 */
int elrs_crc_slot(const uint8_t pkt[8], uint16_t init);       /* 0 = valid with crcHigh field 0 (Hybrid / any non-RC type); 1 or 2 = RC valid only with Wide slot; -1 = invalid */
void elrs_finish_packet(uint8_t pkt[8], uint16_t init, unsigned slot); /* fills crcHigh/crcLow over bytes 0..6 with byte0 = type | slot<<2 */
typedef struct { uint8_t fhss_index, nonce, sw_mode, tlm_ratio, rate_index, uid3, uid4, uid5x; } elrs_sync_t;
void elrs_parse_sync(const uint8_t pkt[8], elrs_sync_t *s);   /* 3.x layout §4: byte3 bit0 swmode, bits1-3 tlm, bits4-7 rate */
bool elrs_sync_uid_ok(const elrs_sync_t *s, const uint8_t uid[6], uint8_t *model_id); /* uid3/uid4 equal, (uid5x^uid[5]) & 0xc0 == 0; model_id = ~(uid5x^uid[5]) & 0x3f */
void elrs_unpack_4x10(const uint8_t raw[5], uint16_t out[4]);
uint16_t elrs_uint10_to_crsf(uint16_t v);                      /* fmap(v,0,1023,172,1811) with dk500.js rounding */
bool elrs_hybrid8(uint8_t switches7, uint8_t *aux, uint16_t *crsf); /* aux 2..8; idx>=6 -> AUX8 = (v&15)*1601/15+191; else SWITCH3b {0:191,5:1792,6:992,7:992, else v*240+391} */
```

CRC detail (ELRS 3.6.4 `OTA.cpp:ValidatePacketCrcStd`): the CRC runs over bytes 0–6 with the 6-bit `crcHigh` field of byte 0 replaced by 0 (Hybrid, and every non-RC type), or by slot `1|2` (Wide RC); the type bits stay.

- [ ] **Step 1: Write the failing test** — `tests/elrs_ota3_test.c`, values computed from the proven `web/elrs/dk500.js`:

```c
static const uint8_t uid45[6] = {0,0,0,0,0x13,0x3e};
uint8_t ks[14] = {0}; elrs_whiten(ks, 14);
assert(!memcmp(ks, (uint8_t[]){0x00,0x88,0xc8,0xea,0xd8,0xe2,0x54,0x6c,0xf9,0xe2,0xdc,0xa4,0x13,0x3a}, 14));
assert(elrs_crc_init(uid45) == 0x133d);
assert(elrs_crc14((uint8_t[]){0,1,2,3,4,5,6}, 7, 0x133d) == 0x3e95);
uint8_t pkt[8] = {0,1,2,3,4,5,6,0}; elrs_finish_packet(pkt, 0x133d, 0);
assert(!memcmp(pkt, (uint8_t[]){0xf8,0x01,0x02,0x03,0x04,0x05,0x06,0x95}, 8));
uint8_t air[14]; elrs_air_encode(pkt, air);
assert(!memcmp(air, (uint8_t[]){0x46,0x4c,0x9a,0xfa,0xda,0xb7,0x57,0xec,0xbe,0xf3,0xcb,0xf0,0x05,0xbb}, 14));
```

Further cases in the same file:
- `round_trip`: 1000 random packets × slot {0,1,2} → encode → flip 1 bit in each of up to 5 distinct codewords → `elrs_air_decode` returns the packet and the flip count.
- `erasures`: for `missing` in 0..30, zero the last `missing` air bits (MSB-first) → decode with `missing` returns the original packet; with 31 missing the function is still memory-safe.
- `crc_slots`: Hybrid RC packet → slot 0; same payload finished with slot 1 → `elrs_crc_slot` returns 1, slot 2 → 2; SYNC finished with slot 0 → 0; one flipped bit → -1.
- `sync_layout`: bytes `{0x02|crc, 160, 77, 0xE3, 0xAA, 0x13, 0x3e^(~5&0x3f), crc}` → `fhss_index 160, nonce 77, sw_mode 1, tlm_ratio 1, rate_index 14`; `elrs_sync_uid_ok` with `uid {0,0,0,0xAA,0x13,0x3e}` true and `model_id == 5`; with uid3 `0xAB` false.
- `hybrid8`: idx 0..5 with v 0..7 → aux `2+idx` and the SWITCH3b table (v=1 → 631, v=5 → 1792, v=6 → 992); `0x37` → aux 8, 938; `0x38` → aux 8, 1044; `0x30` → 191; `0x3f` → 1792.
- `sticks`: `elrs_unpack_4x10` of `pack4x10` of `[crsfToUint10(992), ...]` vectors from dk500.js reproduces `[992, 996, 174, 992]` via `elrs_uint10_to_crsf`.

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m unittest discover -s tests -k test_ota3 -v`
Expected: FAIL (compile error, `ota3.h` missing).

- [ ] **Step 3: Implement `main/elrs/ota3.c`** by porting `web/elrs/dk500.js` (`HAMMING_CODES`, `HAMMING_PACKED_DECODE`, `deinterleave`, `fecDecodeErasures`, CRC table) with the slot rule above.

- [ ] **Step 4: Run test to verify pass**

Run: `python3 -m unittest discover -s tests -k test_ota3 -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add main/elrs/ota3.c main/elrs/ota3.h tests/elrs_ota3_test.c tests/test_elrs.py
git commit -m "elrs: OTA3 whitening, FEC with erasures, CRC14 slots, 3.x SYNC"
```

### Task 4: FHSS sequence, golden generator and SYNC-cadence model

**Files:**
- Create: `main/elrs/fhss.h`, `main/elrs/fhss.c`
- Create: `tools/elrs/fhss_golden.py`, `tests/elrs_fhss_golden.h` (generated, committed)
- Create: `tests/elrs_txsim.h`, `tests/elrs_fhss_test.c`
- Modify: `tests/test_elrs.py` (`test_fhss`)

**Interfaces:**
- Produces (`fhss.h`):

```c
#define ELRS_NCH 80
#define ELRS_SEQ_LEN 240
#define ELRS_SYNC_CH 41
uint32_t elrs_uid_seed(const uint8_t uid[6]);          /* uid2<<24|uid3<<16|uid4<<8|(uid5^3) */
void elrs_fhss_build(uint32_t seed, uint8_t seq[ELRS_SEQ_LEN]);
static inline uint32_t elrs_channel_khz(unsigned ch) { return 2400400u + 1000u * ch; }
unsigned elrs_fhss_ptr(unsigned ptr0, int64_t n0, int64_t n); /* (ptr0 + floor(n/2) - floor(n0/2)) mod 240, unwrapped nonce counters */
```

- Produces (`tests/elrs_txsim.h`, test-only, header-only):

```c
typedef struct { uint8_t uid[6]; double ppm; double loss; unsigned tlm_denom; bool connected; uint32_t rng; /* private: */ int64_t nonce; unsigned ptr; unsigned sync_slot; uint32_t last_sync_ms; double t_us; uint8_t seq[240]; } txsim_t;
typedef struct { double t_us; uint8_t channel; uint8_t type; int64_t nonce; unsigned ptr; bool lost; } txsim_pkt_t;
void txsim_init(txsim_t *tx, const uint8_t uid[6], double ppm, double loss, uint32_t rng_seed);
bool txsim_next(txsim_t *tx, txsim_pkt_t *out);   /* advances one 1000 µs tick (scaled by 1+ppm*1e-6); false on a TLM slot (no packet) */
void txsim_reboot(txsim_t *tx, int64_t new_nonce, unsigned new_ptr);
```

`txsim_next` mirrors 3.6.4 `timerCallback` + `SendRCdataToRF` + `TXdoneISR`: `nonce++`; TLM slot when `nonce % tlm_denom == 0` (default 128); SYNC when `(sync_slot/2) <= nonce%2 && now_ms - last_sync_ms > interval && seq[ptr]==41` (interval 3 ms disconnected, 5000 ms connected), then `sync_slot = (sync_slot+1) % 4`; after the packet, hop (`ptr = (ptr+1) % 240`) when `(nonce+1) % 2 == 0`.

- [ ] **Step 1: Write the golden generator `tools/elrs/fhss_golden.py`**

`fhss_golden.py --elrs <checkout> --tag 3.6.4 --output tests/elrs_fhss_golden.h`: `git -C <checkout> show <tag>:src/lib/FHSS/random.cpp` / `random.h` into a temp dir; extract the `FHSSrandomiseFHSSsequenceBuild` definition from `src/lib/FHSS/FHSS.cpp` by brace matching; write a C++ harness that defines `FHSSptr`, `FHSSgetSequenceCount()` returning 240, empty `DBG`/`DBGCR`; compile with `c++ -std=c++17`; for phrases `test` and `correct horse battery` compute UID = `md5(b'-DMY_BINDING_PHRASE="' + phrase + b'"')[:6]` and seed per `uidMacSeedGet`; print the header with `static const uint8_t golden_uid_<n>[6]`, `golden_seed_<n>`, `golden_seq_<n>[240]`, and the tag + ELRS commit hash in a comment. Commit the script and its output only.

- [ ] **Step 2: Generate and sanity-check the golden header**

Run: `python3 tools/elrs/fhss_golden.py --elrs /private/tmp/claude-502/-Users-vostapiv/f0b1eb44-2381-4dae-b924-a11da92ed9f3/scratchpad/elrs --tag 3.6.4 --output tests/elrs_fhss_golden.h && grep -c , tests/elrs_fhss_golden.h`
Expected: header written; `test` UID `4f,04,fd,82,21,55`, seed `0xfd822156`, sequence starts `41, 27, 47, 26, 44, 13, 22, 18, 52, 15, 38, 21`.

- [ ] **Step 3: Write the failing C test `tests/elrs_fhss_test.c`**

- `golden`: `elrs_uid_seed(golden_uid_test) == golden_seed_test`; `elrs_fhss_build` equals `golden_seq_test` and `golden_seq_chb` byte for byte.
- `permutation`: for 1000 random seeds, each 80-block is a permutation of 0..79 and `seq[0]==seq[80]==seq[160]==41`.
- `ptr_mapping`: `elrs_fhss_ptr(10, 6, 7) == 10`, `elrs_fhss_ptr(10, 6, 8) == 11`, `elrs_fhss_ptr(10, 7, 8) == 11`, `elrs_fhss_ptr(239, 0, 2) == 0`, and across nonce wrap (`n0 = 254`, `n = 258`) gives `+2`.
- `sync_cadence`: txsim disconnected, UID `test`, 0 ppm, 0 loss, 80 000 ticks: SYNC count per sync-channel visit is exactly 1 (visits = runs where `seq[ptr]==41`), consecutive SYNCs are 160 ms ± 1 ms apart (tick count 160), and the SYNC's `nonce%2` pattern over 4 consecutive visits is `{0,0,1,1}` (cyclic); with `connected = true` SYNCs are ≥ 5000 ticks apart; TLM gaps occur once per 128 ticks.

- [ ] **Step 4: Run to verify failure, implement `fhss.c`, run to verify pass**

Run: `python3 -m unittest discover -s tests -k test_fhss -v`
Expected: first FAIL (missing `fhss.h`), then PASS after implementing (`rng` with uint32 wrap, `% 2^31`, `>>16`; `rngN(m) = rng() % m`).

- [ ] **Step 5: Commit**

```bash
git add main/elrs/fhss.c main/elrs/fhss.h tools/elrs/fhss_golden.py tests/elrs_fhss_golden.h tests/elrs_txsim.h tests/elrs_fhss_test.c tests/test_elrs.py
git commit -m "elrs: FHSS sequence with ELRS 3.6.4 golden vectors and TX timing model"
```

### Task 5: DSP front end — convert, FFT detector, overlap drop

**Files:**
- Create: `main/elrs/dsp.h`, `main/elrs/dsp.c` (front-end half)
- Create: `tests/elrs_synth.h`, `tests/elrs_fixture.h`, `tests/elrs_dsp_test.c`
- Modify: `tests/test_elrs.py` (`test_dsp`)

**Interfaces:**
- Consumes: `elrs_air_encode`, `elrs_finish_packet`, `elrs_crc_init` (Task 3); `elrs_channel_khz` (Task 4).
- Produces (`dsp.h`, front end):

```c
#define ELRS_FS_HZ 16000000
#define ELRS_MAX_SAMPLES 16380
#define ELRS_FFT_N 64
#define ELRS_FRAME_STEP 256          /* 16 µs; 64 frames per 16380-sample snapshot */
#define ELRS_INBAND_KHZ 7500         /* |f_ch - LO| limit; see note */
#define ELRS_MAX_BURSTS 8
typedef struct { int16_t i, q; } elrs_iq16_t;
typedef struct { unsigned clipped; uint8_t gain; int16_t mean_i, mean_q; } elrs_convert_info_t;
/* words: bank format (I = bits 0-9, Q = bits 10-19, two's complement; gain = bits 20-27).
 * out may alias words (in place on the device); each element is read before it is written. */
void elrs_convert(const uint32_t *words, elrs_iq16_t *out, size_t n, elrs_convert_info_t *info);
void elrs_fft64(int16_t data[2 * ELRS_FFT_N]);   /* in-place radix-2 sc16, scaled 1/2 per stage, like dsps_fft2r_sc16 */
typedef struct { uint8_t channel; uint16_t frame_start, frame_end; float cfo_hz; float power_db, snr_db; } elrs_burst_t;
typedef struct { unsigned bursts, overlap_dropped; } elrs_detect_stats_t;
size_t elrs_detect(const elrs_iq16_t *x, size_t n, unsigned lo_mhz, elrs_burst_t *out, size_t max, elrs_detect_stats_t *st);
```

Detector rules (§5.4 step 2): Hann-windowed 64-point FFT every 256 samples; per in-band channel sum the bins whose centres are within ±500 kHz of `f_ch − LO`; burst = ≥20 consecutive frames ≥8 dB above that channel's median power and ≥ the power of both neighbouring channels in those frames; `cfo_hz` = power-weighted bin centroid minus `f_ch − LO`. Overlap drop (§5.5, resolved): of two bursts overlapping ≥50% in time, drop the one >10 dB weaker regardless of frequency — the TX sends one packet at a time, and the fixture's ghosts do not sit at exactly −f.

Note on `ELRS_INBAND_KHZ`: §5.4 says ≤6 MHz; the real fixture decodes a packet at 2443.4 MHz with LO 2436 (+7.4 MHz, +7.65 after CFO), so the limit is 7.5 MHz (still inside ±8 MHz Nyquist). Record this in `docs/elrs-receiver.md` (Task 10).

- Produces (`tests/elrs_synth.h`, test-only):

```c
typedef struct { unsigned lo_mhz; uint8_t channel; double cfo_hz, dev_hz, snr_db /* in 1 MHz */, mirror_dbc, amplitude; long start_sample; unsigned truncate_tail_bits; bool second_packet; uint32_t seed; } synth_cfg_t;
/* Writes n bank-format words: preamble 0x55 x2, sync UID5,UID4, 112 air bits, inverted polarity (high f = 0), 300 kb/s, no shaping.
 * amplitude in 10-bit LSB; samples saturate at ±511. second_packet adds the DVDA twin 1000 µs later. */
void synth_snapshot(const synth_cfg_t *c, const uint8_t uid[6], const uint8_t pkt[8], uint32_t *words, size_t n);
void synth_noise(uint32_t seed, double sigma_lsb, uint32_t *words, size_t n);
```

- Produces (`tests/elrs_fixture.h`): `size_t fixture_load(const char *bin, unsigned samples, unsigned index, uint32_t *words)` — reads one int8 I/Q snapshot, multiplies by 4, packs into bank words.

- [ ] **Step 1: Write the failing test `tests/elrs_dsp_test.c`** (argv[1] = `web/elrs/test/real_dk500.bin`)

- `convert`: words with I = −512, +511, Q = −1 sign-extend correctly; mean removed (|mean| ≤ 1 after); `clipped` counts |I|≥511 or |Q|≥511; `gain` = bits 20–27 of the middle word; in-place (`out == (void*)words`) gives the same result as out-of-place.
- `fft64`: a tone at bin 5 puts ≥90% of power in bin 5; DC input → bin 0.
- `detect_synth`: for every channel in band at LO 2438 and CFO in {−50, +230, +300} kHz, SNR 20 dB → exactly 1 burst on the right channel, `|cfo_hz − cfo| < 60 kHz`, `frame_start` within ±2 frames of truth.
- `detect_noise`: 1000 noise-only snapshots → total bursts ≤ 5.
- `overlap_drop`: synth packet plus a −25 dBc mirror image (`mirror_dbc = -25`) → one burst, `overlap_dropped == 1`.
- `detect_fixture`: real snapshots 0..5 at LO 2436 → each snapshot has a burst on the channel the JS decoder reported: `{0:2436.4, 1:2438.4, 2:2432.4, 3:2441.4, 4:2443.4, 5:2431.4}` (channel index = MHz − 2400.4).

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m unittest discover -s tests -k test_dsp -v`
Expected: FAIL (missing `dsp.h`).

- [ ] **Step 3: Implement the front end in `dsp.c` and the two test headers**

Hann window and twiddles as `static const int16_t` Q15 tables generated at first call (no `malloc`). `elrs_fft64` is our own portable code so the host build needs no esp-dsp sources or Xtensa assembly (`dsps_fft2r_sc16_ae32`); Task 9's `BENCH` decides whether swapping in `dsps_fft2r_sc16_ae32` is worth it.

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m unittest discover -s tests -k test_dsp -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add main/elrs/dsp.c main/elrs/dsp.h tests/elrs_synth.h tests/elrs_fixture.h tests/elrs_dsp_test.c tests/test_elrs.py
git commit -m "elrs: int16 convert, 64-point FFT burst detector, overlap drop"
```

### Task 6: Per-burst demodulator and `elrs_decode_snapshot`

**Files:**
- Modify: `main/elrs/dsp.h`, `main/elrs/dsp.c` (demod half)
- Create: `tests/elrs_decode_test.c`
- Modify: `tests/test_elrs.py` (`test_decode`, `test_decode_noise`)

**Interfaces:**
- Consumes: Task 3 (`elrs_air_decode`, `elrs_crc_slot`, `elrs_crc_init`), Task 5 (`elrs_detect`, `elrs_iq16_t`, `elrs_burst_t`).
- Produces:

```c
typedef struct { unsigned lo_mhz; uint8_t uid[6]; elrs_polarity_t polarity; } elrs_decode_cfg_t;  /* elrs_polarity_t from cmd.h */
typedef struct {
    uint8_t pkt[8]; uint8_t channel; int8_t crc_slot;          /* 0 Hybrid/non-RC, 1|2 Wide RC */
    uint32_t sync_end_sample;                                   /* 16 MS/s index of the last sync bit's end */
    float cfo_hz, snr_db; uint8_t fec, sync_errors, missing_bits; bool inverted;
} elrs_packet_t;
typedef struct { unsigned bursts, overlap_dropped, sync_found, fec_reject, crc_fail, wide; } elrs_decode_stats_t;
typedef struct elrs_dsp_work elrs_dsp_work_t;                   /* opaque, ~40 KB, caller provides static storage */
size_t elrs_dsp_work_size(void);
size_t elrs_decode_snapshot(const elrs_iq16_t *x, size_t n, const elrs_decode_cfg_t *cfg, elrs_dsp_work_t *work,
                            elrs_packet_t *out, size_t max_out, elrs_decode_stats_t *st);
```

Demod (§5.4 steps 3–6): NCO mix at `f_ch − LO + cfo` over `[frame_start·256 − 1040, frame_end·256 + 1040]` input samples clipped to the snapshot (1040 = the JS decoder's 260-sample margin at 4 MS/s, 65 µs); boxcar-4 → ≤15-tap low-pass (cutoff 400 kHz at 4 MS/s) → decimate 2 → 2 MS/s; discriminator `I[n-1]Q[n] − Q[n-1]I[n]` normalised by power; `twoMeansCenter` (5 iterations from p10/p90 midpoint) refines and removes CFO; 8-phase integrate-and-dump timing (max mean |soft|); bits = soft < 0 ? 1 : 0 for on-air (inverted) polarity; sync search for `uid[5],uid[4]` with ≤1 bit error, candidates sorted by (errors, position); 112 payload bits, missing tail ≤30 → erasure decode; reject >5 corrections; `elrs_crc_slot` ≥ 0 accepts. Wide slots 1–2 are reported (`crc_slot`) and counted in `st->wide`. `ELRS_POL_AUTO` additionally tries the non-inverted pattern and sets `inverted = false` on those (debug only).

- [ ] **Step 1: Write the failing tests `tests/elrs_decode_test.c`** (argv[1] = real fixture)

- `real_fixture`: snapshots 0..5, LO 2436, UID `{0,0,0,0,0x13,0x3e}`, `ELRS_POL_INV`: ≥5 RC packets; every packet `crc_slot == 0`, `inverted`, sticks via `elrs_unpack_4x10`/`elrs_uint10_to_crsf` = `[992, 996|997, 174, 992]`, `|cfo_hz − 240e3| < 20e3`.
- `ghosts_rejected`: with `ELRS_POL_AUTO` the total is exactly 2 more packets than with `ELRS_POL_INV`, and those 2 have `inverted == false` (snapshots 3 and 4).
- `wrong_uid_decodes_nothing`: UID of phrase `test` (`4f04fd822155`) → 0 packets on all 6 snapshots.
- `synth_matrix`: channel ∈ every in-band channel at LO 2438, CFO ∈ {−50, 0, 120, 230, 300} kHz, deviation 135 kHz, mirror −25 dBc, 20 trials each: at SNR 15 dB ≥95% decoded with the exact packet; at SNR 6 dB ≥20% (print the rate).
- `synth_truncated`: packet starting at sample −1500 (head cut) and packet whose last 30 air bits fall after the snapshot end both decode; 31+ missing bits do not decode and do not crash.
- `synth_dvda_pair`: `second_packet` gives two packets 1000 µs apart (`sync_end_sample` difference 16000 ± 16).
- `synth_wide`: packet finished with slot 2 → `crc_slot == 2`, `st.wide == 1`.
- `synth_clipped_still_decodes`: amplitude 2000 LSB (saturating at ±511), SNR 30 dB → ≥90% decode and `elrs_convert` reports `clipped > 0`.
- `arrival_time`: synth start at sample 3000 → `sync_end_sample` = 3000 + 32 bits × 16e6/300e3 ± 8.

`tests/test_elrs.py::test_decode_noise` runs the same binary with `--noise 10000`: 10⁴ noise-only snapshots (σ = 8 LSB, plus 1000 with a random-UID synth packet at 20 dB) → zero packets accepted. Mark it as the slow test; it must finish in < 120 s on the Mac.

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m unittest discover -s tests -k test_decode -v`
Expected: FAIL (undefined `elrs_decode_snapshot`).

- [ ] **Step 3: Implement the demod and `elrs_decode_snapshot`** — port from `web/elrs/demod.js` (`twoMeansCenter`, `recoverTiming`, sync candidate loop) with the front-end replacement above. Work buffers live in `struct elrs_dsp_work`; no heap.

- [ ] **Step 4: Run to verify pass, including the noise test**

Run: `python3 -m unittest discover -s tests -k ElrsHost -v`
Expected: PASS; printed 6 dB rate recorded in the commit message.

- [ ] **Step 5: Commit**

```bash
git add main/elrs/dsp.c main/elrs/dsp.h tests/elrs_decode_test.c tests/test_elrs.py
git commit -m "elrs: 2 MS/s per-burst demodulator and snapshot decoder"
```

### Task 7: Real 300-snapshot capture as a regression fixture

**Files:**
- Create: `tools/elrs/export_fixture.py`, `tools/elrs/golden_from_js.mjs`
- Create: `tests/fixtures/elrs/caps_rc.bin`, `tests/fixtures/elrs/caps_rc.json` (`git add -f` for the json)
- Create: `tests/elrs_caps_test.c`
- Modify: `tests/test_elrs.py` (`test_caps`, `test_caps_local`), `.gitignore` (`/tests/fixtures/elrs/local/`)

**Interfaces:**
- Consumes: `elrs_decode_snapshot` (Task 6), `fixture_load` (Task 5).
- Produces: `caps_rc.json` = `{"source":"caps.npz","samples":16380,"fs":16000000,"freqMhz":2436,"snapshots":N,"packets":[{"snap":i,"channelMhz":f,"hex":"…16 hex…"}]}` listing the JS decoder's inverted-polarity OTA3 packets.

Privacy rule (resolved): SYNC packets carry UID3, so no snapshot containing a decoded SYNC is committed. The full capture is exported only to the gitignored `tests/fixtures/elrs/local/caps_full.bin` for local runs.

- [ ] **Step 1: Write the exporter and golden script**

`export_fixture.py <npz> <out.bin> [--snapshots i,j,...]`: same conversion as `web/elrs/test/convert_fixture.py` (`rint(iq*128)` int8 interleaved). `golden_from_js.mjs <bin> <samples> <freqMhz>`: runs `web/elrs/demod.js decodeSnapshot` with UID45 and `PolarityFilter`, prints JSON lines `{snap, channelMhz, type, hex, polarity, ota}`.

- [ ] **Step 2: Select and export the subset**

Run: export all 300 to `tests/fixtures/elrs/local/caps_full.bin`; run the golden script on it; choose every snapshot with ≥1 RC packet and no SYNC, plus 8 snapshots with zero bursts, plus 8 with bursts but no decode; export those with `--snapshots`; regenerate golden on the subset into `caps_rc.json`.
Expected: `caps_rc.bin` ≤ 2 MB; `packets` length ≥ 40 (the Python reference decoded 48 on all 300; SYNC snapshots are excluded). `grep -c '"type":"SYNC"' caps_rc.json` → 0.

- [ ] **Step 3: Write the failing test `tests/elrs_caps_test.c`**

Decodes every snapshot of `caps_rc.bin` at LO 2436: ≥90% of golden packets are found with identical bytes and channel; every RC packet has sticks `[992, 996|997, 174, 992]`; snapshots in the "zero bursts" group yield nothing; print C/JS packet counts. `test_caps_local` runs the same binary on `local/caps_full.bin` when present (skipped otherwise) and asserts every decoded SYNC has `rate_index == 14` and `sw_mode == 1` — without printing any UID field.

- [ ] **Step 4: Run to verify failure, then pass** (fix decoder parameters in `dsp.c` only if <90%)

Run: `python3 -m unittest discover -s tests -k test_caps -v`
Expected: PASS with printed counts.

- [ ] **Step 5: Commit**

```bash
git add tools/elrs/export_fixture.py tools/elrs/golden_from_js.mjs tests/fixtures/elrs/caps_rc.bin tests/elrs_caps_test.c tests/test_elrs.py .gitignore
git add -f tests/fixtures/elrs/caps_rc.json
git commit -m "elrs: regression fixture from the 300-snapshot real capture (RC only)"
```

### Task 8: Line formatting

**Files:**
- Create: `main/elrs/frames.h`, `main/elrs/frames.c`, `tests/elrs_frames_test.c`
- Modify: `tests/test_elrs.py` (`test_frames`)

**Interfaces:**
- Consumes: `elrs_packet_t` (Task 6), `elrs_sync_t`/`elrs_parse_sync`/`elrs_sync_uid_ok` (Task 3), `elrs_mode_name` (Task 2).
- Produces:

```c
typedef struct {
    uint32_t up_s; elrs_mode_t mode; const char *state; unsigned snaps, bursts, ok, rc, sync, crcfail, mirror;
    unsigned dec_us, cap_us, ret_us; int lock_err_us; int cfo_khz; unsigned gain, drop;
} elrs_stats_t;
/* nonce < 0 prints '-'. channel is the 0..79 index. Returns length, or -1 if cap is too small. */
int elrs_fmt_packet(char *buf, size_t cap, uint32_t t_us, const elrs_packet_t *p, int nonce, const uint8_t uid[6]);
int elrs_fmt_stats(char *buf, size_t cap, const elrs_stats_t *s);
int elrs_fmt_lock(char *buf, size_t cap, const char *state, const char *reason);
int elrs_fmt_warn(char *buf, size_t cap, const char *text);
```

Formats (§8): `P <t_us> <ch> <R|S|M> <nonce|-> <cfo_khz %d> <snr_db %.1f> <fec> <data>\n`; type R for RC, S for SYNC, M for MSP and TLM; data = 16 lowercase hex for R and M; for S `fhss,nonce,rate,tlm,swmode,mm` with `mm` = model id from `elrs_sync_uid_ok` (or `-` if UID check fails). `S up=… mode=… state=… snaps=… bursts=… ok=… rc=… sync=… crcfail=… mirror=… dec_us=… cap_us=… ret_us=… lock_err_us=… cfo_khz=… gain=… drop=…\n` in exactly that key order. `L <state> <reason>\n`, `W <text>\n`.

- [ ] **Step 1: Write the failing test**

```c
elrs_packet_t p = { .pkt = {0xf8,1,2,3,4,5,6,0x95}, .channel = 36, .cfo_hz = 240400, .snr_db = 18.26f, .fec = 1 };
char b[128];
elrs_fmt_packet(b, sizeof b, 123456, &p, -1, uid45);
assert(!strcmp(b, "P 123456 36 R - 240 18.3 1 f801020304050695\n"));
```

Plus: a SYNC packet built with `elrs_finish_packet` (fhss 160, nonce 77, byte3 0xE3, uid3 0xAA, uid4 0x13, uid5x = 0x3e^(~5&0x3f)), channel 41, `t_us` 5000, cfo 240 kHz, snr 18.26, fec 0, nonce argument 77, UID `{0,0,0,0xAA,0x13,0x3e}` → exactly `"P 5000 41 S 77 240 18.3 0 160,77,14,1,1,5\n"` (no UID byte appears in any form); `elrs_fmt_stats` exact string for a filled struct; `cap` too small returns −1 and leaves `b` NUL-terminated.

- [ ] **Step 2: Run to verify failure, implement, run to verify pass**

Run: `python3 -m unittest discover -s tests -k test_frames -v`
Expected: FAIL then PASS.

- [ ] **Step 3: Commit**

```bash
git add main/elrs/frames.c main/elrs/frames.h tests/elrs_frames_test.c tests/test_elrs.py
git commit -m "elrs: P/S/L/W line formatting"
```

---

## M2 — Step 1: on-chip decode with a parked LO (ships)

### Task 9: Radio primitives, `SNAP16` and `BENCH`

**Files:**
- Create: `main/targets/esp32/elrs/radio.h`, `main/targets/esp32/elrs/radio.c`
- Modify: `main/targets/esp32/elrs/app.c`

**Interfaces:**
- Consumes: `elrs_convert`, `elrs_detect`, `elrs_decode_snapshot` (Tasks 5–6), `elrs_cmd_t` (Task 2).
- Produces (`radio.h`):

```c
typedef struct { uint32_t *words; unsigned n; int64_t t_trig_us; unsigned arm_us, capture_us, finish_us; const char *err; } radio_capture_t;
void radio_init(void);                        /* after Wi-Fi start: prepare_rx at the park LO, read gain_max, hardware AGC on */
void radio_park(unsigned mhz);                /* full tune_rx + prepare_rx */
unsigned radio_park_mhz(void);
void radio_set_gain(bool hardware, unsigned code);
void radio_gain_reply(char *buf, size_t cap); /* "GAIN HARDWARE|MANUAL <i> 0 <max> <forced>", as stock */
unsigned radio_gain_max(void);
bool radio_arm(unsigned n);                   /* stale check + prepare, sentinel fill, filter_apply, bank owner, lanes; 16 MS/s (clock 0) */
bool radio_trigger(int64_t at_us);            /* at_us <= 0: now; else sleep to at_us-1200 then busy-wait on CCOUNT; false if already late */
bool radio_finish(radio_capture_t *cap);      /* wait BIT(18) ≤20 ms, restore owner/lanes/filter; check DUMP_STATUS count + first unwritten word */
bool radio_finish_full_check(radio_capture_t *cap); /* stock full sentinel scan, for BENCH comparison */
```

`radio.c` starts with a comment naming the source: copied from `main/targets/esp32/receiver.c` at commit `e342444` (`prepare_rx`, `apply_gain`, `tune_rx`, `filter_apply/restore`, `acquire_iq` split into arm/trigger/finish). Same register constants, same `SOC_RESERVE_MEMORY_REGION(0x3ffe8000, 0x3fff8000, …)`. If `rx_recalibration_stale()` trips in `radio_arm`, re-tune and set `cap->err = "stale"` so the caller emits `W stale`.

- [ ] **Step 1: Implement `radio.c` and wire `GAIN*`, `PARK`, `PARK?`, `SNAP16`, `BENCH` in `app.c`**

`SNAP16 <n>` (requires RX OFF): arm/trigger/finish at the park LO, then the exact CAP16 frame — `DATA <n> <crc32 %08x> <capture_us>\n` + 2n bytes of IQ8 (upper 8 bits of I then Q) with `esp_rom_crc32_le(0, …)`, as `burst_capture.h` `burst_send_capture` does for format 16. `BENCH <n>` (requires RX OFF, else `ERR rx_on`): n live captures, each timed with CCOUNT for `cap_us` (arm→finish), `arm_us`, `read_us` (convert), `det_us`, `demod_us`, `total_us`; reply `BENCH n=<n> cap_us=<p50>/<p99> arm_us=… read_us=… det_us=… demod_us=… total_us=… hits=<packets>` (p50/p99 from a sorted array). Add `BENCH SNAP16` to `CAPS`.

- [ ] **Step 2: Build**

Run: the Global Constraints build command for `esp32-elrs`, then for `esp32`, each into a fresh `$SCRATCH/art-<n>`
Expected: build succeeds; stock `esp32` profile still builds (run it too).

- [ ] **Step 3: [HIL] SNAP16 parity with stock CAP16**

Flash; with the TX on (ask the user once **[USER]**: "power the TX on DK500 at ~1 m"), run a host script that sends `PARK 2438`, then 50× `SNAP16 16380`, saves them as an int8 fixture (`tests/fixtures/elrs/local/snap16_2438.bin`), and runs `tests/elrs_caps_test.c`-style decode on the host build plus `golden_from_js.mjs`.
Expected: CRC32 matches on every frame; host C decode and JS decode agree on every packet.

- [ ] **Step 4: [HIL] BENCH and the K1 decision**

Run: `BENCH 500` at `PARK 2438` with the TX on.
Expected: one `BENCH` line. Record p50/p99 in the commit message. Decision: p50 `total_us` ≤ 6000 → continue; 6000–15000 → continue and open the optimisation items (swap `elrs_fft64` for `dsps_fft2r_sc16_ae32` after checking the submodule builds, int16 NCO, fewer timing phases), re-run; > 15000 after optimisation → **K1**: stop and run the contingency "F1" below before Task 10. Also compare `radio_finish` vs `radio_finish_full_check` time and confirm the reduced check never passes a capture the full check fails (500 runs).

- [ ] **Step 5: Commit**

```bash
git add main/targets/esp32/elrs/radio.c main/targets/esp32/elrs/radio.h main/targets/esp32/elrs/app.c
git commit -m "esp32-elrs: copied RX primitives with split arm/trigger/finish, SNAP16, BENCH"
```

**Contingency F1 (only if K1 trips):** add `MODE RAW` that runs convert + detect + mix/decimate on the chip and sends `B <t_us> <ch> <cfo_khz> <n>` followed by n int8 I/Q pairs at 2 MS/s (~2.2 KB), and a JS 2 MS/s demod in `web/elrs-rx/` reusing `demod.js` functions. Plan it as its own task with the orchestrator before starting.

### Task 10: SCAN streaming, UID/NVS, output ring

**Files:**
- Create: `main/targets/esp32/elrs/out.h`, `main/targets/esp32/elrs/out.c`
- Modify: `main/targets/esp32/elrs/app.c`
- Create: `docs/elrs-receiver.md`

**Interfaces:**
- Consumes: Tasks 2, 5, 6, 8, 9.
- Produces (`out.h`):

```c
void out_init(void);                          /* 8 KB no-split ring + TX task (prio 3) draining via burst_serial_send */
bool out_stream(const char *line, size_t n);  /* streamed P/S/L/W: never blocks; false + drop++ when full */
void out_reply(const char *line, size_t n);   /* command reply: blocks until queued, keeps order after streamed lines */
void out_direct_begin(void);                  /* wait until ring drained, take the TX mutex (VERSION?, SNAP16 binary) */
void out_direct_end(void);
unsigned out_drops(void);
```

Task layout (§5.2, resolved): command task = `burst_serial_serve` in `app_main` raised to priority 5 (it sleeps 1 tick when idle); radio task priority 4 on the single core, loops arm → trigger(now) → finish → convert → decode → emit and calls `vTaskDelay(1)` at least every 100 ms; TX drain task priority 3. `RX OFF` sets a flag, waits for the radio task to acknowledge the end of its current loop, then `out_reply("OK\n")` — so the `OK` follows the last streamed line.

Command behaviour added here (§8): `UID <hex>` → `OK` (RAM; resets stats/lock; never echoed); `UID?` → `UID NONE|RAM|NVS`; `UID SAVE` → NVS `elrsrx`/`uid` blob → `OK` (`ERR no_uid` if none); `UID CLEAR` → wipe RAM and NVS → `OK`; boot loads the NVS UID if present. `MODE AUTO|SCAN` → `OK`; `MODE TIMED` → `ERR timed_unavailable` until Task 17; `MODE HOP` → `ERR hop_unqualified`; `MODE?` → `MODE <requested> <active>`. `PARK` while RX ON retunes between captures. `RX ON` → `ERR no_uid` without a UID (resolved: avoids a silent burst-only mode the page never uses), else `OK`; `RX ON` twice → `OK`. `STATS?` → one `S` line (the only reply that starts with a stream token; the page ignores the distinction). `BENCH`/`SNAP16` while RX ON → `ERR rx_on`. `POLARITY INV|AUTO` → `OK`.

Streaming rules: emit `P` for every accepted packet with `crc_slot == 0`, `t_us = (uint32_t)(t_trig_us + sync_end_sample/16)`, nonce `-`; Wide packets (`crc_slot` 1–2) or SYNC with `sw_mode == 0` → no `P`, `W wide_mode` at most once per 5 s; SYNC whose `rate_index != 14` → `W rate <n>` at most once per 5 s; SYNC failing `elrs_sync_uid_ok` → count as `crcfail`, no `P`; `clipped > 50` samples in a capture → `W clip` at most once per s; stale recalibration → `W stale`. One `S` line per second with `state=ACQ` and `mode=SCAN` (or `AUTO` requested / `SCAN` active — print the active mode); `cfo_khz` = EMA (α 0.2, reject jumps > 50 kHz) of accepted packets' CFO (§5.5).

`CAPS` becomes `CAPS VERSION ELRSRX DK500 OTA3 UIDNVS BENCH SNAP16` (resolved: `FHSS TIMED` are added in Task 17, `RETUNE` in Task 18, `HOP` only when qualified in Task 19).

- [ ] **Step 1: Implement `out.c` and the SCAN loop in `app.c`**

- [ ] **Step 2: Build both profiles**

Expected: both build; `git diff --stat fe83eda -- main/common main/targets/esp32/receiver.c` is empty.

- [ ] **Step 3: [HIL] Protocol checks (no TX needed)**

Host script at 2 Mbaud: `UID?` → `UID NONE` on an erased NVS; `RX ON` → `ERR no_uid`; `UID 00000000133e` → `OK`; `UID?` → `UID RAM`; `UID SAVE` → `OK`; reset the board (toggle DTR/RTS); `UID?` → `UID NVS`; `RX ON` → `OK` and `S` lines arrive once per second; `BENCH 10` → `ERR rx_on`; `SYNC 99` while streaming → `SYNC 99` within 50 ms among streamed lines; `RX OFF` → no `P`/`S` line after its `OK`; `UID CLEAR` → `OK`, `UID?` → `UID NONE`. Capture the serial log and grep it for the UID hex (`133e`): Expected 0 hits outside the command we sent.

- [ ] **Step 4: [HIL] [USER] SCAN rate**

Ask the user: "TX on, DK500, ~1 m from the board, sticks centred", and ask them to export their UID as `ELRS_UID` in the shell running the host script (or to run `UID SAVE` from a script they launch). The script sends `PARK 2438`, `UID $ELRS_UID` (never printed or written to disk), `RX ON` for 10 minutes and logs `P`/`S` lines to a file outside the repo.
Expected: RC updates/s ≥ 5 averaged over 10 min (target 10–25); `crcfail` small relative to `ok`; no `drop`. Compare with the old Python live rate (3.3 snapshots/s, 3–16% hit rate). Record numbers.

- [ ] **Step 5: Write `docs/elrs-receiver.md`**

Sections: purpose and "viewer only, not flight-safe, never transmits"; flashing; full command/line protocol table (§8 with the resolutions above); `ELRS_INBAND_KHZ` note; overlap-drop note; measurement log (BENCH, SCAN rate) — Task 15/17/19 append to it.

- [ ] **Step 6: Commit**

```bash
git add main/targets/esp32/elrs/out.c main/targets/esp32/elrs/out.h main/targets/esp32/elrs/app.c docs/elrs-receiver.md
git commit -m "esp32-elrs: SCAN mode streaming P/S/W lines, UID in RAM/NVS"
```

### Task 11: Page core modules — `ota3.js`, `link.js`

**Files:**
- Create: `web/elrs-rx/ota3.js`, `web/elrs-rx/link.js`
- Create: `web/elrs-rx/test/ota3.test.js`, `web/elrs-rx/test/link.test.js`
- Modify: `.github/workflows/pages.yml` (test step: `node --test web/elrs/test/*.test.js web/elrs-rx/test/*.test.js`)

**Interfaces:**
- Consumes: `../elrs/dk500.js` (`crc14`, `crcInit`, `unpack4x10`, `uint10ToCrsf`, `toHex`), `../elrs/serial.js` (`SdrLink`), read-only.
- Produces (`ota3.js`):

```js
export function parseLine(line)        // -> {kind:'P'|'S'|'L'|'W', ...} | {kind:'reply', text} | null (empty/garbage)
// P: {kind:'P', tUs, ch, type:'R'|'S'|'M', nonce|null, cfoKhz, snrDb, fec, data}
// S: {kind:'S', stats:{up, mode, state, snaps, bursts, ok, rc, sync, crcfail, mirror, dec_us, cap_us, ret_us, lock_err_us, cfo_khz, gain, drop}}
// L: {kind:'L', state, reason}   W: {kind:'W', text}
export function checkRc(hex, uid)       // -> {sticksCrsf:[r,p,t,y], armed, switches7} | null if not 16 hex or CRC14 (slot 0, OTA3 init) fails
export function parseSyncData(data)     // "fhss,nonce,rate,tlm,swmode,mm" -> {fhss, nonce, rate, tlm, swMode, mm} | null
export class Hybrid8State {             // aux[1..8] CRSF values, updated per RC packet; aux[1] from the arm bit (191/1792)
  update(rc) ; get aux() ; lastUpdate(auxIndex) }
```

- Produces (`link.js`):

```js
export function firmwareKind(infoReply) // 'elrs' if first token 'ESP32ELRS', 'stock' if 'ESP32SDR', else 'unknown'
export class ElrsLink {                 // wraps an SdrLink-like object {readLine, write, synchronize, close}
  constructor(sdr) ; onStream(fn)       // fn(parsedLine) for P/S/L/W
  async command(text, timeoutMs=3000)   // writes, returns the first reply line; P/L/W lines meanwhile go to onStream
  async pump()                          // reads lines forever while running, dispatching stream lines
  async stop()                          // sends RX OFF, waits for OK, sends RELEASE
}
```

- [ ] **Step 1: Write the failing tests**

```js
// ota3.test.js
const uid45 = Uint8Array.of(0, 0, 0, 0, 0x13, 0x3e);
test('P line parses', () => {
  assert.deepEqual(parseLine('P 123456 36 R - 240 18.3 1 f801020304050695'),
    { kind: 'P', tUs: 123456, ch: 36, type: 'R', nonce: null, cfoKhz: 240, snrDb: 18.3, fec: 1, data: 'f801020304050695' });
});
test('corrupted hex is rejected', () => {
  assert.ok(checkRc('f801020304050695', uid45));
  assert.equal(checkRc('f801020304050694', uid45), null);
  assert.equal(checkRc('f80102030405069', uid45), null);
  assert.equal(checkRc('zz01020304050695', uid45), null);
});
test('SYNC data uses the 3.x layout', () => {
  assert.deepEqual(parseSyncData('160,77,14,1,1,5'), { fhss: 160, nonce: 77, rate: 14, tlm: 1, swMode: 1, mm: 5 });
});
test('Hybrid8 covers AUX2..AUX8 including 16-position AUX8', () => { /* switches7 0x37 -> aux[8]=938, 0x38 -> 1044, (2<<3)|5 -> aux[4]=1792, arm bit -> aux[1]=1792 */ });
test('reply lines are not stream lines', () => {
  assert.equal(parseLine('SYNC 42').kind, 'reply');
  assert.equal(parseLine('S up=1 mode=SCAN state=ACQ snaps=3 bursts=1 ok=1 rc=1 sync=0 crcfail=0 mirror=0 dec_us=2000 cap_us=1400 ret_us=0 lock_err_us=0 cfo_khz=240 gain=40 drop=0').kind, 'S');
});
```

`link.test.js` with a fake SdrLink fed from an array of lines:
- `firmwareKind`: `'ESP32ELRS 1 elrs-rx 0'` → `elrs`; `'ESP32SDR 6 burst 16380'` → `stock`; `'ERR command'` → `unknown`.
- "command reply is found among streamed lines": feed `['P …', 'S …', 'OK']` → `command('UID …')` resolves `'OK'` and `onStream` saw P and S in order.
- "malformed lines are ignored": `['\x00\x7fgarb', 'P 1 2', 'P x y z w v u t s']` → no stream events, no throw.
- `stop()` writes `RX OFF\n` then `RELEASE\n`.

- [ ] **Step 2: Run to verify failure**

Run: `node --test web/elrs-rx/test/*.test.js`
Expected: FAIL (modules missing).

- [ ] **Step 3: Implement `ota3.js` and `link.js`** (Hybrid8 mapping identical to `dk500.js parsePacket`; CRC check builds byte 0 as `type` only, init `crcInit(uid, 3)`).

- [ ] **Step 4: Run to verify pass**

Run: `node --test web/elrs/test/*.test.js web/elrs-rx/test/*.test.js`
Expected: all PASS (existing tests unaffected).

- [ ] **Step 5: Commit**

```bash
git add web/elrs-rx/ota3.js web/elrs-rx/link.js web/elrs-rx/test/ota3.test.js web/elrs-rx/test/link.test.js .github/workflows/pages.yml
git commit -m "elrs-rx page: line router, CRC re-check, Hybrid8 state"
```

### Task 12: Page UI (layout C)

**Files:**
- Create: `web/elrs-rx/state.js`, `web/elrs-rx/test/app-state.test.js`
- Create: `web/elrs-rx/index.html`, `web/elrs-rx/app.js`, `web/elrs-rx/elrs-rx.css`

**Interfaces:**
- Consumes: Task 11 modules, `../elrs/dk500.js uidFromPhrase/toHex`, `../elrs/elrs.css`, `../style.css`.
- Produces (`state.js`, pure, no DOM):

```js
export class RxState {
  constructor(now = () => performance.now())
  onLine(parsed)                 // P/S/L/W from ElrsLink
  get updatesPerSec()            // RC P lines in the last 10 s / 10
  get hitRate()                  // last S ok/snaps, or null
  get badge()                    // 'SCAN' | 'LOCKED·TIMED' | 'LOCKED·HOP' | 'COAST'
  get lastPacketAgeMs()          // null before the first packet
  get stale()                    // true when no RC packet for 2000 ms (grey out)
  get hint()                     // null | 'wrong_phrase' | 'wide_mode' | 'clip' | 'no_signal'
  get sticks()                   // {roll,pitch,throttle,yaw} CRSF or null
  get aux()                      // Hybrid8State.aux
}
```

Badge: from the last `L` line (`LOCK` with S `mode=TIMED` → `LOCKED·TIMED`, `mode=HOP` → `LOCKED·HOP`; `COAST` → `COAST`; otherwise `SCAN`). Hints: `wrong_phrase` when the last 5 S lines sum `bursts ≥ 20` and `ok == 0`; `wide_mode` after `W wide_mode` (sticky until reconnect); `clip` for 5 s after `W clip`; `no_signal` when 5 S lines show `bursts == 0`.

- [ ] **Step 1: Write failing tests `app-state.test.js`**

Cover: updates/s with a fake clock (25 RC lines over 10 s → 2.5); `hint for bursts without packets` (5 S lines with `bursts=10 ok=0` → `wrong_phrase`); `W wide_mode` → `wide_mode`; stale after 2001 ms; badge transitions `L LOCK sync` + `S mode=TIMED` → `LOCKED·TIMED`, `L COAST miss` → `COAST`, `L LOST timeout` → `SCAN`; gimbal mapping Mode 2: `sticks` maps OTA index 3 → yaw, 2 → throttle, 0 → roll, 1 → pitch; `t_us` wrap (`4294967000` then `200`) does not produce a negative age.

- [ ] **Step 2: Run to verify failure, implement `state.js`, run to verify pass**

Run: `node --test web/elrs-rx/test/*.test.js`
Expected: FAIL then PASS.

- [ ] **Step 3: Build the page**

`index.html` header links like the other pages (`../`, `../elrs/`, `../elrs-rx/`, `../flash/`). Top row: password input for the phrase + **Set**; checkboxes "Remember phrase in this browser" (localStorage, try/catch) and "Store UID on the device" (`UID SAVE` when ticked at connect, `UID CLEAR` when unticked by the user while connected); **Connect**; status text `updates/s · hit rate · channel · last packet age` and the badge; "UID set" indicator only. Gimbals left (yaw X, throttle Y) and right (roll X, pitch Y), range 172–1811, grey when `stale`. Eight AUX bars (AUX1 arm bit, AUX2–AUX8 Hybrid8). Hint banner texts: wrong_phrase "Bursts seen but nothing decodes — check the binding phrase"; wide_mode "TX uses Wide switch mode; set Hybrid"; clip "Signal clipping — move the board further away"; no_signal "No ELRS bursts seen — is the TX on?". Footer: "Viewer only. Not flight-safe. The receiver never transmits."

Connect flow (§10.1): open 2 Mbaud via `SdrLink.open`; `synchronize()`; `INFO` → `firmwareKind`: `stock` → message "This board runs stock ESP-SDR. Use the raw-I/Q viewer (../elrs/) or flash the ELRS receiver firmware (../flash/)" and close; `unknown` → "Unknown firmware" and close; `elrs` → `UID <hex>`, optional `UID SAVE`, `MODE AUTO`, `RX ON`, `pump()`. Disconnect/`pagehide` → `stop()` then `close()`. Render with `requestAnimationFrame`, not per line.

- [ ] **Step 4: [HIL] Browser check**

Serve `web/` with `python3 -m http.server 8000`, open `http://localhost:8000/elrs-rx/` in Chrome, connect to the `esp32-elrs` board with the TX on **[USER]** (the user types the phrase).
Expected: gimbals move with sticks; badge `SCAN`; updates/s ≥ 5; entering a wrong phrase shows the wrong-phrase hint within ~5 s; reloading the tab mid-stream and reconnecting works (Review Focus 2).

- [ ] **Step 5: Commit**

```bash
git add web/elrs-rx/state.js web/elrs-rx/test/app-state.test.js web/elrs-rx/index.html web/elrs-rx/app.js web/elrs-rx/elrs-rx.css
git commit -m "elrs-rx page: layout C UI for the on-chip receiver"
```

### Task 13: Existing pages — `INFO` check, landing cards, README

**Files:**
- Modify: `web/elrs/app.js` (inside `connect()`, right after `await link.synchronize();`)
- Modify: `web/index.html`, `web/README.md`

**Interfaces:**
- Consumes: nothing new (inline check; `web/elrs/` must not import from `web/elrs-rx/`).

- [ ] **Step 1: Add the `INFO` check (≈10 lines)**

After `synchronize()`: `const info = await link.command('INFO');` if it starts with `ESP32ELRS`, `throw` an error whose message is "This board runs the ELRS receiver firmware; use the ELRS receiver page (../elrs-rx/)" — the existing catch shows it via `setStatus(..., 'err')` and closes the link. Any other reply continues exactly as before.

- [ ] **Step 2: Landing page and README**

`web/index.html`: three cards — "ELRS stick viewer (raw I/Q)" with "needs **stock ESP-SDR** firmware" → `elrs/`; "ELRS receiver (on-chip decode, fast)" with "needs **ELRS receiver** firmware" → `elrs-rx/`; "Flasher (both)" → `flash/`. Header nav gains `elrs-rx/`. `web/README.md`: describe all three pages and which firmware each needs.

- [ ] **Step 3: Run web tests**

Run: `node --test web/elrs/test/*.test.js web/elrs-rx/test/*.test.js`
Expected: PASS.

- [ ] **Step 4: [HIL] §9.4 cross-checks**

With `esp32-elrs` flashed: `web/elrs/` shows the redirect message and closes. Flash stock: `web/elrs/` decodes as before, `INFO` = `ESP32SDR 6 burst 16380`; `web/elrs-rx/` shows the stock message. Re-flash `esp32-elrs`.

- [ ] **Step 5: Commit**

```bash
git add web/elrs/app.js web/index.html web/README.md
git commit -m "web: redirect between raw-IQ viewer and ELRS receiver by INFO"
```

## Task 13b — SDR waterfall on both pages (amendment, inserted after Task 13)

User request (2026-10-23): both pages show an SDR waterfall next to the AUX bars. `web/elrs/` (stock firmware, raw I/Q) computes the spectrum in the browser; `web/elrs-rx/` gets a new streamed `F` line from the detector's existing 64-bin spectrogram, switched by `SPECTRUM ON|OFF`. One shared renderer.

Decisions (resolved while planning):

- **Spectrum = peak hold.** Per bin, the maximum `|X|²` over all frames of a snapshot (and, on the firmware, over all snapshots of a 100 ms window). ELRS bursts fill part of a 1 ms snapshot; a mean would wash them out.
- **Quantisation** (identical in C and JS): `q(p) = clamp(round(20·log10(max(p, 1))), 0, 255)`, i.e. 0.5 dB per step, 0 = power ≤ 1. Firmware bin power stays below 2^27, so `q` ≤ 163 in practice.
- **Bin order is DC-centred:** `out[j]` = FFT bin `(j + 32) mod 64`, centre frequency `LO + (j − 32) · 250 kHz`. So `out[0]` = LO − 8 MHz, `out[32]` = LO, `out[63]` = LO + 7.75 MHz.
- **`F` line:** `F <t_us> <lo_mhz> <128 lowercase hex>\n`. The 128 hex characters are `out[0..63]`, two per bin. The line is 141 bytes for a 4-digit `t_us`. `t_us` = `(uint32_t)cap.t_trig_us` of the first snapshot in the window. `lo_mhz` is in the line so the page needs no `PARK?`, and later TIMED/HOP modes stay unambiguous.
- **Rate limit:** the radio loop max-merges each snapshot's `elrs_spectrum` into a 64-byte accumulator. The window is timed on capture trigger timestamps, not wall clock: its `t_us` is the trigger time of its first snapshot, and when a snapshot's `(uint32_t)(t_trig_us − window_t_us) ≥ 100000` it emits one `F` line via `out_stream` and clears the accumulator (the next snapshot opens a new window). Wall-clock timing (`esp_timer_get_time()` at emit) was tried in 13b-1 and gave `t_us` gaps as small as 9 ms, because decode latency varies 4.5–11 ms; the trigger-time rule guarantees ≥ 100 ms between successive `t_us`. That gives ≤ 10 lines/s (measured 8.4 at 86 snapshots/s), roughly 1.4 kB/s, under 1% of the UART. If the LO changes while it accumulates, the accumulator restarts at the new LO (no mixed-LO rows). `SPECTRUM OFF`, `RX ON` and `RX OFF` also clear it. A dropped `F` line counts in `drop` like any streamed line.
- **`SPECTRUM ON|OFF`:** RAM only, default `OFF` at boot, allowed in any RX state, reply `OK`. The flag survives `RX OFF`/`UID CLEAR`. The page sends `SPECTRUM ON` on every connect, and the flag does nothing while RX is off. `CAPS` gains `SPECTRUM`.
- **Shared module lives in `web/elrs/waterfall.js`.** `web/elrs-rx/` already imports `../elrs/dk500.js` and `../elrs/serial.js`, so elrs-rx → elrs is the established direction, and the "`web/elrs/` must not import `web/elrs-rx/`" rule holds. A new `web/common/` would need both pages' import paths plus the CI test glob changed, for no gain. Its tests go in `web/elrs/test/waterfall.test.js`, which the existing glob already picks up. The module touches no DOM at import time, because `web/elrs/worker.js` imports it.
- **Display auto-levels.** The two pages have different absolute scales (int8 float DFT vs int16 Q15 FFT with 1/64 scaling). The colour range is `[floor, floor + 60]` q-units (30 dB), where `floor` is an EMA (α 0.1) of each row's 25th percentile. There's one fixed palette, with no pickers or zoom.
- **Span:** one capture covers ±8 MHz around the LO. That is about 16 channels at 1 MHz spacing, with about 14 decodable inside `ELRS_INBAND_KHZ`, at `PARK 2438` (elrs-rx) or `FREQ 2436` (elrs). The axis is labelled in MHz, with ticks at LO−8, LO−4, LO, LO+4. Packet marks are a magenta pixel at the decoded channel's column on the newest row.

Plan edits to make alongside: add these rows to File Structure — `web/elrs/waterfall.js` (shared spectrum and waterfall, 13b), `main/elrs/dsp.c/.h` (+ 13b), `main/elrs/cmd.c/.h` (+ 13b), `main/elrs/frames.c/.h` (+ 13b). In Global Constraints, change "streamed lines start with token `P`, `S`, `L` or `W`" to "`P`, `S`, `L`, `W` or `F`".

### Task 13b-1: Firmware spectrum export, `F` line, `SPECTRUM` command

**Files:**
- Modify: `main/elrs/dsp.h`, `main/elrs/dsp.c`, `main/elrs/cmd.h`, `main/elrs/cmd.c`, `main/elrs/frames.h`, `main/elrs/frames.c`
- Modify: `main/targets/esp32/elrs/app.c`
- Modify: `tests/elrs_dsp_test.c`, `tests/elrs_cmd_test.c`, `tests/elrs_frames_test.c`
- Modify: `docs/elrs-receiver.md`, `docs/superpowers/specs/2026-10-23-elrs-receiver-firmware-design.md` (append the Spec amendment below)

**Interfaces:**
- Consumes: `elrs_detect` / `elrs_decode_snapshot` (Tasks 5–6; signatures unchanged), `out_stream` (Task 10).
- Produces (`dsp.h`):

```c
/* clamp(round(20 * log10(max(power, 1))), 0, 255): 0.5 dB steps. */
uint8_t elrs_spectrum_q(uint32_t power);
/* Peak-hold spectrum of the most recent elrs_detect call (elrs_decode_snapshot calls it), DC-centred:
 * out[j] = elrs_spectrum_q(max over frames of bin (j + 32) % 64), centre LO + (j - 32) * 250 kHz.
 * Returns the frame count used; 0 with out zeroed before any detect or when it saw < 64 samples.
 * Not reentrant; call before the next elrs_detect. */
size_t elrs_spectrum(uint8_t out[ELRS_FFT_N]);
```

- Produces (`cmd.h`): `ELRS_CMD_SPECTRUM` appended to `elrs_cmd_kind_t`; `bool on;` added to the union.
- Produces (`frames.h`):

```c
/* "F <t_us> <lo_mhz> <128 lowercase hex>\n"; bins DC-centred as elrs_spectrum. -1 if cap is too small. */
int elrs_fmt_spectrum(char *buf, size_t cap, uint32_t t_us, unsigned lo_mhz, const uint8_t bins[ELRS_FFT_N]);
```

Implementation notes:
- Move `bins` out of `elrs_detect` to a file-scope `static uint32_t g_bins[MAX_FRAMES][ELRS_FFT_N]`, and record the frame count in a `static size_t g_bins_frames` after `spectrogram()`. The detector's behaviour and output stay byte-identical. Use `float` and `log10f` (never `double`) and no heap.
- `cmd.c`: `SPECTRUM ON` / `SPECTRUM OFF` (argc 1); anything else → `ERR args`. Add `"SPECTRUM"` to the overflow `args` list.
- `app.c`: `static volatile bool spectrum_on`. In `radio_task`, after `elrs_decode_snapshot` and only when `spectrum_on`: call `elrs_spectrum(tmp)`, max-merge it into `acc[64]` (restart if `cfg.lo_mhz != acc_lo`), and apply the rate rule above. `case ELRS_CMD_SPECTRUM`: set the flag, request an accumulator clear, `reply("OK\n")`. `CAPS` → `CAPS VERSION ELRSRX DK500 OTA3 UIDNVS BENCH SNAP16 SPECTRUM`. `BENCH` does not touch the accumulator.

- [ ] **Step 1: Write the failing tests**

`tests/elrs_dsp_test.c`, new cases (the existing ones stay unchanged as the regression guard):
- `spectrum_q`: `elrs_spectrum_q(0) == 0`, `(1) == 0`, `(3) == 10`, `(10) == 20`, `(100) == 40`, `(1000) == 60`, `(1u << 27) == 163`, `(UINT32_MAX) == 193`.
- `spectrum_before_detect`: run this case first in `main`. `memset(out, 0xAA, 64)`; `elrs_spectrum(out) == 0` and all 64 bytes are 0.
- `spectrum_tone_pos`: 16380 samples `x[k].i = lroundf(400 cosf(2π·8k/64))`, `.q = lroundf(400 sinf(...))` (+2 MHz) → `elrs_detect(x, 16380, 2438, …)`; `elrs_spectrum(out) == 64`; argmax(out) == 40; `out[40] >= out[j] + 40` for every `|j − 40| >= 3`.
- `spectrum_tone_neg`: the same at −3 MHz (bin −12) → argmax == 20.
- `spectrum_dc`: `i = 300, q = 0` constant → argmax == 32.
- `spectrum_zero`: all-zero input → returns 64, all 64 bytes are 0.
- `spectrum_short`: `elrs_detect` with n = 32 → `elrs_spectrum` returns 0 and out is zeroed.

`tests/elrs_cmd_test.c`:

```c
P("SPECTRUM ON");   assert(c.kind == ELRS_CMD_SPECTRUM && c.u.on);
P("SPECTRUM OFF");  assert(c.kind == ELRS_CMD_SPECTRUM && !c.u.on);
INVALID("SPECTRUM", "args");
INVALID("SPECTRUM X", "args");
INVALID("SPECTRUM ON 1", "args");
```

`tests/elrs_frames_test.c`:

```c
uint8_t bins[64];
for (int j = 0; j < 64; j++) bins[j] = (uint8_t)(j * 4);
char f[160];
assert(elrs_fmt_spectrum(f, sizeof f, 5000, 2438, bins) == 141);
assert(!strcmp(f, "F 5000 2438 "
    "0004080c1014181c2024282c3034383c4044484c5054585c6064686c7074787c"
    "8084888c9094989ca0a4a8acb0b4b8bcc0c4c8ccd0d4d8dce0e4e8ecf0f4f8fc\n"));
assert(elrs_fmt_spectrum(f, 141, 5000, 2438, bins) == -1 && memchr(f, 0, 141));
assert(elrs_fmt_spectrum(f, 142, 5000, 2438, bins) == 141);
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m unittest tests.test_elrs -v`
Expected: `test_dsp`, `test_cmd` and `test_frames` FAIL (undeclared `elrs_spectrum`, `ELRS_CMD_SPECTRUM`, `elrs_fmt_spectrum`).

- [ ] **Step 3: Implement dsp, cmd and frames**

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m unittest tests.test_elrs -v`
Expected: all PASS, including the unchanged `test_dsp` detector cases, `test_decode` and `test_caps` (detector output unchanged).

- [ ] **Step 5: Commit**

```bash
git add main/elrs/dsp.c main/elrs/dsp.h main/elrs/cmd.c main/elrs/cmd.h main/elrs/frames.c main/elrs/frames.h tests/elrs_dsp_test.c tests/elrs_cmd_test.c tests/elrs_frames_test.c
git commit -m "elrs: peak-hold spectrum export, F line formatter, SPECTRUM command parse"
```

- [ ] **Step 6: Wire `app.c`, then build both profiles**

Run: the Global Constraints build command for `esp32-elrs`, then for `esp32`, each into a fresh `$SCRATCH/art-<n>`.
Expected: both build; `git diff --stat fe83eda -- main/common main/targets/esp32/receiver.c` is empty.

- [ ] **Step 7: Docs and spec amendment**

`docs/elrs-receiver.md`:
- `CAPS` row → `… SNAP16 SPECTRUM`.
- New row: `SPECTRUM ON|OFF` → `OK`; RAM only, default `OFF`, allowed in any RX state.
- The intro sentence lists `P`, `S`, `L`, `W` or `F`.
- New Streamed-lines bullet with the exact `F` format, bin order, quantisation, rate rule and LO-change rule from the Decisions above.

Append the "Spec amendment" paragraph (end of this file) to the spec under §14 Decision record.

- [ ] **Step 8: [HIL] Protocol check (no TX needed)**

Use a host script at 2 Mbaud, with the test UID `00000000133e`:
1. `CAPS` ends with `SPECTRUM`.
2. `SPECTRUM X` → `ERR args`.
3. `UID 00000000133e`, `RX ON`: for 3 s there are no `F` lines.
4. `SPECTRUM ON` → `OK`, then over 10 s: 80–100 `F` lines. Each matches `^F \d+ 2438 [0-9a-f]{128}$` and successive `t_us` differ by ≥ 100000. `S` lines still arrive at 1/s with `drop=0`.
5. `PARK 2450`: within 200 ms, `F` lines carry `2450`, and no line carries 2438 after the first 2450.
6. `SPECTRUM OFF` → `OK`, then no `F` line after that `OK`.
7. `RX OFF`.

Expected: all as stated. Record the `dec_us` before and after `SPECTRUM ON` in the commit message (it should be within +5%).

- [ ] **Step 9: Commit**

```bash
git add main/targets/esp32/elrs/app.c docs/elrs-receiver.md docs/superpowers/specs/2026-10-23-elrs-receiver-firmware-design.md
git commit -m "esp32-elrs: stream rate-limited F spectrum lines under SPECTRUM ON"
```

### Task 13b-2: Shared waterfall module and `web/elrs-rx/` integration

**Files:**
- Create: `web/elrs/waterfall.js`, `web/elrs/test/waterfall.test.js`
- Modify: `web/elrs-rx/ota3.js`, `web/elrs-rx/test/ota3.test.js`, `web/elrs-rx/test/link.test.js`
- Modify: `web/elrs-rx/index.html`, `web/elrs-rx/app.js`, `web/elrs/elrs.css`

**Interfaces:**
- Consumes: the `F` line and `SPECTRUM ON` (13b-1); `ElrsLink.onStream` (Task 11; `F` routes there unchanged because `kind !== 'reply'`).
- Produces (`web/elrs/waterfall.js`, no DOM access at import time):

```js
export const WF_BINS = 64;                 // DC-centred; column j = LO + (j - 32) * 250 kHz
export function specQ(p)                   // clamp(round(20*log10(max(p,1))), 0, 255), same as elrs_spectrum_q
export function spectrumFromIq8(iq)        // Int8Array interleaved I,Q -> Uint8Array(64): frames every 256 samples,
                                           // 64-pt periodic Hann (0.5 - 0.5 cos(2πi/64)) float DFT, |X|² peak hold, specQ, DC-centred
export function binColumn(freqKhz, loMhz)  // Math.round((freqKhz - loMhz*1000) / 250) + 32, or null outside 0..63
export function axisTicks(loMhz)           // [{frac:(col+0.5)/64, text:String(lo+d)} for d of -8,-4,0,4]
export const WF_PALETTE                    // Uint8ClampedArray(256*3), linear stops 0:(0,0,0) .35:(0,0,170) .6:(0,200,200) .8:(255,220,0) 1:(255,255,255)
export const WF_MARK = [255, 0, 255];
export class LevelScale {                  // floor: first update = 25th pct (sorted[16]); then floor += 0.1*(pct - floor)
  update(bins) ; get floor() ; index(q) }  // index = clamp(round((q - floor) * 255 / 60), 0, 255)
export class WaterfallModel {
  constructor(rows = 120)
  push(bins, loMhz)                        // newest row first; LO change clears history first; > rows drops oldest; LevelScale.update
  mark(freqKhz)                            // marks binColumn(freqKhz, lo) on the newest row; ignored if null or no rows
  get loMhz()                              // null before the first push
  get dirty()                              // true after push/mark, false after pixels()
  pixels()                                 // Uint8ClampedArray(64*rows*4) RGBA, row 0 = newest, colour = WF_PALETTE[index(q)] using the
}                                          // scale at push time, marked pixels WF_MARK, rows not yet filled (0,0,0,255)
export function drawWaterfall(canvas, model, axisEl)  // if model.dirty: putImageData(pixels); rebuild axis labels when LO changed
```

- Produces (`ota3.js`): `parseLine` case `'F'` → `{kind:'F', tUs, loMhz, bins: Uint8Array(64)}`, or `null` unless there are exactly 4 tokens, `t_us` is an integer, `lo_mhz` is an integer in 2402..2478, and the data matches `/^[0-9a-f]{128}$/i`.

- [ ] **Step 1: Write the failing tests**

`web/elrs/test/waterfall.test.js`:
- `specQ`: 0→0, 1→0, 3→10, 10→20, 100→40, 1000→60, 2**27→163, 2**32−1→193.
- `spectrumFromIq8`:
  - A 16380-sample tone with amplitude 100 at +2 MHz (`iq[2k] = Math.round(100*cos(2π·8k/64))`, `iq[2k+1] = …sin…`): argmax 40, and `out[40] >= out[j] + 40` for `|j−40| >= 3`.
  - −3 MHz → argmax 20.
  - Constant I=50, Q=0 → argmax 32.
  - All-zero → all 0.
  - 32 samples → all 0.
- `binColumn`: `(2436400, 2438)` → 26; `(2438000, 2438)` → 32; `(2430000, 2438)` → 0; `(2445750, 2438)` → 63; `(2446000, 2438)` → null; `(2429800, 2438)` → null.
- `axisTicks(2438)` texts `['2430','2434','2438','2442']`, fracs `[0.5/64, 16.5/64, 32.5/64, 48.5/64]`.
- `WF_PALETTE`: entry 0 = 0,0,0; entry 255 = 255,255,255; luminance (0.299r+0.587g+0.114b) non-decreasing.
- `LevelScale`: update(all 40) → floor 40, `index(40)`=0, `index(70)`=128, `index(100)`=255, `index(10)`=0; then update(all 50) → floor 41.
- `WaterfallModel(3)`:
  - Push A (all 40), then B (all 100) at LO 2438: row 0 pixels = palette[255], row 1 = palette[0], and row 2 = (0,0,0,255).
  - `mark(2436400)` → pixel (col 26, row 0) = 255,0,255.
  - Push 2 more → 3 rows kept, and the oldest is gone.
  - Push at LO 2450 → only 1 row filled, `loMhz` 2450.
  - `mark` before any push does not throw.
  - `dirty` false after `pixels()`.

`web/elrs-rx/test/ota3.test.js`:
- The exact 13b-1 line `'F 5000 2438 0004080c…f8fc'` → `{kind:'F', tUs:5000, loMhz:2438}` with `bins[j] === j*4` for all j.
- These are rejected (each → null):
  - 126 hex chars.
  - `'F 5000 2438'`.
  - `'F x 2438 <128 hex>'`.
  - `'F 5000 2401 <128 hex>'`.
  - `'F 5000 2438 <127 hex>g'`.

`web/elrs-rx/test/link.test.js`: feed `['F 5000 2438 <128 hex>', 'P …', 'OK']` → `command(...)` resolves `'OK'`, and `onStream` saw F then P.

- [ ] **Step 2: Run to verify failure**

Run: `node --test web/elrs/test/*.test.js web/elrs-rx/test/*.test.js`
Expected: FAIL (`waterfall.js` missing; `F` parses as `reply`).

- [ ] **Step 3: Implement `waterfall.js` and the `F` parser**

- [ ] **Step 4: Run to verify pass**

Run: `node --test web/elrs/test/*.test.js web/elrs-rx/test/*.test.js`
Expected: all PASS, existing tests unaffected.

- [ ] **Step 5: Page integration (`web/elrs-rx/`)**

- `elrs.css`: add a modifier so `web/elrs/` keeps its layout until 13b-3:
  - `.grid.wf3 { grid-template-columns: minmax(0,2fr) minmax(0,1fr) minmax(0,1fr); }`
  - `@media (max-width: 960px) { .grid.wf3 { grid-template-columns: 1fr 1fr; } .grid.wf3 .gimbals { grid-column: 1 / -1; } }`
  - The existing ≤640 px rule still gives one column.
  - `.wf` styled like `.aux`.
  - `.wf canvas { display:block; width:100%; height:240px; image-rendering:pixelated; background:#000; }`
  - `.wfaxis { position:relative; height:1.2em; font-size:.75rem; color:var(--muted); }` with absolutely positioned tick labels (`left: frac*100%`, `translateX(-50%)`).
- `index.html`:
  - The grid gets class `wf3`.
  - After the `.aux` div, add `<div class="wf"><h2>Spectrum</h2><canvas id="wf" width="64" height="120"></canvas><div id="wfaxis" class="wfaxis"></div><p id="wfnote" class="muted">MHz · ±8 MHz around the receiver LO; only channels in this span are visible. Magenta = decoded packet.</p></div>`.
- `app.js`:
  - `const wf = new WaterfallModel(120)`.
  - In the `onStream` handler: `F` → `wf.push(l.bins, l.loMhz)` (not forwarded to `state.onLine`); `P` → `state.onLine(l)` and `wf.mark(2400400 + 1000 * l.ch)`; others → `state.onLine(l)` as before.
  - In `startRx()`, before `RX ON`: `const r = await link.command('SPECTRUM ON')`. If it starts with `ERR`, set `#wfnote` to "Waterfall needs newer ELRS receiver firmware" and carry on (not a connect failure).
  - `render()` calls `drawWaterfall(el.wf, wf, el.wfaxis)`.
  - On connect, reset `wf` (new model).

- [ ] **Step 6: Commit**

```bash
git add web/elrs/waterfall.js web/elrs/test/waterfall.test.js web/elrs/elrs.css web/elrs-rx/ota3.js web/elrs-rx/test/ota3.test.js web/elrs-rx/test/link.test.js web/elrs-rx/index.html web/elrs-rx/app.js
git commit -m "web: shared spectrum waterfall; elrs-rx shows F lines next to AUX"
```

### Task 13b-3: `web/elrs/` waterfall, README, Chrome check

**Files:**
- Modify: `web/elrs/worker.js`, `web/elrs/app.js`, `web/elrs/index.html`, `web/README.md`

**Interfaces:**
- Consumes: `spectrumFromIq8`, `WaterfallModel`, `drawWaterfall` (13b-2). `web/elrs/` imports only from `web/elrs/` (`./waterfall.js`).
- Produces: worker result message gains `spectrum: Uint8Array(64)` (transferable copy); no other message changes.

- [ ] **Step 1: Worker**

In `worker.js`, compute `const spectrum = spectrumFromIq8(new Int8Array(m.iq))` before `decodeSnapshot`, and include it in the `result` message. On a decode error, still post `{type:'error', …, spectrum}` so the row is kept.

- [ ] **Step 2: Page**

- `index.html`: the same `wf3` grid and `.wf` panel as 13b-2, with the note "MHz · ±8 MHz around 2436 MHz; only channels in this span are visible. Magenta = decoded packet."
- `app.js`:
  - `const wf = new WaterfallModel(120)`, reset on connect.
  - In `worker.onmessage`, if `m.spectrum`, call `wf.push(m.spectrum, FREQ_MHZ)`; then for each packet `wf.mark(Math.round(p.channelMhz * 1000))`.
  - `render()` calls `drawWaterfall(el.wf, wf, el.wfaxis)`.
- The rows equal the processed snapshots (about 3 per second here, so 120 rows cover about 40 s).

- [ ] **Step 3: README**

`web/README.md`: one sentence per page that the right-hand panel is a ±8 MHz peak-hold waterfall around the LO (browser FFT on `elrs/`, firmware `F` lines on `elrs-rx/`).

- [ ] **Step 4: Run web tests**

Run: `node --test web/elrs/test/*.test.js web/elrs-rx/test/*.test.js`
Expected: PASS. Then run `grep -n "elrs-rx" web/elrs/*.js`; expected: the only hits are the existing redirect message string in `app.js`.

- [ ] **Step 5: Commit**

```bash
git add web/elrs/worker.js web/elrs/app.js web/elrs/index.html web/README.md
git commit -m "elrs page: browser-FFT waterfall next to AUX"
```

- [ ] **Step 6: [HIL] [USER] Chrome checks**

Serve `web/` with `python3 -m http.server 8000`. TX on DK500 at about 1 m. The user types the phrase.

A. On `esp32-elrs`, open `http://localhost:8000/elrs-rx/`:
1. The waterfall panel is to the right of AUX.
2. It scrolls about 10 rows/s.
3. The axis reads 2430 · 2434 · 2438 · 2442 MHz.
4. ELRS bursts show as short bright dashes at varying columns.
5. Magenta marks line up with bright dashes.
6. Gimbals and updates/s are unchanged (≥ 5).
7. A narrow window (≤ 960 px) shows two columns with the waterfall under AUX, and a phone width shows one column with no horizontal scroll.
8. Reload mid-stream and reconnect: the waterfall restarts.

B. Flash stock and open `/elrs/`:
1. The waterfall scrolls about 3 rows/s, and the axis is centred on 2436.
2. Bursts and magenta marks are visible.
3. Decoding works as before.

Re-flash `esp32-elrs` afterwards.

Expected: all items pass. Report a screenshot of each page to the orchestrator.

---

### Spec amendment (append to `docs/superpowers/specs/2026-10-23-elrs-receiver-firmware-design.md`, §14 Decision record)

**2026-10-23 — SDR waterfall (Task 13b, user request).**

§8 gains the command `SPECTRUM ON|OFF`: `OK`, RAM only, default `OFF`, allowed in any RX state. `CAPS` gains `SPECTRUM`.

There is a fifth streamed line, `F <t_us> <lo_mhz> <128 hex>`. It carries the detector's 64-bin peak-hold spectrum, DC-centred: hex byte j = bin centred at LO + (j − 32) · 250 kHz, value `clamp(round(20·log10(max(|X|², 1))), 0, 255)` in 0.5 dB steps. Each line is the max over all frames of all snapshots in a window of ≥ 100 ms measured on capture trigger timestamps (not wall clock), so at most 10 lines/s (measured about 8.4, about 1.2 kB/s); `t_us` is the window's first trigger time. The window restarts when the LO changes. Lines are sent only while `RX ON` and `SPECTRUM ON`. Clients route by first token, so `F` joins `P`/`S`/`L`/`W`.

§10: both pages show a waterfall panel next to the AUX bars, with 120 rows, newest on top, an auto-levelled 30 dB range, a MHz axis covering ±8 MHz around the LO, and decoded-packet channels marked. `web/elrs/` computes the same spectrum in the browser from each raw I/Q snapshot, with no firmware change. `web/elrs-rx/` sends `SPECTRUM ON` at connect and draws `F` lines; an `ERR` reply only disables the panel. The shared renderer lives in `web/elrs/waterfall.js` (elrs-rx → elrs import direction, as for `dk500.js`).

---

### Task 14: Two-button flasher and CI-built published binaries

**Files:**
- Create: `tools/elrs/publish_web_firmware.py`, `tests/test_publish_elrs_firmware.py`
- Modify: `web/flash/index.html` (second section between `<!-- elrs-firmware:begin -->` / `<!-- elrs-firmware:end -->` markers)
- Create (by the script): `web/flash/manifest-elrs.json`, `web/flash/firmware/esp32-elrs/{0-bootloader.bin,1-partition-table.bin,2-esp_sdr.bin}`

**Interfaces:**
- Consumes: CI artifact `firmware-esp32-elrs` produced by `tools/build_firmware.py` (`manifest.json`, `build-info.json`, `esp32-elrs/*.bin`).
- Produces: `publish(artifact_dir: Path, web_root: Path, commit: str) -> dict` and CLI `publish_web_firmware.py <run-id> [--artifact-dir DIR]` (`--artifact-dir` skips `gh run download`, used by tests).

`publish` checks: `build-info.json` `profile == 'esp32-elrs'` and `version == commit` (full 40-hex); every part's size/SHA-256/MD5 matches the manifest; parts are exactly offsets `0x1000`, `0x8000`, `0x10000` named `0-bootloader.bin`, `1-partition-table.bin`, `2-esp_sdr.bin`; `chip == 'ESP32'`. Then copies the files, writes `manifest-elrs.json` = `{"name":"ESP-SDR ELRS receiver","version":"<commit[:7]>","new_install_prompt_erase":true,"builds":[{"chipFamily":"ESP32","parts":[{"path":"firmware/esp32-elrs/0-bootloader.bin","offset":4096},{"path":"firmware/esp32-elrs/1-partition-table.bin","offset":32768},{"path":"firmware/esp32-elrs/2-esp_sdr.bin","offset":65536}]}]}`, and rewrites the marked block in `web/flash/index.html` with the commit link, the file/offset/SHA-256 table and the GPL source link. Nothing is written if any check fails. `manifest.json` and the stock section are never touched.

- [ ] **Step 1: Write failing tests** (fixture artifact built like `tests/test_firmware_export.py` does, three fake parts):
`test_publishes_manifest_and_table` (exact `manifest-elrs.json`, table contains each SHA-256, stock `manifest.json` bytes unchanged); `test_version_mismatch_writes_nothing`; `test_hash_mismatch_writes_nothing`; `test_wrong_offsets_rejected`.

- [ ] **Step 2: Run to verify failure, implement, run to verify pass**

Run: `python3 -m unittest discover -s tests -k PublishElrs -v`
Expected: FAIL then PASS.

- [ ] **Step 3: Flasher page**

Add the ELRS section with `<esp-web-install-button manifest="manifest-elrs.json">` ("Install ELRS receiver") and the note: "Switching between firmwares means reflashing. A UID saved on the device survives unless you choose to erase."; rename the first button to "Install stock ESP-SDR". Header nav gains `../elrs-rx/`.

- [ ] **Step 4: Publish real binaries (needs a CI run)**

Ask the orchestrator to confirm `git push origin main`. After the `Build firmware` workflow is green for that commit: `python3 tools/elrs/publish_web_firmware.py <run-id>`.
Expected: files under `web/flash/firmware/esp32-elrs/`, `manifest-elrs.json` version = that commit's short hash.

- [ ] **Step 5: [HIL] Flash from the page**

Open `http://localhost:8000/flash/` in Chrome, install ELRS receiver, then stock, then ELRS again; after each, `INFO` matches the chosen firmware and (ELRS) `UID?` still reports `NVS` if saved and erase was declined.

- [ ] **Step 6: Commit**

```bash
git add tools/elrs/publish_web_firmware.py tests/test_publish_elrs_firmware.py web/flash/index.html web/flash/manifest-elrs.json web/flash/firmware/esp32-elrs
git commit -m "flash: second install button for the CI-built ELRS receiver firmware"
```

### Task 15: M2 acceptance session

**Files:**
- Modify: `docs/elrs-receiver.md` (measurement log, M2 result)

- [ ] **Step 1: [HIL] [USER] Stick and switch correctness**

Ask the user, with the radio's channel monitor open: move each stick to both extremes and centre; flip AUX1–AUX8 through every position; set a multi-position switch on AUX8 and step through all positions. Compare each with `web/elrs-rx/`.
Expected: all values match the channel monitor (within one CRSF step for sticks; exact for switches).

- [ ] **Step 2: [HIL] 10-minute plausibility run**

SCAN 10 min at 1 m, logging P lines; a host script re-checks CRC and flags any RC packet whose sticks differ by >40 CRSF from its DVDA twin (same channel, ~1 ms later) or jump >400 CRSF between consecutive packets while the user holds sticks still.
Expected: 0 implausible values; ≥5 RC updates/s average.

- [ ] **Step 3: §9.4 stock regression (repeat Task 13 Step 4 on the published binaries)**

- [ ] **Step 4: Record and decide**

Append BENCH, rate, hit rate, crcfail and the correctness results to `docs/elrs-receiver.md`. Exit: ≥5 updates/s at 1 m, sticks and AUX1–AUX8 correct, no implausible values, §9.4 passes → M2 ships (push after orchestrator confirmation).

- [ ] **Step 5: Commit**

```bash
git add docs/elrs-receiver.md
git commit -m "docs: ELRS receiver step 1 measurements and acceptance"
```

---

## M3 — Step 2a: TIMED

### Task 16: Timing lock (`lock.c`) with simulation

**Files:**
- Create: `main/elrs/lock.h`, `main/elrs/lock.c`, `tests/elrs_lock_test.c`
- Modify: `tests/elrs_txsim.h` (connected flag already present; add `txsim_set_ptr_nonce` if needed), `tests/test_elrs.py` (`test_lock`)

**Interfaces:**
- Consumes: `elrs_fhss_build`, `elrs_uid_seed`, `elrs_fhss_ptr` (Task 4), `elrs_sync_t` (Task 3), txsim (Task 4).
- Produces (`lock.h`):

```c
typedef enum { ELRS_LOCK_ACQ, ELRS_LOCK_LOCK, ELRS_LOCK_COAST, ELRS_LOCK_LOST } elrs_lock_state_t;
typedef struct { double t_us; uint8_t channel; bool is_sync; elrs_sync_t sync; float cfo_hz; } elrs_obs_t;
typedef struct { double t_us; uint8_t channel; unsigned ptr; int nonce; /* -1 unknown */ int64_t index; } elrs_pred_t;
typedef struct { elrs_lock_state_t state; const char *reason; bool changed; } elrs_lock_event_t;
typedef struct elrs_lock { /* defined in lock.h for static allocation; only lock.c touches the fields */ } elrs_lock_t;
void elrs_lock_init(elrs_lock_t *l, const uint8_t uid[6]);
elrs_lock_event_t elrs_lock_observe(elrs_lock_t *l, const elrs_obs_t *o);   /* any decoded packet */
elrs_lock_event_t elrs_lock_miss(elrs_lock_t *l, double t_us);                /* scheduled capture, no valid packet */
elrs_lock_event_t elrs_lock_tick(elrs_lock_t *l, double now_us);             /* COAST timeout (2 s) -> LOST -> ACQ */
bool elrs_lock_next(const elrs_lock_t *l, double after_us, unsigned lo_mhz, unsigned inband_khz, elrs_pred_t *out); /* next predicted packet in band after time */
bool elrs_lock_next_sync(const elrs_lock_t *l, double after_us, elrs_pred_t *out);  /* next sync-channel visit (SYNC bias in ACQ) */
double elrs_lock_error_us(const elrs_lock_t *l);                             /* RMS of the last 64 residuals */
```

Rules (§7): SYNC acquisition requires `seq[fhss_index] == 41`, `rate_index == 14`, `elrs_sync_uid_ok`, `sw_mode == 1` (Wide: reason `wide_mode`, no lock); it sets ptr, nonce and pair phase. Triplet path: hypotheses `(p1, δ)` with δ ∈ {−1000, 0, +1000} µs over the 3 positions where `seq[p] == c1`; keep those with `seq[(p1+Δh) mod 240] == c2`; lock when exactly one survives a 3rd and 4th observation; gaps < 5 s; nonce stays −1. Tracking: `t(N) = T0 + N·P`, `P` initialised `1000·(1 − cfo/f_c)` µs, α-β filter α 0.1 β 0.005, gate residuals > 150 µs; every SYNC cross-checks pointer/nonce — mismatch → immediate relock (reason `resync`); 3 consecutive RC packets at an unexpected channel/time → `LOST` (reason `mismatch`); 40 consecutive `elrs_lock_miss` → `COAST`; COAST > 2 s → `LOST` → `ACQ`. TLM slots (nonce % 128 == 0 when nonce known) are not predicted.

- [ ] **Step 1: Write failing tests `tests/elrs_lock_test.c`** — simulated observer that "captures" a txsim packet if it lies in band at LO 2438, with configurable loss; time base in µs from txsim:

- `sync_primary`: ±100 ppm, 50% loss, 1000 runs from random start → median time-to-lock ≤ 2 s and 95th percentile ≤ 5 s (with 10% SCAN catch modelled as Bernoulli 0.1 per SYNC); after 1 s of lock, |predicted − actual| < 50 µs for every in-band packet.
- `triplet_secondary`: txsim `connected` (SYNC every 5 s), RC observations only → locks with nonce −1; false locks < 0.1% over 10 000 runs (false = predicted channel wrong for >50% of the next 100 packets).
- `loss_sweep`: loss 0, 30, 60, 90% → lock eventually in all; prediction error target holds for ≤60%.
- `reboot`: `txsim_reboot` mid-run → `LOST` or `resync` within 200 ms of the next SYNC, re-lock afterwards.
- `nonce_jump`: SYNC with nonce +37 → immediate relock with the new nonce.
- `coast_lost_reacquire`: stop feeding packets → COAST after 40 misses, LOST after 2 s, ACQ, then re-lock when packets resume; events emitted in that order with `changed == true` exactly once each.
- `wide_rejected`: SYNC with `sw_mode == 0` → no lock, reason `wide_mode`.

- [ ] **Step 2: Run to verify failure, implement `lock.c`, run to verify pass**

Run: `python3 -m unittest discover -s tests -k test_lock -v`
Expected: FAIL then PASS; print lock-time median/p95 and false-lock rate.

- [ ] **Step 3: Commit**

```bash
git add main/elrs/lock.c main/elrs/lock.h tests/elrs_lock_test.c tests/elrs_txsim.h tests/test_elrs.py
git commit -m "elrs: SYNC/triplet timing lock with drift tracking and simulation tests"
```

### Task 17: TIMED mode on the device

**Files:**
- Modify: `main/targets/esp32/elrs/app.c`, `docs/elrs-receiver.md`

**Interfaces:**
- Consumes: `elrs_lock_*` (Task 16), `radio_arm/radio_trigger/radio_finish` (Task 9), `elrs_fmt_lock` (Task 8).
- Produces: `MODE TIMED` → `OK` (TIMED only once locked; until then SCAN with `state=ACQ`); `MODE AUTO` = SCAN → TIMED on lock, back on LOST; `L` line on every state change; `S` `state=` from the lock, `lock_err_us` from `elrs_lock_error_us`; `P` lines carry the predicted nonce when known; `CAPS` gains `FHSS TIMED`.

Scheduler (§7.2): in LOCK, `elrs_lock_next(now + arm_budget)` picks the next in-band packet; `radio_arm` before, `radio_trigger(pred.t_us − 250)` so the packet sits ~250 µs into the 1.02 ms window; decode only `pred.channel` with the lock's CFO EMA (add `elrs_decode_cfg_t` fields `force_channel` (−1 = none) and `force_cfo_hz` in `dsp.h`, with a host test in `elrs_decode_test.c`: forced channel on the real fixture decodes the same packets and skips detection on other channels). A capture without a valid packet → `elrs_lock_miss`. A stale recalibration in a timed mode → re-tune, `W stale`, and treat the capture as a miss (the lock drifts into COAST rather than breaking). In ACQ, after any sync-channel packet, schedule a capture for `elrs_lock_next_sync` (SYNC bias). Convert lock times between `esp_timer` µs and `double`.

- [ ] **Step 1: Host test for forced-channel decode (fail → implement → pass)**

Run: `python3 -m unittest discover -s tests -k test_decode -v`
Expected: PASS including `forced_channel`.

- [ ] **Step 2: Implement TIMED in `app.c`, build both profiles**

- [ ] **Step 3: [HIL] [USER] M3 measurements**

TX on at 1 m, RX off. (a) 20× from `RX ON` in SCAN: time to the `L LOCK` line. (b) Ask the user to power-cycle the TX 5 times; measure `L LOST` → `L LOCK`. (c) Hold lock for 10 min; log updates/s, `lock_err_us`, miss rate. (d) Optional, ask the user: power the bound RX to exercise the triplet path; measure lock time. (e) R3: compare `GAIN HARDWARE` with a `GAIN MANUAL` sweep (every 8 codes) in TIMED at 1 m and, asking the user, at ~5 m; watch `W clip` and the gain field; make manual gain the TIMED default only if it decodes more.
Expected (exit): lock < 2 s typical; holds 10 min; ≥40 updates/s; reacquires after every TX power cycle. **K2**: lock error > 250 µs or false locks > 1% → leave `MODE AUTO` = SCAN, keep TIMED behind `MODE TIMED`, ship step 1 only.

- [ ] **Step 4: [HIL] Page badge** — `LOCKED·TIMED` appears in `web/elrs-rx/` and drops to `SCAN` on TX power-off.

- [ ] **Step 5: Record results and K2 decision in `docs/elrs-receiver.md`; commit**

```bash
git add main/targets/esp32/elrs/app.c main/elrs/dsp.c main/elrs/dsp.h tests/elrs_decode_test.c docs/elrs-receiver.md
git commit -m "esp32-elrs: TIMED captures from the FHSS schedule (step 2a)"
```

---

## M4 — Step 2b: HOP (gated on retune latency)

### Task 18: Fast retune and `RETUNE` latency measurement

**Files:**
- Modify: `main/targets/esp32/elrs/radio.c`, `radio.h`, `app.c`, `docs/elrs-receiver.md`

**Interfaces:**
- Produces: `bool radio_tune_fast(unsigned mhz)` — after one full calibration at 2440 MHz, retune with `rom_set_rf_freq_offset` alone after releasing the capacitor-code pin (`ram_chip_i2c_writeReg(0x62, 1, 0, cap & ~0x80u)`), as `tune_rx` does for non-Wi-Fi channels; `bool radio_tune_full(unsigned mhz)` (= `tune_rx` + prepare); `RETUNE <a> <b> <n> FULL|FAST` → `RETUNE call_us=<p50>/<p99>/<max> fails=<k>` (alternating a/b, ESP sweeps the full band when a=2402 b=2478 by stepping 1 MHz); `CAPS` gains `RETUNE`. A "fail" is a retune after which a quick 256-sample capture shows the DC/noise floor deviating > 6 dB from the calibrated reference (PLL unlock symptom).

- [ ] **Step 1: Implement, build both profiles**

- [ ] **Step 2: [HIL] Software call time**

Run: `RETUNE 2402 2478 1000 FULL` and `RETUNE 2402 2478 1000 FAST`.
Expected: two lines; record p50/p99/max and fails. If FAST fails > 2% of channels or call time is unusable, record **K3 = no-go** and skip Task 19 (TIMED stays, still 50+/s).

- [ ] **Step 3: Commit**

```bash
git add main/targets/esp32/elrs/radio.c main/targets/esp32/elrs/radio.h main/targets/esp32/elrs/app.c docs/elrs-receiver.md
git commit -m "esp32-elrs: fast retune and RETUNE latency command"
```

### Task 19: HOP mode, `LEAD`, per-channel qualification, AUTO chain

**Files:**
- Modify: `main/targets/esp32/elrs/app.c`, `docs/elrs-receiver.md`
- Modify: `main/elrs/lock.c/.h` (`elrs_lock_next_any`: next packet on any channel), `tests/elrs_lock_test.c`

**Interfaces:**
- Consumes: Tasks 16–18.
- Produces: `elrs_lock_next_any(const elrs_lock_t*, double after_us, bool second_of_pair, elrs_pred_t*)` (host test: returns only second-of-pair packets when asked, never TLM slots); `LEAD <us>` → `OK` (0 = auto); HOP scheduler: retune after packet 1's predicted end, capture packet 2 of the DVDA pair, LO = `ceil(f_ch MHz) − 2` (signal ≈ +1.6 MHz), every k-th hop with `k = ceil((retune + arm + capture + finish + decode) / 2000 µs)`; per-channel qualification table (RAM): during LOCK in AUTO, interleave trial HOP captures; channel qualified after ≥10 trials at ≥90% of the in-band TIMED decode rate with CFO spread < 30 kHz; excluded channels fall back to TIMED-only coverage; HOP is "qualified" once ≥64 of 80 channels qualify [D, tune from §9.3 data]. `MODE HOP` → `ERR hop_unqualified` until then; `CAPS` lists `HOP` only when qualified; AUTO: SCAN → TIMED → HOP, falling back down on loss; `S ret_us` = mean retune time.

- [ ] **Step 1: Host test for `elrs_lock_next_any` (fail → implement → pass)**

Run: `python3 -m unittest discover -s tests -k test_lock -v`
Expected: PASS.

- [ ] **Step 2: Implement HOP in `app.c`, build both profiles**

- [ ] **Step 3: [HIL] §9.3 settling and qualification**

In a TIMED lock: sweep `LEAD` 100…3000 µs (steps of 100) in `MODE HOP` for 30 s each; record decode success and the CFO difference between the first and last 48 bits of each packet. The knee is the retune-plus-settle latency. Then run AUTO for 10 min and dump per-channel qualification.
Expected (exit): **K3** go if settle ≤ 15 ms and ≤2% channels fail; then ≥100 updates/s in HOP. No-go → `MODE HOP` permanently `ERR hop_unqualified`, AUTO stops at TIMED.

- [ ] **Step 4: Record K3 decision and numbers in `docs/elrs-receiver.md`; commit**

```bash
git add main/targets/esp32/elrs/app.c main/elrs/lock.c main/elrs/lock.h tests/elrs_lock_test.c docs/elrs-receiver.md
git commit -m "esp32-elrs: HOP mode with per-channel qualification (step 2b)"
```

---

## M5 — Optional dual core

### Task 20: Dual-core DSP (only if measurements justify it)

**Files:**
- Modify: `sdkconfig.defaults.esp32-elrs` (`CONFIG_FREERTOS_UNICORE=n`), `main/targets/esp32/elrs/app.c`, `docs/elrs-receiver.md`

**Interfaces:**
- Produces: radio task pinned to core 0 (capture, NVS, serial), DSP task pinned to core 1 receiving converted buffers through a 2-slot queue; core 1 never touches 0x3ffe8000–0x3fff7fff while the dump engine owns it (convert happens on core 0 before hand-off); no `DPORT_REG_READ` in core-1 code paths.

- [ ] **Step 1: Re-run `BENCH 500` and the 10-min rate in the current mode as the baseline**
- [ ] **Step 2: Implement the split, build both profiles; stock profile still unicore**
- [ ] **Step 3: [HIL] Re-run BENCH and the rate**
Expected: ≥1.5× updates/s or lower `total_us` p50; otherwise revert this task's commit.
- [ ] **Step 4: Commit (or revert) and record the outcome**

```bash
git add sdkconfig.defaults.esp32-elrs main/targets/esp32/elrs/app.c docs/elrs-receiver.md
git commit -m "esp32-elrs: run DSP on core 1"
```

---

## Self-review notes (resolved while planning)

- Spec §5.4 in-band limit ≤6 MHz contradicts the fixture (packet at +7.4 MHz); plan uses 7.5 MHz and documents it.
- Spec §5.5 mirror rule "±f": the fixture's ghosts are not at −f (e.g. real +5.65 MHz, ghost −0.94 MHz); plan drops the >10 dB weaker of time-overlapping bursts regardless of frequency, polarity remains the decisive check.
- `burst_gpio.c` is needed by `burst_serial_init()` though §5.1 omits it.
- `STATS?` reply is an `S` line, the one exception to "replies never start with a stream token".
- `CAPS` grows with milestones; `MODE TIMED` returns `ERR timed_unavailable` before M3; `RX ON` without UID → `ERR no_uid`.
- FFT: own portable `elrs_fft64` so host builds need no esp-dsp; `BENCH` decides whether to swap in `dsps_fft2r_sc16_ae32` on the device.
- `export_web_firmware.py` rejected hyphenated profile ids in the version record; Task 1 relaxes the regex.
- SYNC packets expose UID3; committed fixtures exclude SYNC-bearing snapshots.
