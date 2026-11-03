/*
 * wa_test.c — standalone check that WASAPI loopback capture works.
 *
 * Build (cross):
 *   x86_64-w64-mingw32-gcc -O2 -o wa_test.exe tools/wa_test.c src/wasapi_capture.c \
 *       -lole32 -I src
 *
 * Run it while something is playing, then while everything is silent: the RMS
 * should be clearly non-zero in the first case and zero in the second.
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "wasapi_capture.h"

#ifdef WA_TEST_HOOK
void wa_test_feed(const void *data, unsigned int frames, int channels, int bits,
                  int is_float, unsigned int rate);
void wa_test_reset(void);
#endif

typedef struct { int ep; int secs; } render_arg;

static DWORD WINAPI render_thread(LPVOID p)
{
    render_arg *a = (render_arg *)p;
    fprintf(stderr, "RT rc=%d\n", wa_render_tone(a->ep, a->secs, 440.0, 0.05));
    return 0;
}

int main(int argc, char **argv)
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 3;
    int endpoint = (argc > 2) ? atoi(argv[2]) : -1;   /* -1 = default */
    int16_t buf[1024 * 2];
    long    total = 0, real = 0;
    double  sumsq = 0.0;
    int16_t peak = 0;
    DWORD   start;
    unsigned int underruns = 0, captured = 0, rate = 0, channels = 0, silent_pkts = 0, peak_in = 0;
    char src_name[128] = "(none)";

    if (argc > 1 && !strcmp(argv[1], "list")) { wa_capture_list(); return 0; }

    if (argc > 1 && !strcmp(argv[1], "play")) {
        /* Render a quiet tone to an endpoint: used to feed the LDAC pipeline. */
        int ep = (argc > 2) ? atoi(argv[2]) : -1;
        int secs = (argc > 3) ? atoi(argv[3]) : 10;
        double amp = (argc > 4) ? atof(argv[4]) : 0.02;
        printf("playing %d s tone to endpoint %d at %.0f%%\n", secs, ep, amp * 100.0);
        return wa_render_tone(ep, secs, 440.0, amp);
    }

    if (argc > 1 && !strcmp(argv[1], "conv")) {
        /* Feed a known sine through convert_and_push and read it back. */
        static float src[480 * 2];
        int16_t b[480 * 2];
        int i, got; long n = 0; double sumsq = 0.0; int16_t peak = 0;
        for (i = 0; i < 480; i++) {
            float v = (float)(0.5 * sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
            src[i * 2 + 0] = v; src[i * 2 + 1] = v;
        }
        wa_test_reset();
        wa_test_feed(src, 480, 2, 32, 1, 48000);
        while ((got = wa_capture_read(b, 480)) > 0) {
            n += got;
            for (i = 0; i < got * 2; i++) { sumsq += (double)b[i] * (double)b[i]; if (b[i] > peak) peak = b[i]; }
        }
        printf("conv: frames %ld peak %d rms %.1f\n", n, (int)peak,
               n ? sqrt(sumsq / (double)(n * 2)) : 0.0);
        printf((n > 0 && peak > 1000) ? "RESULT: CONVERSION OK\n" : "RESULT: CONVERSION BROKEN\n");
        return 0;
    }

    if (argc > 1 && !strcmp(argv[1], "selftest")) {
        /* Render a tone to endpoint N (main thread) while the capture thread
           reads it back in loopback. No extra threads: deterministic. */
        int ep = (argc > 2) ? atoi(argv[2]) : -1;
        int16_t b2[1024 * 2];
        long n = 0; double sumsq = 0.0; int i, got, rrc;
        unsigned int u2 = 0, c2 = 0, r2 = 0, ch2 = 0, s2 = 0, pk2 = 0;
        char nm[128] = "(none)";

        if (wa_capture_start_ex(ep) != 0) { printf("selftest: capture failed\n"); return 1; }
        rrc = wa_render_tone(ep, 2, 440.0, 0.15);
        printf("render rc=%d\n", rrc);

        while ((got = wa_capture_read(b2, 1024)) > 0) {
            n += got;
            for (i = 0; i < got * 2; i++) sumsq += (double)b2[i] * (double)b2[i];
        }
        wa_capture_stop();
        wa_capture_stats(&u2, &c2, &r2, &ch2, &s2, &pk2, nm, sizeof(nm));
        printf("endpoint %d '%s' %u Hz %u ch\n", ep, nm, r2, ch2);
        printf("frames %ld measured, peak %u, silent-flagged %u, underruns %u\n", n, pk2, s2, u2);
        printf("rms %.1f\n", n ? sqrt(sumsq / (double)(n * 2)) : 0.0);
        printf(n && sqrt(sumsq / (double)(n * 2)) > 100.0 ? "RESULT: LOOPBACK WORKS\n"
                                                          : "RESULT: no audio captured\n");
        return 0;
    }

    printf("starting capture for %d s on endpoint %d...\n", seconds, endpoint);
    fflush(stdout);
    if (wa_capture_start_ex(endpoint) != 0) {
        printf("RESULT: FAILED to start capture\n");
        return 1;
    }

    start = GetTickCount();
    while ((GetTickCount() - start) < (DWORD)(seconds * 1000)) {
        int got = wa_capture_read(buf, 1024);
        int i;
        total += 1024;
        real  += got;
        for (i = 0; i < 1024 * 2; i++) {
            double v = (double)buf[i];
            sumsq += v * v;
            if (buf[i] > peak) peak = buf[i];
            if (-buf[i] > peak) peak = (int16_t)-buf[i];
        }
        Sleep(20);
    }
    wa_capture_stop();

    wa_capture_stats(&underruns, &captured, &rate, &channels, &silent_pkts, &peak_in, src_name, sizeof(src_name));

    {
        double rms = (total > 0) ? sqrt(sumsq / (double)(total * 2)) : 0.0;
        printf("source: '%s' %u Hz, %u ch\n", src_name, rate, channels);
        printf("frames: requested %ld, captured %ld (%.1f%%)\n",
               total, real, total ? 100.0 * (double)real / (double)total : 0.0);
        printf("underrun events: %u   silent-flagged packets: %u   source peak: %u\n", underruns, silent_pkts, peak_in);
        printf("rms: %.1f   peak: %d\n", rms, (int)peak);
        if (rms < 1.0) printf("RESULT: SILENT (nothing playing, or capture broken)\n");
        else           printf("RESULT: AUDIO DETECTED\n");
    }
    return 0;
}
