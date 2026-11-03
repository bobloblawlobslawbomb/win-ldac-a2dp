# Debugging notes — what actually broke, and why

Everything here cost real time to find. Read it before changing the code.

## 1. The 330 kbps ceiling was a Windows timer, not the radio

**Symptom:** LDAC negotiated at 990 kbps, but only ~62 packets/s went out (~330 kbps).
HQ and SQ both capped at the same rate; MQ played correctly because it *is* ~62 pkt/s.

**Misdiagnosis to avoid:** HCI captures showed `Number of Completed Packets` arriving
every 16.00 ms, one packet per event, which looks exactly like an air-interface limit.
It is not. 16.00 ms ≈ 15.6 ms, the default Windows timer tick.

`btstack_run_loop_windows.c` waits with `WaitForMultipleObjects(..., timeout_ms)` /
`Sleep(timeout_ms)`. Without raising timer resolution, any timeout below ~16 ms is
rounded up to 15.6 ms — so a 10 ms audio timer fires every 16 ms, sending one packet
per tick.

**Fix:** `timeBeginPeriod(1)` at startup, and a 2 ms audio timer.

```
62.5 pkt/s  →  175.3 pkt/s  (925 kbps)
```

## 2. The audio clock must be microseconds

After the timer fix, SQ ran at 80% speed with `samples_ready` pinned at 0 — the encoder
was starving. The demo derives `num_samples` from `btstack_run_loop_get_time_ms()`,
which truncates a ~2.5 ms interval to 2 ms and silently loses 20% of the audio clock.

**Fix:** pace production with `QueryPerformanceCounter` (`ldac_now_us()`), accumulating
frames owed and encoding only when ≥ 128 are due. Never derive audio timing from a
millisecond clock.

## 3. Never discard an encoded LDAC packet

**Symptom:** crackle at HQ, more at higher bitrates.

When `a2dp_source_stream_send_media_payload_rtp()` returned non-zero (link busy), the
old code `break`ed and threw the encoded packet away — 3–10% of the audio, at random.

**Fix:** copy the packet into `ldac_pending[]`, keep its RTP timestamp, and retry on the
next tick before encoding anything new. Refusals must apply back-pressure, not loss.
This is what turned "sounds good but crackles" into "sounds good".

## 4. `LE Rand` on a BR/EDR-only controller wedges the stack

Sending LE commands to a controller without LE support (LMP feature bit 38 clear — e.g.
an old CSR/ISSC dongle) leaves BTstack waiting forever for `Command Complete`; every
subsequent command fails and nothing connects.

**Fix:** gate `hci_run_general_gap_le()` on the LMP feature bit
(`local_supported_features[4] & 0x40`) and disable `ENABLE_LE_*` in `btstack_config.h`
for classic-only builds.

## 5. `hci_send_cmd` refuses inside an event callback

`hci.c: hci_send_cmd called but cannot send packet now` — an authentication request
issued from inside a `HCI_EVENT_CONNECTION_COMPLETE` handler. The controller has no free
command slot at that instant.

**Fix:** set a pending flag and retry from a 100 ms `btstack_timer_source_t` until the
command is accepted.

## 6. A2DP never opens because nobody authenticates

BTstack registers the AVDTP PSM at `gap_get_security_level()`, which defaults to 0, so
no `Authentication Requested` is ever sent. Sony headphones silently drop the link
(`L2CAP_CONNECTION_BASEBAND_DISCONNECT`, 0x6a) rather than refusing visibly.

**Fix:** pair properly (`SSP` just-works) and let the link key persist; a stale Windows
link key in BTstack's store yields `0x05 Authentication Failure` and must be deleted so
the device re-pairs.

## 7. Pre-SSP dongles cannot work

A BT 2.0+EDR adapter without Secure Simple Pairing answers legacy pairing with
`0x18 PAIRING_NOT_ALLOWED` on modern sinks. No patch fixes this — use a BT 4.0+ radio
(Intel AX2xx combo radios work well).

## 8. Build pitfalls

- **Header dependency tracking is absent.** `btstack_config.h` changes do not trigger a
  rebuild; stale objects silently keep old features. Always `rm -f *.o *.exe`.
