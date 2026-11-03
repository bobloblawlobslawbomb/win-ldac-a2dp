/*
 * wasapi_capture.c — WASAPI loopback capture for win-ldac-a2dp
 *
 * Design notes (these matter):
 *  - Shared-mode WASAPI only accepts the endpoint's *mix format*, so we take
 *    whatever the device offers and convert: float32/int16/int32 -> S16,
 *    N channels -> 2, and resample to 48 kHz with a stateful linear
 *    interpolator if the endpoint runs at 44.1 kHz.
 *  - The capture thread is a producer only. It never touches BTstack, so a
 *    stalled audio device cannot wedge the Bluetooth run loop.
 *  - Underruns produce silence, never a stall: an A2DP sink that receives
 *    nothing for a second or two drops the stream.
 */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wchar.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "wasapi_capture.h"


/* mingw does not export these GUIDs from libuuid.a, so define them here. */
static const GUID guid_mmdevice_enum =
    {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};  /* CLSID_MMDeviceEnumerator */
static const GUID guid_immdevice_enum =
    {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};  /* IID_IMMDeviceEnumerator */
static const GUID guid_iaudioclient =
    {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};  /* IID_IAudioClient */
static const PROPERTYKEY pkey_friendly_name =
    {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};  /* PKEY_Device_FriendlyName */
static const GUID guid_iaudiorenderclient =
    {0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};  /* IID_IAudioRenderClient */
static const GUID guid_iaudiocaptureclient =
    {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};  /* IID_IAudioCaptureClient */

#define TARGET_RATE      48000
#define TARGET_CHANNELS  2
#define RING_FRAMES      (TARGET_RATE * 2)      /* 2 s of stereo audio */

static CRITICAL_SECTION wa_lock;
static int16_t          wa_ring[RING_FRAMES * TARGET_CHANNELS];
static volatile LONG    wa_rpos;                /* producer index (frames) */
static volatile LONG    wa_wpos;                /* consumer index (frames) */
static volatile LONG    wa_ready;
static volatile LONG    wa_stop;
static HANDLE           wa_thread;
static HANDLE           wa_started_evt;

static volatile LONG    wa_peaksample;
static volatile LONG    wa_silent_pkts;
static unsigned int     wa_underruns;
static unsigned int     wa_captured;
static unsigned int     wa_src_rate;
static unsigned int     wa_src_channels;

/* --- resampler state (producer side only) ------------------------------- */
static int              wa_endpoint_index = -1;  /* -1 = default render endpoint */
static char             wa_endpoint_match[128];  /* non-empty: match by name instead */
static wchar_t          wa_endpoint_name[256] = L"(unknown)";
static int              wa_fmt_channels = 2;    /* decided at open time */
static int              wa_fmt_bits     = 16;
static int              wa_fmt_float    = 0;
static unsigned int     wa_fmt_rate     = TARGET_RATE;

static double           rs_pos;                 /* fractional read position */
static int16_t          rs_prev[TARGET_CHANNELS];
static int              rs_have_prev;

/* ------------------------------------------------------------------------ */
/* ring buffer (SPSC guarded by a critical section: cheap, and correct)      */
/* ------------------------------------------------------------------------ */
static void ring_push(const int16_t *src, int frames)
{
    int i;
    EnterCriticalSection(&wa_lock);
    for (i = 0; i < frames; i++) {
        LONG w = wa_wpos;
        LONG r = wa_rpos;
        if ((w + 1) % RING_FRAMES == r) break;      /* full: drop newest */
        wa_ring[(w % RING_FRAMES) * TARGET_CHANNELS + 0] = src[i * TARGET_CHANNELS + 0];
        wa_ring[(w % RING_FRAMES) * TARGET_CHANNELS + 1] = src[i * TARGET_CHANNELS + 1];
        wa_wpos = (w + 1) % RING_FRAMES;
    }
    LeaveCriticalSection(&wa_lock);
}

