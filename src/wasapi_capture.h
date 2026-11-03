/*
 * wasapi_capture.h — system-audio capture (WASAPI loopback) for win-ldac-a2dp
 *
 * Captures the default render endpoint in loopback, converts to the format the
 * LDAC encoder wants (48 kHz, 16-bit, interleaved stereo) and exposes it through
 * a small ring buffer.
 *
 * The A2DP stream must never starve: when nothing is playing, reads return
 * digital silence rather than blocking, so the sink does not time out.
 */
#ifndef WASAPI_CAPTURE_H
#define WASAPI_CAPTURE_H

#include <stdint.h>

/* Starts the capture thread. Returns 0 on success, negative on failure.
 * The capture thread is a *producer*; it never calls into BTstack. */
int wa_capture_start(void);

/* Same, but for a specific render endpoint (index from wa_capture_list()). */
int wa_capture_start_ex(int endpoint_index);

/* Same, but matches a substring of the endpoint's friendly name (e.g. "LG TV"). */
int wa_capture_start_named(const char *name_substring);

/* Captures whichever render endpoint currently has an active audio session,
 * following it if it changes. Falls back to the default endpoint. */
int wa_capture_start_auto(void);

/* Prints active render endpoints as "[i] name" and returns how many. */
int wa_capture_list(void);

/* Plays a sine to the given endpoint for verification purposes.
 * Returns 0 on success. Used by --selftest. */
int wa_render_tone(int endpoint_index, int seconds, double freq, double amp);

/* Stops the capture thread and releases COM objects. Safe to call twice. */
void wa_capture_stop(void);

/* Reads up to `frames` stereo frames into dst (interleaved, 48 kHz S16).
 * Always fills the whole buffer: missing audio becomes silence.
 * Returns the number of real (non-silent-filler) frames copied. */
int wa_capture_read(int16_t *dst, int frames);

/* Diagnostics for the status line. Any pointer may be NULL. */
void wa_capture_stats(unsigned int *underruns, unsigned int *captured_frames,
                      unsigned int *source_rate, unsigned int *source_channels,
                      unsigned int *silent_packets, unsigned int *peak_sample,
                      char *source_name, int max_chars);

#endif /* WASAPI_CAPTURE_H */
