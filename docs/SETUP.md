# Setup on a new Windows machine

Tested on Windows 11 (build 26300) x64. Windows 10 22H2 should behave the same.

## 0. Prerequisites

- A **Bluetooth radio that is not doing anything else you need.** Claiming it for WinUSB
  removes it from Windows — no mice, keyboards or headset mic through it while running.
  An Intel AX2xx combo card works; a BT 4.0+ USB dongle is the safest choice.
- Administrator rights.
- Headphones that support LDAC (Sony WH-1000XM2/XM3/XM4/XM5, WF series, etc.).

> Pre-SSP (Bluetooth 2.0/2.1) dongles **cannot** work — modern sinks reject legacy
> pairing with `0x18 PAIRING_NOT_ALLOWED`. See [FINDINGS.md](FINDINGS.md#7-pre-ssp-dongles-cannot-work).

## 1. Find your radio

```powershell
Get-PnpDevice -Class Bluetooth, USB, USBDevice -Status OK |
  Select-Object FriendlyName, Class, InstanceId | Format-Table -AutoSize
```

Note the `InstanceId` of the radio you intend to claim, e.g.
`USB\VID_8087&PID_0032\8&F2CB6FA&0&14`.

## 2. Claim it for WinUSB

1. Download [Zadig](https://zadig.akeo.ie/) and run it as Administrator.
2. **Options → List All Devices.**
3. Pick the radio from the dropdown (it shows the USB VID/PID, not a friendly name).
4. Target driver: **WinUSB**. Click **Install Driver** (or *Replace Driver*).
5. Confirm Windows no longer shows the radio in Bluetooth settings.

Verify:

```powershell
.\scripts\verify-winusb.ps1
```

## 3. Pair the headphones

1. Put the headphones into **pairing mode** (hold power until the light flashes).
2. Put `a2dp_source_demo.exe` in a **fixed folder** — the link key is written to
   `btstack_<YOUR-RADIO-MAC>.tlv` in the *current working directory*, so launching from
   elsewhere loses the bond.
3. Run it (`.\scripts\run.ps1 -Addr AA:BB:CC:DD:EE:FF -Mode HQ`).
4. The headphones will announce a pairing request — accept it. BTstack supports SSP
   just-works and numeric comparison; on Windows 11 no PIN dialog appears for
   just-works, the headphones just chime.

On success you should see:

```
LDAC: set_config_other status 0x00 (local 1 -> remote 3)
A2DP Source: Stream established ... remote_seid 0x03
LDAC: encoder initialised, 990 kbps (HQ), target AC:80:0A:F2:C5:A2
```

## 4. Run

```powershell
.\a2dp_source_demo.exe --addr AC:80:0A:F2:C5:A2 --mode HQ
```

| Flag | Meaning |
|---|---|
| `--addr` | Target BD address (default: the XM4 used during development) |
| `--mode` | `HQ` 990 kbps / `SQ` 660 / `MQ` 330 (default `HQ`) |
| `--source` | `tone` (default, generated sine) or `system` (WASAPI loopback) |
| `--endpoint auto\|N\|"name"` | auto (default): follow the endpoint with an active session; or force one by index/name |
| `--scan` | list nearby Bluetooth devices, then exit |
| `--list-endpoints` | prints active render endpoints as `[n] name`, then exits |
| `--gain 0-100` | test-tone level in percent (default 12) — irrelevant for `system` |
| `--help` | Usage |

## 5. Capturing real system audio

Endpoints are Windows' individual output devices — "Speakers (Realtek)", "LG TV",
"Headphones (Oculus Virtual)", etc. Windows renders to **one** at a time, so the
capture has to open that same one.

```powershell
.\a2dp_source_demo.exe --list-endpoints
[0] Speakers (Steam Streaming Microphone)
[1] LG TV SSCR2 (NVIDIA High Definition Audio)
[2] Headphones (Oculus Virtual Audio Device)
[3] Realtek Digital Output (Realtek(R) Audio)

# by index, or by a substring of the name — far friendlier across machines:
.\a2dp_source_demo.exe --addr AC:80:0A:F2:C5:A2 --mode HQ --source system --endpoint "LG TV"
```

Start playback in any app, then run it. If you hear nothing:

1. Confirm the endpoint index matches the device the app is actually using
   (Windows Settings → System → Sound shows the active output).
2. Beware **virtual or HDMI endpoints** — they can render nothing and loop back as
   silence even though capture "works".
3. Verify capture independently with `wa_test.exe selftest <n>`, which renders a tone
   to that endpoint and captures it back (prints `LOOPBACK WORKS` or not).

Env equivalents `LDAC_ADDR` / `LDAC_MODE` / `LDAC_SOURCE` / `LDAC_GAIN`. Stop with Ctrl-C.

**Quality guidance:** HQ runs close to the link's limit; refusals are absorbed by a
hold-and-retry buffer rather than dropped, but on a congested 2.4 GHz band `--mode SQ`
is more robust and still well above SBC.

## Restoring Windows Bluetooth

Pick one:

**A. Device Manager (always works)**

1. Device Manager → **View → Show hidden devices**.
2. Find the radio (often under *Universal Serial Bus devices* or *Bluetooth* with a
   warning icon).
3. Right-click → **Uninstall device** → tick *Attempt to remove the driver for this device*.
4. **Action → Scan for hardware changes.** Windows reinstalls its own driver.

**B. PowerShell**

```powershell
.\scripts\restore-bluetooth.ps1 -InstanceId 'USB\VID_8087&PID_0032\8&F2CB6FA&0&14'
```

You will then need to re-pair the headphones **with Windows**, because the headset's
stored link key belongs to this stack, not to Windows.

## Building on Windows (MSYS2)

```bash
pacman -S mingw-w64-x86_64-gcc make git
# then, from the repo root, in an MSYS2 mingw64 shell:
WORK=$PWD/.build ./build/build-windows.sh
```

The script generates the case-sensitivity header shim automatically; on Windows that
step is a no-op because the filesystem is already case-insensitive.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `0x18` on pairing | Dongle lacks SSP — use a BT 4.0+ radio |
| `0x05` Authentication Failure | Stale link key — delete `btstack_*.tlv` and re-pair |
| `0x6a` disconnect right after connect | Sink refused AVDTP because no authentication happened |
| Connects, no audio | Streaming started but no packets — check `--mode` and that Windows' own Bluetooth is off |
| Everything wedged after start | LE commands sent to a BR/EDR-only controller; use the prebuilt binary (already gated) |
| Crackle at HQ | Rebuild properly: `rm -f *.o *.exe` (header changes are not tracked) |