int wa_capture_read(int16_t *dst, int frames)
{
    int got = 0;
    if (!dst || frames <= 0) return 0;

    if (!wa_ready) {
        memset(dst, 0, (size_t)frames * TARGET_CHANNELS * sizeof(int16_t));
        return 0;
    }

    EnterCriticalSection(&wa_lock);
    while (got < frames) {
        LONG r = wa_rpos;
        LONG w = wa_wpos;
        if (r == w) break;
        dst[got * TARGET_CHANNELS + 0] = wa_ring[(r % RING_FRAMES) * TARGET_CHANNELS + 0];
        dst[got * TARGET_CHANNELS + 1] = wa_ring[(r % RING_FRAMES) * TARGET_CHANNELS + 1];
        wa_rpos = (r + 1) % RING_FRAMES;
        got++;
    }
    LeaveCriticalSection(&wa_lock);

    if (got < frames) {
        memset(dst + (size_t)got * TARGET_CHANNELS, 0,
               (size_t)(frames - got) * TARGET_CHANNELS * sizeof(int16_t));
        InterlockedIncrement((volatile LONG *)&wa_underruns);
    }
    InterlockedExchangeAdd((volatile LONG *)&wa_captured, got);
    return got;
}

/* ------------------------------------------------------------------------ */
/* format conversion                                                         */
/* ------------------------------------------------------------------------ */
static int16_t clip16(double v)
{
    if (v > 32767.0)  return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)v;
}

static double sample_at(const BYTE *base, int frame, int ch, int channels,
                        int bits, int is_float)
{
    if (is_float) {
        /* WASAPI float is nominal +/-1.0; clip16() expects int16 scale. */
        const float *p = (const float *)base;
        return (double)p[frame * channels + ch] * 32768.0;
    }
    if (bits == 16) {
        const int16_t *p = (const int16_t *)base;
        return (double)p[frame * channels + ch];
    }
    if (bits == 32) {
        const int32_t *p = (const int32_t *)base;
        return (double)p[frame * channels + ch] / 65536.0;   /* 32 -> 16 bit */
    }
    if (bits == 24) {
        const BYTE *p = base + ((size_t)frame * channels + ch) * 3;
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
        if (v & 0x800000) v |= (int32_t)0xFF000000;
        return (double)(v >> 8);
    }
    return 0.0;
}

/* Convert one captured buffer to stereo S16 at TARGET_RATE and push it. */
static void convert_and_push(const BYTE *data, UINT32 frames, int channels,
                             int bits, int is_float, unsigned int rate)
{
    LONG local_peak = 0;
    int16_t tmp[4096 * TARGET_CHANNELS];
    int out = 0;
    UINT32 i;

    if (channels < 1) channels = 1;

    for (i = 0; i < frames; i++) {
        double l = sample_at(data, (int)i, 0, channels, bits, is_float);
        double r = (channels > 1) ? sample_at(data, (int)i, 1, channels, bits, is_float) : l;
        int16_t sl = clip16(l), sr = clip16(r);

        if (rate == TARGET_RATE) {
            tmp[out * 2 + 0] = sl;
            tmp[out * 2 + 1] = sr;
            out++;
        } else {
            /* Stateful linear resampler: step through the input by rate/48000. */
            double step = (double)rate / (double)TARGET_RATE;
            int16_t prev_l = rs_have_prev ? rs_prev[0] : sl;
            int16_t prev_r = rs_have_prev ? rs_prev[1] : sr;
            rs_pos += 1.0;
            while (rs_pos >= step && out < (int)(sizeof(tmp) / (2 * sizeof(int16_t)))) {
                double frac = 1.0 - (rs_pos - step) / step;
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
                tmp[out * 2 + 0] = clip16(prev_l + (sl - prev_l) * frac);
                tmp[out * 2 + 1] = clip16(prev_r + (sr - prev_r) * frac);
                out++;
                rs_pos -= step;
            }
            rs_prev[0] = sl;
            rs_prev[1] = sr;
            rs_have_prev = 1;
        }

        {
            int a = (sl < 0) ? -sl : sl;
            int b = (sr < 0) ? -sr : sr;
            if (a > local_peak) local_peak = a;
            if (b > local_peak) local_peak = b;
        }

        if (out >= (int)(sizeof(tmp) / (2 * sizeof(int16_t)))) {
            ring_push(tmp, out);
            out = 0;
        }
    }
    if (out > 0) ring_push(tmp, out);
    if (local_peak > (LONG)wa_peaksample) InterlockedExchange(&wa_peaksample, local_peak);
}

