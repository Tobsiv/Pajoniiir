# Pajoniiir BL-A1800

Standalone dual-deck DJ system built around a JC4880P443C_I_W ESP32-P4
multimedia board. The P4 is the whole product: it reads Rekordbox media from USB
directly, drives the LVGL touchscreen UI, runs the decode/mixer/DSP engine,
takes controller input as a raw MIDI 1.0 byte stream on a UART and makes all
authoritative deck, mixer and LED decisions. No ESP32-S3, no host PC and no
controller USB host are required at runtime.

Canonical repository: `https://github.com/dvucinozd/Pajoniiir.git`. The former
`dvucinozd/ESP32-DDJ-FLX4` URL is deprecated and retained only as a GitHub
redirect.

![Pajoniiir](docs/images/122.jpg)

> [!IMPORTANT]
> The active P4 target builds only under **ESP-IDF v6.0.2** (the component
> manifest pins `idf: "==6.0.2"`) and targets **pre-v3 ESP32-P4 silicon**
> (board-observed v1.3 — `sdkconfig.defaults` keeps the three revision
> selectors; omitting them silently retargets rev 3.1, which will not boot on
> the production board).
>
> This branch (`refactor/p4-single-usb-host`) reworks the P4 into a standalone
> controller: a **single USB host for media only**, a **UART MIDI input**
> (raw MIDI 1.0 byte stream, 19200 8N1) that any external controller feeds, and
> **two PCM5102A DACs** for MAIN and headphone-cue audio. `idf.py build` and
> `tests/run_p4_host_tests.ps1` pass. Hardware acceptance is in progress for the
> UART controller round-trip (PFL + LED echo), the second CUE DAC,
> flash-without-DFU + monitor, USB sticks behind a hub and dual-stick hot-plug.
> The earlier dual-USB-host line (`feat/p4-dual-usb-host`, where the P4 hosted
> the FLX4 directly over USB1) and the two-board `RC1`/`RC2` history are retained
> in Git and dated documents.

## Why the architecture changed

`feat/p4-dual-usb-host` had the P4 host both USB0 storage and the USB1 FLX4
MIDI/audio interface through a forked, patched `esp-usb`. That broke serial
monitoring, forced DFU-mode flashing and blocked external USB hubs. This branch
removes the dual-root stack: `usb_storage.c` owns one `usb_host_install()` on
released `espressif/usb 1.5.0` + `usb_host_msc 1.2.0`, so monitoring, DFU-free
flashing and hub enumeration all work again. Controller input moved to a UART
MIDI stream (fed by any external MIDI source — see below) and headphone-cue
audio moved to a dedicated second DAC. The P4 no longer hosts USB-MIDI at all,
so a USB-only controller such as the DDJ-FLX4 needs a small USB-MIDI-host bridge
MCU in front of the UART; `controller_runtime` keeps the built-in DDJ-FLX4 map,
so a bridge that forwards the FLX4's raw MIDI works without configuration.

## System at a Glance

| Part | Role |
| --- | --- |
| **Controller (any MIDI source on the UART)** | Operator surface: transport, jogs, tempo, mixer, pads, cue. It must present a raw MIDI 1.0 byte stream on the P4's UART RX — directly (a microcontroller, or a controller with a TRS/serial MIDI OUT) or through a USB-MIDI-host bridge MCU for a USB-only controller like the DDJ-FLX4. The bundled bring-up sketch (`firmware/main-deck-p4/Atmega32u4_test/`) runs on a Circuit Playground Classic and sends DDJ-FLX4-compatible messages. |
| **ESP32-P4 board** | Single USB media host, merged Rekordbox library, authoritative playback/deck state, LVGL UI, audio DSP/mixer, MAIN + cue routing, controller LED feedback (FLX4-shaped notes on the UART TX), signed OTA. |

```
MIDI controller ─ MIDI 1.0 ─ UART ─▶ midi_uart_link ─▶ controller_runtime ─▶ semantic queue ─▶ deck_core
                                        ▲                                                          │
                                        └─ LED sink: FLX4-shaped notes on UART TX ◀── control_link ┤
                                                                                          audio_engine + UI
                                                                                                  │
                                                                            PCM5102A MAIN (I2S1) + PCM5102A CUE (I2S0)
```

Detailed ownership and data flow: [Architecture](docs/ARCHITECTURE.md). The
historical `0xA5`/`0xA6` P4/S3 UART transport survives only in
[Control Link Protocol](docs/CONTROL_LINK_PROTOCOL.md) and Git history.

## Current Capabilities

- Two independent decks with Rekordbox library browsing and MP3, WAV and FLAC
  playback. Compressed audio uses a bounded LRU page cache (8 × 32 KiB per deck)
  instead of whole-file PSRAM allocation. The WAV subset is classic RIFF/WAVE
  PCM16 mono/stereo.
- **Up to three USB sticks at once** (via a hub) merged into one library:
  `library_init()` reads the `export.pdb` of every mounted stick,
  source-slot-salts the track keys so ids never collide, and a deck playing from
  one stick keeps playing when another is inserted or removed. The Library
  screen has an `ALL / A / B / C / D` source filter and a one-letter origin
  badge per row; `/api/library` streams the source.
