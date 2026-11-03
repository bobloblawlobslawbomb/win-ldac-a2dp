# win-ldac-a2dp

**LDAC 990 kbps audio from Windows — no kernel driver, no code-signing, no purchase.**

Microsoft's inbox `BthA2dp.sys` only speaks SBC and AAC. This project drives a
Bluetooth radio directly from user space (WinUSB + [BTstack](https://github.com/bluekitchen/btstack))
and encodes with Sony's own [libldac](https://github.com/wdv4758h/libldac), so a
Windows machine can stream LDAC to any LDAC-capable sink.

Verified end-to-end on an Intel AX201 against Sony WH-1000XM4, **990 kbps, stable**.

| | |
|---|---|
| Codec | LDAC HQ 990 / SQ 660 / MQ 330 kbps |
| Transport | A2DP over AVDTP → L2CAP, stock BTstack |
| Privileges | Administrator (driver assignment only) |
| Kernel driver | **none** |
| Cost | **£0** |

## Status

| Area | State |
|---|---|
| LDAC HQ/SQ/MQ streaming | **working, verified** |
| Pairing (SSP), link-key persistence, AVRCP volume | working |
| Configurable target / quality (`--addr`, `--mode`) | working |
| **Audio source** | **test tone (v0.1)** |
| System-audio capture (WASAPI loopback) | **next milestone** |
| Installer / auto-start | not yet |

**Read that honestly:** v0.1 proves the hard part — that a signed-kernel-driver-free
LDAC path is possible on Windows — but it plays a sine wave, not your music.
System audio is the next milestone and the tracking issue.

## How it works

```
 your app  →  [ test tone ]  →  libldac encoder  →  A2DP/AVDTP  →  BTstack  →  WinUSB  →  radio  →  headphones
```

Windows is taken out of the Bluetooth audio path entirely: the radio is claimed by
WinUSB, BTstack owns the HCI/L2CAP/AVDTP layers, and libldac does the encoding.

## Quick start

1. **Claim the radio.** Install [Zadig](https://zadig.akeo.ie/), enable *List All
   Devices*, select your Bluetooth radio, choose **WinUSB**, click *Install Driver*.
   ⚠️ This disables normal Windows Bluetooth until you undo it — see
   [docs/SETUP.md](docs/SETUP.md#restoring-windows-bluetooth).
2. **Pair.** Put the headphones in pairing mode.
3. **Run**, from a fixed folder (the link key is stored next to the executable):

   ```powershell
   .\a2dp_source_demo.exe --addr AC:80:0A:F2:C5:A2 --mode HQ
   ```

4. Accept the pairing prompt on the headphones. Audio starts once AVDTP connects.

Environment equivalents: `LDAC_ADDR`, `LDAC_MODE`.

## Build from source

Cross-compiling from Linux (or WSL) needs `mingw-w64`; the script pins both
upstream revisions and applies the patch set.

```bash
sudo apt install mingw-w64 git make
./build/build-windows.sh
# -> dist/a2dp_source_demo.exe
```

See [docs/SETUP.md](docs/SETUP.md) for the Windows-side build with MSYS2.

## Known limits

- **One radio per machine.** The claimed radio is unavailable to Windows while running.
- **No system audio yet** — test tone only (v0.1).
- **Latency/quality trade-off at HQ.** The link sustains ~90 packets/s; the encoder's
  refusals are absorbed by a hold-and-retry buffer rather than dropped, which is what
  removed the crackle.
- Combo (Wi-Fi + BT) adapters work; a dedicated adapter is recommended.

## Licensing — read before you redistribute

- **BTstack** is BSD-3-Clause **plus a non-commercial clause**: redistribution and use
  are permitted only for personal, non-commercial purposes. Building this for yourself,
  or publishing a non-commercial fork, is fine. Selling it, or shipping it in a product,
  needs a commercial licence from BlueKitchen GmbH.
- **libldac** is Apache-2.0 (Sony). Shipping LDAC *products* additionally requires LDAC
  certification from Sony; personal use does not.
- This project's own code is MIT. See [THIRD-PARTY.md](THIRD-PARTY.md).

No Sony, Intel or Microsoft code is redistributed here — the build fetches upstream at
pinned revisions.

## Credits

- [BlueKitchen BTstack](https://github.com/bluekitchen/btstack) — HCI/L2CAP/AVDTP/A2DP stack
- [libldac](https://github.com/wdv4758h/libldac) (AOSP) — LDAC encoder
- [A2DP-Windows-Bridge](https://github.com/SeiyaFunaokaJP/A2DP-Windows-Bridge) — proved the
  WinUSB-claimed-radio approach was viable
- [docs/FINDINGS.md](docs/FINDINGS.md) — the non-obvious failures and their fixes