/* ------------------------------------------------------------------------ */
/* capture thread                                                            */
/* ------------------------------------------------------------------------ */
static void device_name(IMMDevice *device, wchar_t *out, size_t out_chars)
{
    IPropertyStore *store = NULL;
    PROPVARIANT     pv;
    PropVariantInit(&pv);
    out[0] = 0;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store)) && store) {
        if (SUCCEEDED(IPropertyStore_GetValue(store, &pkey_friendly_name, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal) {
            wcsncpy(out, pv.pwszVal, out_chars - 1);
            out[out_chars - 1] = 0;
        }
        IPropertyStore_Release(store);
    }
    PropVariantClear(&pv);
}

int wa_capture_list(void)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection *collection = NULL;
    HRESULT hr;
    UINT32 count = 0, i;
    int printed = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return -1;

    hr = CoCreateInstance(&guid_mmdevice_enum, NULL, CLSCTX_ALL,
                          &guid_immdevice_enum, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) { fprintf(stderr, "WA: enumerator failed\n"); return -1; }

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) { IMMDeviceEnumerator_Release(enumerator); return -1; }

    IMMDeviceCollection_GetCount(collection, &count);
    for (i = 0; i < count; i++) {
        IMMDevice *dev = NULL;
        if (SUCCEEDED(IMMDeviceCollection_Item(collection, i, &dev)) && dev) {
            wchar_t name[256];
            device_name(dev, name, sizeof(name) / sizeof(name[0]));
            printf("[%u] %ls\n", i, name);
            printed++;
            IMMDevice_Release(dev);
        }
    }
    IMMDeviceCollection_Release(collection);
    IMMDeviceEnumerator_Release(enumerator);
    return printed;
}

static int name_matches(const char *haystack, const char *needle)
{
    size_t nl = strlen(needle), hl = strlen(haystack), i, j;
    if (nl == 0 || nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static HRESULT select_device(IMMDeviceEnumerator *enumerator, IMMDevice **device_out)
{
    if (wa_endpoint_match[0]) {
        /* select by (case-insensitive) substring of the friendly name */
        IMMDeviceCollection *collection = NULL;
        UINT32 count = 0, i;
        HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender,
                                                            DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr) || !collection) return hr;
        IMMDeviceCollection_GetCount(collection, &count);
        for (i = 0; i < count; i++) {
            IMMDevice *dev = NULL;
            if (SUCCEEDED(IMMDeviceCollection_Item(collection, i, &dev)) && dev) {
                wchar_t wname[256];
                char    aname[256];
                int k = 0;
                device_name(dev, wname, sizeof(wname) / sizeof(wname[0]));
                while (wname[k] && k < (int)sizeof(aname) - 1) { aname[k] = (char)wname[k]; k++; }
                aname[k] = 0;
                if (name_matches(aname, wa_endpoint_match)) {
                    *device_out = dev;
                    IMMDeviceCollection_Release(collection);
                    return S_OK;
                }
                IMMDevice_Release(dev);
            }
        }
        IMMDeviceCollection_Release(collection);
        return E_NOTFOUND;
    }
    if (wa_endpoint_index < 0) {
        return IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, device_out);
    } else {
        IMMDeviceCollection *collection = NULL;
        HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender,
                                                            DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr) || !collection) return hr;
        hr = IMMDeviceCollection_Item(collection, (UINT32)wa_endpoint_index, device_out);
        IMMDeviceCollection_Release(collection);
        return hr;
    }
}