- FLX4 transport, jog/vinyl scratch, tempo and Master Tempo, mixer/EQ, hot cues,
  loops, beat jump/sync, Pad FX and Beat FX (Filter, Echo, Flanger, one-shot
  Delay) + Smart CFX. Filter and Echo have recorded hardware acceptance on the
  historical S3 path; the UART link needs its own acceptance row.
- Simultaneous PCM5102A RCA MAIN output and a second PCM5102A headphone-cue DAC
  (I2S0). ES8311 is compiled only for dev boards (`CONFIG_BSP_ES8311_MONITOR`).
- P4-owned controller LED feedback (FLX4-shaped notes on the UART TX) with
  reconnect and board-reboot resynchronization.
- LVGL Overview, Library (paginated 8-row table), Hot Cues and Settings tabs on
  the 800×480 panel, PPA hardware rotation, GT911 touch.
- Data-driven controller profiles loadable from SD; the built-in DDJ-FLX4 map is
  the fallback. Profile activation over the UART link needs a UART-side selector
  and is currently dormant.
- Signed dual-slot P4 OTA (`main-deck-p4.ddjota`, ECDSA P-256 manifest),
  validation and rollback.

## Interface

The captures are representative; small UI details may be newer in firmware.

| Overview | Library | Settings |
| --- | --- | --- |
| ![Overview screen](docs/images/overview.jpg) | ![Library screen](docs/images/library.jpg) | ![Settings screen](docs/images/settings.jpg) |

The Hot Cues tab is implemented but does not yet have an archived screenshot.

## Repository Layout

```text
controllers/                 Compiled and source controller profiles
firmware/
  main-deck-p4/               ESP32-P4 complete product firmware
    Atmega32u4_test/          UART serial-MIDI bring-up sketch (Circuit Playground Classic)
    components/midi_uart_link/ UART MIDI link + controller runtime ownership
  common/                     Shared firmware components (OTA manifest, health, schedulers)
docs/                         Product, protocol, validation and design records
tests/                        PC-side regression tests (tests/run_p4_host_tests.ps1)
tools/                        Profile compiler, OTA packager and support tools
```

## Build and Test

Required baseline: **ESP-IDF v6.0.2** and its matching Espressif Python and
toolchain environment. Host tests additionally require native GCC and
PowerShell 5.1+ on Windows (or a standard shell on Linux).

Initialize an ESP-IDF 6.0.2 shell (Windows example) and verify the version:

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
idf.py --version    # must report ESP-IDF v6.0.2
```

Build the P4 target:

```powershell
cd firmware\main-deck-p4
idf.py set-target esp32p4
idf.py build
idf.py -p COM15 flash        # no DFU mode needed on this branch
```

Run the host regression suite (the same entry point CI uses; Windows PowerShell
5.1 and PowerShell 7):

```powershell
.\tests\run_p4_host_tests.ps1
```

If `gcc` is not on `PATH`, **append** msys2 rather than prepending it —
prepending shadows the system `python.exe` with msys2's, which cannot run the
OTA signing suite:

```powershell
$env:Path = "$env:Path;C:\msys64\ucrt64\bin"
```

Headless LVGL navigation + exact-framebuffer screenshot gate:

```powershell
.\tests\ui_simulator\run_ui_simulator_e2e.ps1
```

The first run fetches the pinned LVGL source into the ignored `.cache`
directory. This PC gate does not replace P4 display, touch or waveform-motion
hardware acceptance.

Flashing, signed release packaging and rollback are covered by
[OTA Update](docs/OTA-UPDATE.md); bring-up and recurring acceptance checks are
in the [Startup Checklist](docs/STARTUP_CHECKLIST.md).

## Documentation

Start with the [complete documentation index](docs/README.md) and
[Documentation Status](docs/DOCUMENTATION_STATUS.md) (scope and source-of-truth
policy). Primary operational documents:

| Topic | Document |
| --- | --- |
| P4 developer guide (current, most detailed) | [`firmware/main-deck-p4/CLAUDE.md`](firmware/main-deck-p4/CLAUDE.md) |
| Product shape and implemented scope | [Project Overview](docs/PROJECT_OVERVIEW.md) |
| Responsibilities and data flow | [Architecture](docs/ARCHITECTURE.md) |
| FLX4 inputs, outputs and acceptance ledger | [DDJ-FLX4 MIDI Map](docs/DDJ_FLX4_MIDI_MAP.md) |
| Serial-MIDI link pins | [`firmware/main-deck-p4/CLAUDE.md`](firmware/main-deck-p4/CLAUDE.md) · [PINOUT_P4.md](firmware/main-deck-p4/PINOUT_P4.md) |
| Historical P4/S3 UART protocol | [Control Link Protocol](docs/CONTROL_LINK_PROTOCOL.md) |
| Wiring, USB and audio connections | [Hardware Wiring](docs/HARDWARE_WIRING.md) |
| Current phases and remaining work | [Development Plan](docs/DEVELOPMENT_PLAN.md) |
| Open and accepted risks | [Risk Register](docs/RISK_REGISTER.md) |

> [!NOTE]
> Several documents under `docs/` still describe the `feat/p4-dual-usb-host`
> dual-USB architecture (P4 hosting the FLX4 over USB1). `CLAUDE.md` is the
> current source of truth for this branch; the dated `docs/` records are kept
> as design and validation history.

Dated design records explain intent; they do not override current firmware or
active operational documents.