- **New example targets do not link** (`0` objects on the link line). Build through the
  `a2dp_source_demo` target and copy your source over `example/a2dp_source_demo.c`.
- **`-I` path is relative to `BTSTACK_ROOT`.** The Makefile expects libldac as a *sibling*
  of the btstack clone (`../libldac`).
- **Windows headers are case-insensitive, mingw on Linux is not.** BTstack includes
  `<windows.h>`, `<winusb.h>`, `<setupapi.h>` etc.; a shim directory of case-variant
  symlinks to the mingw headers is required. `build/build-windows.sh` generates it.
- **The link-key store is CWD-relative** (`btstack_<MAC>.tlv`). Launch from a fixed
  directory or pairing is lost every run.

## 9. Measurement commands that proved the above

```bash
# packet rate
grep -c "packets sent" log.txt
# HCI completion cadence
tshark -r hci_dump.pklg -Y 'bthci_cmd.opcode == 0x040c' -T fields -e frame.time_delta
# what the host actually emits
tshark -r hci_dump.pklg -Y 'bthci_acl' -T fields -e frame.time_relative
```

If a future change reintroduces a rate cap, measure packets/s **and** the audio clock's
owed-frame backlog (`owed`) before theorising about the radio.

## 10. WASAPI float32 is ±1.0, not int16 scale

**Symptom:** capture ran, delivered real frames at real-time, and every sample was
zero. All five render endpoints on the test machine looked "silent", which invited a
hardware explanation — wrong.

WASAPI shared-mode mixes are almost always `WAVE_FORMAT_IEEE_FLOAT` 32-bit, whose
samples are nominal ±1.0. The conversion path passed those straight into a clipper
expecting int16 scale (`±32768`), so everything truncated to `0`.

**Fix:** scale float samples by 32768 before clipping. **Lesson:** when capture
returns zeros, unit-test the conversion with synthetic input *before* suspecting the
device — a three-line feed test proved it in seconds after an hour of endpoint
theorising.

## 11. Test capture without depending on what is playing

`wa_test.exe` gained a `selftest <endpoint>` mode that renders a tone into an
endpoint and captures it back in loopback. That single tool separates:

- capture broken → `no audio captured`
- nothing playing → `no audio captured` (but `selftest` says `LOOPBACK WORKS`)

Never debug loopback against "whatever the user happens to be playing"; render your
own known signal on the same endpoint.

## 12. Endpoints: shared mode forces the device's format, and virtual devices lie

- In shared mode `IAudioClient::Initialize` only accepts the endpoint's **mix format**;
  anything else fails. Read it with `GetMixFormat` and convert (rate, channel count,
  float/int). Endpoints commonly run 48 kHz or 192 kHz float32.
- Loopback on a **virtual or disconnected** endpoint (Steam Streaming, Oculus, an HDMI
  sink with the TV off) returns digital silence even though capture "works". A default
  endpoint can be one of these, so capturing "the default" is not the same as
  capturing "what the user hears".
- The reliable way to pick is `IAudioSessionManager2` → session enumerator → any
  session in `AudioSessionStateActive`. Verified: with a tone playing on the HDMI
  endpoint, the tool selected `LG TV SSCR2 (192000 Hz)` with no endpoint argument.
- Re-evaluate after a few seconds of silence, otherwise a device switch leaves you
  capturing a dead endpoint forever.

## 13. `ldac_ABR_Proc` needs the initialised handle, not a fresh one

Calling `ldac_ABR_get_handle()` inline per packet and passing the result to
`ldac_ABR_Proc` produced an immediate `0xc0000005` on the first send — the handle is
uninitialised. Store the handle created at `ldac_ABR_Init` time and reuse it.

## 14. Console children die with their console

`Start-Process` from a remote PowerShell session exits the child as soon as the
parent's console is torn down, which looks exactly like a crash and logs nothing.
Launch long-running test processes with WMI instead:

```powershell
Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine="cmd.exe /c `"$bat`""}
```

## 15. Audio sinks sleep

A paired headset that idles goes unreachable (`0x04 PAGE_TIMEOUT`) and every test then
fails for a reason unrelated to the code. Wake it before each run; a single press of
the power button reconnects without re-pairing.