static int open_capture(IAudioClient **client_out, IAudioCaptureClient **cap_out)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice           *device = NULL;
    IAudioClient        *client = NULL;
    IAudioCaptureClient *capture = NULL;
    WAVEFORMATEX        *fmt = NULL;
    WAVEFORMATEXTENSIBLE *ext;
    REFERENCE_TIME       dur = 2000000;      /* 200 ms buffer, 100-ns units */
    HRESULT hr;
    int ok = -1;
    int channels, bits, is_float;
    unsigned int rate;

    hr = CoCreateInstance(&guid_mmdevice_enum, NULL, CLSCTX_ALL,
                          &guid_immdevice_enum, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) { fprintf(stderr, "WA: no device enumerator (0x%08lx)\n", (unsigned long)hr); goto done; }

    hr = select_device(enumerator, &device);
    if (FAILED(hr) || !device) { fprintf(stderr, "WA: cannot open render endpoint %d (0x%08lx)\n", wa_endpoint_index, (unsigned long)hr); goto done; }
    device_name(device, wa_endpoint_name, sizeof(wa_endpoint_name) / sizeof(wa_endpoint_name[0]));

    hr = IMMDevice_Activate(device, &guid_iaudioclient, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr) || !client) { fprintf(stderr, "WA: activate failed (0x%08lx)\n", (unsigned long)hr); goto done; }

    hr = IAudioClient_GetMixFormat(client, &fmt);
    if (FAILED(hr) || !fmt) { fprintf(stderr, "WA: GetMixFormat failed (0x%08lx)\n", (unsigned long)hr); goto done; }

    rate     = fmt->nSamplesPerSec;
    channels = fmt->nChannels;
    bits     = fmt->wBitsPerSample;
    is_float = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);

    ext = (WAVEFORMATEXTENSIBLE *)fmt;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        /* SubFormat Data1 carries the equivalent wFormatTag; avoids GUID symbols */
        if (ext->SubFormat.Data1 == 0x00000003) is_float = 1;
    }

    fprintf(stderr, "WA: endpoint '%ls' mix format %u Hz, %d ch, %d bit, %s\n",
            wa_endpoint_name, rate, channels, bits, is_float ? "float" : "pcm");

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 dur, 0, fmt, NULL);
    if (FAILED(hr)) { fprintf(stderr, "WA: Initialize failed (0x%08lx)\n", (unsigned long)hr); goto done; }

    hr = IAudioClient_GetService(client, &guid_iaudiocaptureclient, (void **)&capture);
    if (FAILED(hr) || !capture) { fprintf(stderr, "WA: GetService failed (0x%08lx)\n", (unsigned long)hr); goto done; }

    hr = IAudioClient_Start(client);
    if (FAILED(hr)) { fprintf(stderr, "WA: Start failed (0x%08lx)\n", (unsigned long)hr); goto done; }

    wa_src_rate     = rate;
    wa_src_channels = (unsigned int)channels;

    /* Stash the decoded format where the thread can reach it. */
    *client_out = client;
    *cap_out    = capture;

    /* Keep the format flags in globals used by the thread. */
    wa_fmt_channels = channels;
    wa_fmt_bits     = bits;
    wa_fmt_float    = is_float;
    wa_fmt_rate     = rate;
    ok = 0;

done:
    if (fmt) CoTaskMemFree(fmt);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (ok != 0) {
        if (capture) IAudioCaptureClient_Release(capture);
        if (client) IAudioClient_Release(client);
    }
    return ok;
}

static DWORD WINAPI capture_thread(LPVOID param)
{
    IAudioClient        *client = NULL;
    IAudioCaptureClient *capture = NULL;
    HRESULT hr;
    (void)param;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "WA: CoInitializeEx failed (0x%08lx)\n", (unsigned long)hr);
        SetEvent(wa_started_evt);
        return 1;
    }

    if (open_capture(&client, &capture) != 0) {
        SetEvent(wa_started_evt);
        return 1;
    }

    InterlockedExchange(&wa_ready, 1);
    SetEvent(wa_started_evt);

    while (!wa_stop) {
        UINT32 packet = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture, &packet))) break;
        if (packet == 0) { Sleep(4); continue; }

        while (packet > 0) {
            BYTE  *data = NULL;
            UINT32 frames = 0;
            DWORD  flags = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) break;
            if (frames > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    InterlockedIncrement(&wa_silent_pkts);
                    int16_t silence[512 * 2];
                    UINT32 off = 0;
                    memset(silence, 0, sizeof(silence));
                    while (off < frames) {
                        UINT32 n = frames - off; if (n > 512) n = 512;
                        ring_push(silence, (int)n);
                        off += n;
                    }
                } else {
                    convert_and_push(data, frames, wa_fmt_channels, wa_fmt_bits,
                                     wa_fmt_float, wa_fmt_rate);
                }
            }
            IAudioCaptureClient_ReleaseBuffer(capture, frames);
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture, &packet))) { packet = 0; break; }
        }
    }

    IAudioClient_Stop(client);
    IAudioCaptureClient_Release(capture);
    IAudioClient_Release(client);
    CoUninitialize();
    return 0;
}


/* ------------------------------------------------------------------------ */
/* render helper (used by the self-test and by --selftest)                   */
/* ------------------------------------------------------------------------ */
static void fill_tone(BYTE *buf, UINT32 frames, int channels, int bits, int is_float,
                      double *phase, double freq, double amp, unsigned int rate)
{
    UINT32 i; int ch;
    for (i = 0; i < frames; i++) {
        double v = amp * sin(*phase);
        *phase += 2.0 * 3.14159265358979323846 * freq / (double)rate;
        if (*phase > 2.0 * 3.14159265358979323846) *phase -= 2.0 * 3.14159265358979323846;
        for (ch = 0; ch < channels; ch++) {
            if (is_float) {
                ((float *)buf)[(size_t)i * channels + ch] = (float)v;
            } else if (bits == 16) {
                ((int16_t *)buf)[(size_t)i * channels + ch] = (int16_t)(v * 32767.0);
            } else if (bits == 32) {
                ((int32_t *)buf)[(size_t)i * channels + ch] = (int32_t)(v * 2147483647.0);
            }
        }
    }
}

int wa_render_tone(int endpoint_index, int seconds, double freq, double amp)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice           *device = NULL;
    IAudioClient        *client = NULL;
    IAudioRenderClient  *render = NULL;
    WAVEFORMATEX        *fmt = NULL;
    WAVEFORMATEXTENSIBLE *ext;
    UINT32  buffer_frames = 0;
    HRESULT hr;
    int channels, bits, is_float, ok = -1;
    unsigned int rate;
    double phase = 0.0;
    DWORD start;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { fprintf(stderr, "RT: CoInitializeEx 0x%08lx\n", (unsigned long)hr); return -1; }

    hr = CoCreateInstance(&guid_mmdevice_enum, NULL, CLSCTX_ALL, &guid_immdevice_enum, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) { fprintf(stderr, "RT: CoCreateInstance 0x%08lx\n", (unsigned long)hr); goto done; }
    hr = select_device(enumerator, &device);
    if (FAILED(hr) || !device) { fprintf(stderr, "RT: select_device 0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IMMDevice_Activate(device, &guid_iaudioclient, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr) || !client) { fprintf(stderr, "RT: Activate 0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IAudioClient_GetMixFormat(client, &fmt);
    if (FAILED(hr) || !fmt) { fprintf(stderr, "RT: GetMixFormat 0x%08lx\n", (unsigned long)hr); goto done; }

    rate = fmt->nSamplesPerSec; channels = fmt->nChannels; bits = fmt->wBitsPerSample;
    is_float = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    ext = (WAVEFORMATEXTENSIBLE *)fmt;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22 && ext->SubFormat.Data1 == 0x00000003)
        is_float = 1;

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, fmt, NULL);
    if (FAILED(hr)) { fprintf(stderr, "RT: Initialize 0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IAudioClient_GetService(client, &guid_iaudiorenderclient, (void **)&render);
    if (FAILED(hr) || !render) { fprintf(stderr, "RT: GetService render 0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IAudioClient_GetBufferSize(client, &buffer_frames);
    if (FAILED(hr)) { fprintf(stderr, "RT: GetBufferSize 0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) { fprintf(stderr, "RT: Start 0x%08lx\n", (unsigned long)hr); goto done; }
    fprintf(stderr, "RT: rendering %d s, %u Hz %d ch %d bit %s, buffer %u frames\n",
            seconds, rate, channels, bits, is_float ? "float" : "pcm", buffer_frames);

    start = GetTickCount();
    while ((GetTickCount() - start) < (DWORD)(seconds * 1000)) {
        UINT32 pad = 0, avail;
        BYTE *data = NULL;
        if (FAILED(IAudioClient_GetCurrentPadding(client, &pad))) break;
        avail = buffer_frames - pad;
        if (avail == 0) { Sleep(5); continue; }
        if (FAILED(IAudioRenderClient_GetBuffer(render, avail, &data))) break;
        fill_tone(data, avail, channels, bits, is_float, &phase, freq, amp, rate);
        IAudioRenderClient_ReleaseBuffer(render, avail, 0);
        Sleep(10);
    }
    IAudioClient_Stop(client);
    fprintf(stderr, "RT: rendered %d s of tone\n", seconds);
    ok = 0;

done:
    if (fmt) CoTaskMemFree(fmt);
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* public API                                                                */
/* ------------------------------------------------------------------------ */
int wa_capture_start_ex(int endpoint_index)
{
    wa_endpoint_index = endpoint_index;
    wa_endpoint_match[0] = 0;
    return wa_capture_start();
}

int wa_capture_start_named(const char *name_substring)
{
    if (!name_substring || !*name_substring) return wa_capture_start_ex(-1);
    strncpy(wa_endpoint_match, name_substring, sizeof(wa_endpoint_match) - 1);
    wa_endpoint_match[sizeof(wa_endpoint_match) - 1] = 0;
    wa_endpoint_index = -2;   /* "match by name" */
    return wa_capture_start();
}

int wa_capture_start(void)
{
    InitializeCriticalSection(&wa_lock);
    wa_rpos = wa_wpos = 0;
    wa_ready = 0;
    wa_stop = 0;
    rs_pos = 0.0;
    rs_have_prev = 0;

    wa_started_evt = CreateEvent(NULL, TRUE, FALSE, NULL);
    wa_thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!wa_thread) return -1;

    WaitForSingleObject(wa_started_evt, 3000);
    return wa_ready ? 0 : -2;
}

void wa_capture_stop(void)
{
    if (!wa_thread) return;
    InterlockedExchange(&wa_stop, 1);
    WaitForSingleObject(wa_thread, 3000);
    CloseHandle(wa_thread);
    CloseHandle(wa_started_evt);
    wa_thread = NULL;
    wa_ready = 0;
}

void wa_capture_stats(unsigned int *underruns, unsigned int *captured_frames,
                      unsigned int *source_rate, unsigned int *source_channels,
                      unsigned int *silent_packets, unsigned int *peak_sample,
                      char *source_name, int max_chars)
{
    if (underruns)       *underruns = wa_underruns;
    if (captured_frames) *captured_frames = wa_captured;
    if (source_rate)     *source_rate = wa_src_rate;
    if (source_channels) *source_channels = wa_src_channels;
    if (source_name)     { int i = 0; while (wa_endpoint_name[i] && i < (int)max_chars - 1) { source_name[i] = (char)wa_endpoint_name[i]; i++; } source_name[i] = 0; }
    if (silent_packets)  *silent_packets = wa_silent_pkts;
    if (peak_sample)     *peak_sample = (unsigned int)wa_peaksample;
}

#ifdef WA_TEST_HOOK
/* Test hooks: feed synthetic data through the real conversion path. */
void wa_test_feed(const void *data, unsigned int frames, int channels, int bits,
                  int is_float, unsigned int rate)
{
    convert_and_push((const BYTE *)data, (UINT32)frames, channels, bits, is_float, rate);
}
void wa_test_reset(void)
{
    InitializeCriticalSection(&wa_lock);
    wa_rpos = wa_wpos = 0;
    wa_ready = 1;
}
#endif
