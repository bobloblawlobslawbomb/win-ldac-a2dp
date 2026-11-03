/*
 * Copyright (C) 2016 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "a2dp_source_demo.c"

/*
 * a2dp_source_demo.c
 */

// *****************************************************************************
/* EXAMPLE_START(a2dp_source_demo): A2DP Source - Stream Audio and Control Volume
 *
 * @text This A2DP Source example demonstrates how to send an audio data stream 
 * to a remote A2DP Sink device and how to switch between two audio data sources.  
 * In addition, the AVRCP Target is used to answer queries on currently played media,
 * as well as to handle remote playback control, i.e. play, stop, repeat, etc. If HAVE_BTSTACK_STDIN 
 * is set, press SPACE on the console to show the available AVDTP and AVRCP commands.
 *
 * @text To test with a remote device, e.g. a Bluetooth speaker,
 * set the device_addr_string to the Bluetooth address of your 
 * remote device in the code, and use the UI to connect and start playback. 
 * 
 * @text For more info on BTstack audio, see our blog post 
 * [A2DP Sink and Source on STM32 F4 Discovery Board](http://bluekitchen-gmbh.com/a2dp-sink-and-source-on-stm32-f4-discovery-board/).
 * 
 */
// *****************************************************************************


#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "btstack.h"
#include "ldacBT.h"
#include "ldacBT_abr.h"
#include "wasapi_capture.h"
#include "hci_cmd.h"
#include "ldacBT.h"
#include "mods/mod.h"

#include <string.h>

#include <windows.h>   // QueryPerformanceCounter for the audio clock
#include <string.h>
#include <stdlib.h>

// ---- user configuration (CLI > env > default) --------------------------------
// --addr AA:BB:CC:DD:EE:FF   target headphones (env LDAC_ADDR)
// --mode HQ|SQ|MQ            LDAC quality    (env LDAC_MODE, default HQ)
static char g_target_addr[18] = "AC:80:0A:F2:C5:A2";
static int  g_eqmid = LDACBT_EQMID_HQ;
static const char *g_eqmid_name = "HQ";
static int  g_gain_pct = 12;   // test-tone level, % of full scale (it is only a tone)
static int  g_source_system = 0;   // 1 = WASAPI loopback capture, 0 = test tone
static int  g_endpoint_index = -1; // -1 = default render endpoint
static char g_endpoint_name[128]; // non-empty: match endpoint by name
static int  g_endpoint_auto = 0;  // follow whichever endpoint is playing
static int  g_scan_mode = 0;      // --scan: list nearby devices, then exit
static int  g_abr_enabled = 0;
static HANDLE_LDAC_ABR g_abr_handle = NULL;

static void ldac_apply_mode(const char *m){
    if (!m) return;
    if (!strcasecmp(m, "HQ")){ g_eqmid = LDACBT_EQMID_HQ; g_eqmid_name = "HQ"; }
    else if (!strcasecmp(m, "SQ")){ g_eqmid = LDACBT_EQMID_SQ; g_eqmid_name = "SQ"; }
    else if (!strcasecmp(m, "MQ")){ g_eqmid = LDACBT_EQMID_MQ; g_eqmid_name = "MQ"; }
    else printf("LDAC: unknown mode '%s' (HQ|SQ|MQ), keeping %s\n", m, g_eqmid_name);
}

#define A2DP_CODEC_VENDOR_ID_SONY 0x12d
#define A2DP_SONY_CODEC_LDAC 0xaa

#define A2DP_CODEC_VENDOR_ID_APT_LTD 0x4f
#define A2DP_APT_LTD_CODEC_APTX 0x1

#define A2DP_CODEC_VENDOR_ID_QUALCOMM 0xd7
#define A2DP_QUALCOMM_CODEC_APTX_HD 0x24

#define A2DP_CODEC_VENDOR_ID_FRAUNHOFER 0x08A9
#define A2DP_FRAUNHOFER_CODEC_LC3PLUS 0x0001

// logarithmic volume reduction, samples are divided by 2^x
// #define VOLUME_REDUCTION 3

//#define AVRCP_BROWSING_ENABLED

// select preferred audio sampling rate: 44100 or 48000
#define A2DP_SOURCE_DEMO_PREFERRED_SAMPLING_RATE 44100

#define NUM_CHANNELS                2
#define BYTES_PER_AUDIO_SAMPLE      (2*NUM_CHANNELS)
#define AUDIO_TIMEOUT_MS            2 
#define TABLE_SIZE_441HZ            100

#define SBC_STORAGE_SIZE 1030

typedef enum {
    STREAM_SINE = 0,
    STREAM_MOD,
    STREAM_PTS_TEST
} stream_data_source_t;
    
typedef struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    uint8_t  stream_opened;
    uint16_t avrcp_cid;

    uint32_t time_audio_data_sent; // ms
    uint32_t acc_num_missed_samples;
    uint32_t samples_ready;
    btstack_timer_source_t audio_timer;
    uint8_t  streaming;
    int      max_media_payload_size;
    uint32_t rtp_timestamp;

    uint8_t  sbc_storage[SBC_STORAGE_SIZE];
    uint16_t sbc_storage_count;
    uint8_t  sbc_ready_to_send;

    uint8_t volume;
} a2dp_media_sending_context_t;

static  uint8_t __attribute__((unused)) media_sbc_codec_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | (AVDTP_SBC_48000 << 4) | AVDTP_SBC_STEREO,
    0xFF,//(AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
}; 

// Audio Generator
static struct {
    union {
        btstack_audio_generator_t      base;
        btstack_audio_generator_sine_t sine;
#ifdef ENABLE_MODPLAYER
        btstack_audio_generator_mod_t  mod;
#endif
    } generator;
    bool initialized;
} audio_generator_state;

static const int A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS = 12;

typedef struct {
    int reconfigure;
    
    int num_channels;
    int sampling_frequency;
    int block_length;
    int subbands;
    int min_bitpool_value;
    int max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

static btstack_packet_callback_registration_t hci_event_callback_registration;

#ifndef REMOTE_ADDR_STRING
#define REMOTE_ADDR_STRING "00:1A:7D:DA:71:01"
#endif
static const char * device_addr_string = REMOTE_ADDR_STRING;

static bd_addr_t device_addr;
static bool scan_active;

static uint8_t sdp_a2dp_source_service_buffer[150];
static uint8_t sdp_avrcp_target_service_buffer[200];
static uint8_t sdp_avrcp_controller_service_buffer[200];
static uint8_t device_id_sdp_service_buffer[100];

static media_codec_configuration_sbc_t sbc_configuration;

static const btstack_sbc_encoder_t *   sbc_encoder_instance;
static btstack_sbc_encoder_bluedroid_t sbc_encoder_state;

static uint8_t __attribute__((unused)) media_sbc_codec_configuration[4];

// ---- LDAC (vendor codec) ----
// Capability/configuration blob = Vendor ID (4, LE) + Codec ID (2, LE) + params
static uint8_t media_ldac_codec_capabilities[]     = { 0x2D,0x01,0x00,0x00, 0xAA,0x00, 0x3C,0x07 };
static uint8_t media_ldac_codec_configuration[]    = { 0x2D,0x01,0x00,0x00, 0xAA,0x00, 0x10,0x01 };
static uint8_t ldac_local_seid;
static uint8_t ldac_remote_seid;
static int     ldac_active;
static int     ldac_configured;
static HANDLE_LDAC_BT ldac_handle;
static int16_t ldac_pcm[128 * 2];
static int sent_packets;
static int stat_refused, stat_starved;
static int diag;
static hci_con_handle_t g_con_handle = 0xffff;
static uint8_t  ldac_pending[1024];
static uint16_t ldac_pending_len;
static uint32_t ldac_pending_ts;
static uint8_t  ldac_buf[1024];
static uint16_t g_ldac_cid;
static uint8_t  g_ldac_seid;
static uint32_t ldac_rtp_ts;

#define LDAC_RING_SAMPLES 16384
static int16_t ldac_ring[LDAC_RING_SAMPLES];
static int ldac_ring_head, ldac_ring_tail, ldac_ring_count;

static uint64_t ldac_now_us(void){
    static LARGE_INTEGER freq; static int init;
    LARGE_INTEGER c;
    if (!init){ QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000LL) / freq.QuadPart);
}
static void __attribute__((unused)) ldac_ring_write(const int16_t *src, int n){
    for (int i = 0; i < n; i++){ ldac_ring[ldac_ring_head] = src[i]; ldac_ring_head = (ldac_ring_head + 1) % LDAC_RING_SAMPLES; }
    ldac_ring_count += n;
}
static void __attribute__((unused)) ldac_ring_read(int16_t *dst, int n){
    for (int i = 0; i < n; i++){ dst[i] = ldac_ring[ldac_ring_tail]; ldac_ring_tail = (ldac_ring_tail + 1) % LDAC_RING_SAMPLES; }
    ldac_ring_count -= n;
}

// Encoder pump: called whenever the radio says it can take a packet, so the
// send rate follows the link instead of bursting at the timer and getting
// ACL_BUFFERS_FULL.
static void __attribute__((unused)) ldac_pump(void){
    if (!ldac_active || ldac_handle == NULL) return;
    int guard = 0;
    while (guard++ < 16){
        if (ldac_pending_len){
            uint8_t s = a2dp_source_stream_send_media_payload_rtp(g_ldac_cid, g_ldac_seid, 0,
                                                                  ldac_pending_ts, ldac_pending, ldac_pending_len);
            if (s != 0){
                stat_refused++;
                if (diag++ < 6 && g_con_handle != 0xffff){
                    fprintf(stderr, "DIAG: send 0x%02x free_acl_slots=%d pktbuf_reserved=%d\n", s, hci_number_free_acl_slots_for_handle(g_con_handle), (int)hci_is_packet_buffer_reserved()); fflush(stderr);
                }
                break;
            }
            ldac_pending_len = 0;
            sent_packets++;
            if ((sent_packets % 200) == 0) printf("LDAC: %d packets sent (refused %d, starved %d)\n", sent_packets, stat_refused, stat_starved);
            continue;
        }
        if (ldac_ring_count < 256){ stat_starved++; break; }
        ldac_ring_read(ldac_pcm, 256);
        int pcm_used = 0, stream_sz = 0, frame_num = 0;
        int rc = ldacBT_encode(ldac_handle, ldac_pcm, &pcm_used, &ldac_buf[1], &stream_sz, &frame_num);
        if (rc != 0){ printf("LDAC: encode rc=%d\n", rc); return; }
        if (stream_sz > 0){
            ldac_buf[0] = (uint8_t)(frame_num & 0x0F);
            memcpy(ldac_pending, ldac_buf, (size_t)stream_sz + 1);
            ldac_pending_len = (uint16_t)(stream_sz + 1);
            ldac_pending_ts = ldac_rtp_ts;
            ldac_rtp_ts += (uint32_t)frame_num * 128;
        }
    }
    // re-arm: BTstack delivers the next CAN_SEND only when asked
    a2dp_source_stream_endpoint_request_can_send_now(g_ldac_cid, g_ldac_seid);
}

static int ldac_encoder_init(void){
    ldac_handle = ldacBT_get_handle();
    if (!ldac_handle) return -1;
    int rc = ldacBT_init_handle_encode(ldac_handle, 679, g_eqmid,
                                       LDACBT_CHANNEL_MODE_STEREO,
                                       LDACBT_SMPL_FMT_S16, 48000);
    if (rc != 0) return -2;
    printf("LDAC: encoder initialised, %d kbps (%s), target %s, gain %d%%\n", ldacBT_get_bitrate(ldac_handle), g_eqmid_name, g_target_addr, g_gain_pct);

    // Adaptive bitrate: step HQ -> SQ -> MQ when the TX queue backs up, instead
    // of letting refusals turn into dropouts. Thresholds are in packets.
    g_abr_handle = ldac_ABR_get_handle();
    if (g_abr_handle && ldac_ABR_Init(g_abr_handle, 100) == 0 &&
        ldac_ABR_set_thresholds(g_abr_handle, 8, 4, 2) == 0){
        g_abr_enabled = 1;
        printf("LDAC: ABR enabled (thresholds 8/4/2 packets)\n");
    }
    return 0;
}
static a2dp_media_sending_context_t media_tracker;

static stream_data_source_t data_source;

static int current_sample_rate = 44100;

/* AVRCP Target context START */

typedef struct {
    uint8_t track_id[8];
    uint32_t song_length_ms;
    avrcp_playback_status_t status;
    uint32_t song_position_ms; // 0xFFFFFFFF if not supported
} avrcp_play_status_info_t;

// python -c "print('a'*512)"
static const char title[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

avrcp_track_t tracks[] = {
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}, 1, "Sine", "Generated", "A2DP Source Demo", "monotone", 12345, 6789},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}, 2, "Nao-deceased", "Decease", "A2DP Source Demo", "vivid", 12345, 6789},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}, 3, (char *)title, "Decease", "A2DP Source Demo", "vivid", 12345, 6789},
};
int current_track_index;
avrcp_play_status_info_t play_info;

/* AVRCP Target context END */

/* @section Main Application Setup
 *
 * @text The Listing MainConfiguration shows how to setup AD2P Source and AVRCP services. 
 * Besides calling init() method for each service, you'll also need to register several packet handlers:
 * - hci_packet_handler - handles pairing and device discovery
 * - a2dp_source_packet_handler - handles events on stream connection status (established, released), the media codec configuration, and, the commands on stream itself (open, pause, stopp).
 * - avrcp_packet_handler - receives connect/disconnect event.
 * - avrcp_controller_packet_handler - receives answers for sent AVRCP commands.
 * - avrcp_target_packet_handler - receives AVRCP commands, and registered notifications.
 * - stdin_process - used to trigger AVRCP commands to the A2DP Source device, such are get now playing info, start, stop, volume control. Requires HAVE_BTSTACK_STDIN.
 *
 * @text To announce A2DP Source and AVRCP services, you need to create corresponding
 * SDP records and register them with the SDP service. 
 */

/* LISTING_START(MainConfiguration): Setup Audio Source and AVRCP Target services */
static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void a2dp_source_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * event, uint16_t event_size);
static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
#ifdef HAVE_BTSTACK_STDIN
static void stdin_process(char cmd);
#endif

static int a2dp_source_and_avrcp_services_init(void){

    // Request role change on reconnecting headset to always use them in slave mode
    hci_set_master_slave_policy(0);
    // enabled EIR
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    l2cap_init();

#ifdef ENABLE_BLE
    // Initialize LE Security Manager. Needed for cross-transport key derivation
    // sm_init() removed for this build: the controller is BR/EDR only and
    // wedges on the first LE command (SM sends LE Rand at HCI_STATE_WORKING,
    // gets no Command Complete, and the stack dies).
    // sm_init();
#endif

    // Initialize  A2DP Source
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_source_packet_handler);

    // LDAC-only build: register just the vendor endpoint, otherwise BTstack's
    // config process picks the first registered endpoint (SBC) and LDAC never
    // gets selected.
    avdtp_stream_endpoint_t * local_stream_endpoint = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
        media_ldac_codec_capabilities, sizeof(media_ldac_codec_capabilities),
        media_ldac_codec_configuration, sizeof(media_ldac_codec_configuration));
    if (!local_stream_endpoint){
        printf("LDAC stream endpoint NOT created\n");
        return 1;
    }
    ldac_local_seid = avdtp_local_seid(local_stream_endpoint);
    printf("LDAC stream endpoint created, local seid %u\n", ldac_local_seid);

    avdtp_set_preferred_sampling_frequency(local_stream_endpoint, A2DP_SOURCE_DEMO_PREFERRED_SAMPLING_RATE);

    // Store stream enpoint's SEP ID, as it is used by A2DP API to indentify the stream endpoint
    media_tracker.local_seid = avdtp_local_seid(local_stream_endpoint);
    avdtp_source_register_delay_reporting_category(media_tracker.local_seid);

    // Initialize AVRCP Service
    avrcp_init();
    avrcp_register_packet_handler(&avrcp_packet_handler);
    // Initialize AVRCP Target
    avrcp_target_init();
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // Initialize AVRCP Controller
    avrcp_controller_init();
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);

    // Initialize SDP, 
    sdp_init();
    
    // Create A2DP Source service record and register it with SDP
    memset(sdp_a2dp_source_service_buffer, 0, sizeof(sdp_a2dp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_service_buffer, sdp_create_service_record_handle(), AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    btstack_assert(de_get_len( sdp_a2dp_source_service_buffer) <= sizeof(sdp_a2dp_source_service_buffer));
    sdp_register_service(sdp_a2dp_source_service_buffer);
    
    // Create AVRCP Target service record and register it with SDP. We receive Category 1 commands from the headphone, e.g. play/pause
    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    uint16_t supported_features = AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER;
#ifdef AVRCP_BROWSING_ENABLED
    supported_features |= AVRCP_FEATURE_MASK_BROWSING;
#endif
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, sdp_create_service_record_handle(), supported_features, NULL, NULL);
    btstack_assert(de_get_len( sdp_avrcp_target_service_buffer) <= sizeof(sdp_avrcp_target_service_buffer));
    sdp_register_service(sdp_avrcp_target_service_buffer);

    // Create AVRCP Controller service record and register it with SDP. We send Category 2 commands to the headphone, e.g. volume up/down
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    uint16_t controller_supported_features = AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER;
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(), controller_supported_features, NULL, NULL);
    btstack_assert(de_get_len( sdp_avrcp_controller_service_buffer) <= sizeof(sdp_avrcp_controller_service_buffer));
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    // Register Device ID (PnP) service SDP record
    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer, sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    btstack_assert(de_get_len( device_id_sdp_service_buffer) <= sizeof(device_id_sdp_service_buffer));
    sdp_register_service(device_id_sdp_service_buffer);

    // Set local name with a template Bluetooth address, that will be automatically
    // replaced with a actual address once it is available, i.e. when BTstack boots
    // up and starts talking to a Bluetooth module.
    gap_set_local_name("A2DP Source 00:00:00:00:00:00");
    gap_discoverable_control(1);
    gap_set_class_of_device(0x200408);
    // Just Works: we can't display a passkey. Without this the SSP exchange
    // can end up requesting numeric comparison we cannot answer.
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    
    // Register for HCI events.
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    data_source = STREAM_SINE;  // sine keeps up with 48 kHz; MOD player starves

    // Parse human readable Bluetooth address.
    sscanf_bd_addr(device_addr_string, device_addr);

#ifdef HAVE_BTSTACK_STDIN
    btstack_stdin_setup(stdin_process);
#endif
    return 0;
}
/* LISTING_END */

static void produce_audio(int16_t * pcm_buffer, int num_samples){
    if (g_source_system){
        // Loopback capture; returns silence if the endpoint is idle so the
        // A2DP sink never times out.
        wa_capture_read(pcm_buffer, num_samples);
        return;
    }
    btstack_audio_generator_generate(&audio_generator_state.generator.base, pcm_buffer, num_samples);
    if (g_gain_pct < 100){
        int n = num_samples * NUM_CHANNELS;
        for (int i = 0; i < n; i++) pcm_buffer[i] = (int16_t)((pcm_buffer[i] * g_gain_pct) / 100);
    }
#ifdef VOLUME_REDUCTION
    int i;
    for (i=0;i<num_samples*2;i++){
        if (pcm_buffer[i] > 0){
            pcm_buffer[i] =     pcm_buffer[i]  >> VOLUME_REDUCTION;
        } else {
            pcm_buffer[i] = -((-pcm_buffer[i]) >> VOLUME_REDUCTION);
        }
    }
#endif
}

static void a2dp_source_configure_audio_source(stream_data_source_t type, int sample_rate){
    printf("A2DP Source: Configure audio generator: type %s, frequency %u hz\n", type == STREAM_SINE ? "Sine" : "Mod", sample_rate);
    // finalize audio generator if active
    if (audio_generator_state.initialized) {
        btstack_audio_generator_finalize(&audio_generator_state.generator.base);
        audio_generator_state.initialized = false;
    }
    // initialize
    switch (type) {
        case STREAM_SINE:
            btstack_audio_generator_sine_init(&audio_generator_state.generator.sine, sample_rate, NUM_CHANNELS, 441);
            break;
#ifdef ENABLE_MODPLAYER
            case STREAM_MOD:
            btstack_audio_generator_modplayer_init(&audio_generator_state.generator.mod, sample_rate, NUM_CHANNELS,
                mod_titles[MOD_TITLE_DEFAULT].data, mod_titles[MOD_TITLE_DEFAULT].len);
            break;
#endif
        default:
            btstack_unreachable();
            break;
    }
    audio_generator_state.initialized = true;
    media_tracker.sbc_storage_count = 0;
    media_tracker.samples_ready = 0;
}

static void a2dp_demo_send_media_packet(void){
    int num_bytes_in_frame = sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    int bytes_in_storage = media_tracker.sbc_storage_count;
    uint8_t num_sbc_frames = bytes_in_storage / num_bytes_in_frame;
    // Prepend SBC Header
    media_tracker.sbc_storage[0] = num_sbc_frames;  // (fragmentation << 7) | (starting_packet << 6) | (last_packet << 5) | num_frames;
    a2dp_source_stream_send_media_payload_rtp(media_tracker.a2dp_cid, media_tracker.local_seid, 0,
                                               media_tracker.rtp_timestamp,
                                               media_tracker.sbc_storage, bytes_in_storage + 1);

    // update rtp_timestamp
    unsigned int num_audio_samples_per_sbc_buffer = sbc_encoder_instance->num_audio_frames(&sbc_encoder_state);
    media_tracker.rtp_timestamp += num_sbc_frames * num_audio_samples_per_sbc_buffer;

    media_tracker.sbc_storage_count = 0;
    media_tracker.sbc_ready_to_send = 0;
}

static int a2dp_demo_fill_sbc_audio_buffer(a2dp_media_sending_context_t * context){
    // perform sbc encoding
    int total_num_bytes_read = 0;
    unsigned int num_audio_samples_per_sbc_buffer =  sbc_encoder_instance->num_audio_frames(&sbc_encoder_state);
    uint16_t sbc_buffer_length = sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    while (context->samples_ready >= num_audio_samples_per_sbc_buffer
        && (context->max_media_payload_size - context->sbc_storage_count) >= sbc_buffer_length){

        int16_t pcm_frame[256*NUM_CHANNELS];

        produce_audio(pcm_frame, num_audio_samples_per_sbc_buffer);

        // encode into sbc storage buffer, first byte contains sbc media header
        sbc_encoder_instance->encode_signed_16(&sbc_encoder_state, pcm_frame, &context->sbc_storage[1 + context->sbc_storage_count]);

        total_num_bytes_read += num_audio_samples_per_sbc_buffer;

        uint16_t sbc_frame_size =  sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
        context->sbc_storage_count += sbc_frame_size;
        context->samples_ready -= num_audio_samples_per_sbc_buffer;
    }
    return total_num_bytes_read;
}

static void a2dp_demo_audio_timeout_handler(btstack_timer_source_t * timer){
    a2dp_media_sending_context_t * context = (a2dp_media_sending_context_t *) btstack_run_loop_get_timer_context(timer);
    btstack_run_loop_set_timer(&context->audio_timer, AUDIO_TIMEOUT_MS); 
    btstack_run_loop_add_timer(&context->audio_timer);
    uint32_t now = btstack_run_loop_get_time_ms();

    uint32_t update_period_ms = AUDIO_TIMEOUT_MS;
    if (context->time_audio_data_sent > 0){
        update_period_ms = now - context->time_audio_data_sent;
    } 

    uint32_t num_samples = (update_period_ms * current_sample_rate) / 1000;
    context->acc_num_missed_samples += (update_period_ms * current_sample_rate) % 1000;
    
    while (context->acc_num_missed_samples >= 1000){
        num_samples++;
        context->acc_num_missed_samples -= 1000;
    }
    context->time_audio_data_sent = now;
    context->samples_ready += num_samples;

    // LDAC path. One packet in flight at a time: if the controller's ACL
    // buffers are full (0x57) keep the encoded packet and retry next tick
    // instead of dropping it.
    if (ldac_active){
        static uint64_t last_us;
        static int64_t  owed;
        uint64_t now_us = ldac_now_us();
        if (last_us == 0) last_us = now_us;
        owed += (int64_t)((now_us - last_us) * 48000ULL / 1000000ULL);
        last_us = now_us;
        if (owed > 24000) owed = 24000;

        // flush a packet held back by a refusal first — never drop audio
        if (ldac_pending_len){
            uint8_t st = a2dp_source_stream_send_media_payload_rtp(context->a2dp_cid, context->local_seid, 0,
                                                                   ldac_pending_ts, ldac_pending, ldac_pending_len);
            if (st != 0) return;
            ldac_pending_len = 0;
            sent_packets++;
            if ((sent_packets % 500) == 0) printf("LDAC: %d packets sent (refused %d, owed %d)\n", sent_packets, stat_refused, (int)owed);
        }

        int guard = 0;
        while (owed >= 128 && guard++ < 24){
            int pcm_used = 0, stream_sz = 0, frame_num = 0;
            produce_audio(ldac_pcm, 128);
            owed -= 128;
            int rc = ldacBT_encode(ldac_handle, ldac_pcm, &pcm_used,
                                   &context->sbc_storage[1], &stream_sz, &frame_num);
            if (rc != 0){ printf("LDAC: encode rc=%d\n", rc); break; }
            if (stream_sz <= 0) continue;
            context->sbc_storage[0] = (uint8_t)(frame_num & 0x0F);
            memcpy(ldac_pending, context->sbc_storage, (size_t)stream_sz + 1);
            ldac_pending_len = (uint16_t)(stream_sz + 1);
            ldac_pending_ts  = context->rtp_timestamp;
            context->rtp_timestamp += (uint32_t)frame_num * 128;
            uint8_t st = a2dp_source_stream_send_media_payload_rtp(context->a2dp_cid, context->local_seid, 0,
                                                                   ldac_pending_ts, ldac_pending, ldac_pending_len);
            if (g_abr_enabled){
                unsigned int depth = (unsigned int)(owed / 128) + (ldac_pending_len ? 1u : 0u);
                ldac_ABR_Proc(ldac_handle, g_abr_handle, depth, 1);
            }
            if (st != 0){ stat_refused++; break; }   // held for the next tick
            ldac_pending_len = 0;
            sent_packets++;
            if ((sent_packets % 500) == 0) printf("LDAC: %d packets sent (refused %d, owed %d)\n", sent_packets, stat_refused, (int)owed);
        }
        return;
    }

    if (context->sbc_ready_to_send) return;

    a2dp_demo_fill_sbc_audio_buffer(context);

    if ((context->sbc_storage_count + sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state)) > context->max_media_payload_size){
        // schedule sending
        context->sbc_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(context->a2dp_cid, context->local_seid);
    }
}

static void a2dp_demo_timer_start(a2dp_media_sending_context_t * context){
    context->max_media_payload_size = btstack_min(a2dp_max_media_payload_size(context->a2dp_cid, context->local_seid), SBC_STORAGE_SIZE);
    context->sbc_storage_count = 0;
    context->sbc_ready_to_send = 0;
    context->streaming = 1;
    btstack_run_loop_remove_timer(&context->audio_timer);
    btstack_run_loop_set_timer_handler(&context->audio_timer, a2dp_demo_audio_timeout_handler);
    btstack_run_loop_set_timer_context(&context->audio_timer, context);
    btstack_run_loop_set_timer(&context->audio_timer, AUDIO_TIMEOUT_MS); 
    btstack_run_loop_add_timer(&context->audio_timer);
}

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context){
    context->time_audio_data_sent = 0;
    context->acc_num_missed_samples = 0;
    context->samples_ready = 0;
    context->streaming = 1;
    context->sbc_storage_count = 0;
    context->sbc_ready_to_send = 0;
    btstack_run_loop_remove_timer(&context->audio_timer);
} 

static void dump_sbc_configuration(media_codec_configuration_sbc_t * configuration){
    printf("Received media codec configuration:\n");
    printf("    - num_channels: %d\n", configuration->num_channels);
    printf("    - sampling_frequency: %d\n", configuration->sampling_frequency);
    printf("    - channel_mode: %d\n", configuration->channel_mode);
    printf("    - block_length: %d\n", configuration->block_length);
    printf("    - subbands: %d\n", configuration->subbands);
    printf("    - allocation_method: %d\n", configuration->allocation_method);
    printf("    - bitpool_value [%d, %d] \n", configuration->min_bitpool_value, configuration->max_bitpool_value);
}

static void __attribute__((unused)) a2dp_source_demo_start_scanning(void){
    printf("Start scanning...\n");
    gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
    scan_active = true;
}

static hci_con_handle_t pending_auth_handle = 0xffff;
static btstack_timer_source_t auth_retry_timer;
static btstack_timer_source_t post_pair_timer;

// Reconnect the A2DP stream once the link is authenticated.
static void post_pair_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    uint8_t st = a2dp_source_establish_stream(device_addr, &media_tracker.a2dp_cid);
    printf("POSTPAIR: establish_stream status 0x%02x cid 0x%02x\n", st, media_tracker.a2dp_cid);
}

// Reuse the pairing the headset already has with this Intel adapter, dumped
// from HKLM SYSTEM CurrentControlSet Services BTHPORT Parameters Keys
// f8b54d6f5be3 ac800af2c5a2 (SYSTEM-only) on this PC. Must run AFTER the
// link key store is initialised (i.e. once HCI is working), otherwise the
// entry is dropped and BTstack answers Link Key Request with "negative".
static void __attribute__((unused)) store_windows_link_key(void){
    bd_addr_t paired_addr;
    link_key_t win_key = { 0xB3,0xCF,0xCD,0x89,0x67,0x5A,0xCE,0xAE,
                           0x11,0xC5,0x86,0x98,0x22,0xBE,0xA9,0x52 };
    if (sscanf_bd_addr(g_target_addr, paired_addr) > 0){
        gap_store_link_key_for_bd_addr(paired_addr, win_key, UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192);
        printf("Stored the Windows-paired link key for %s\n", bd_addr_to_str(paired_addr));
    }
}

// hci_send_cmd() is refused from inside an event callback ("cannot send packet
// now"), so retry from a timer until the command slot is free.
static void auth_retry_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (pending_auth_handle == 0xffff) return;
    int sent = hci_send_cmd(&hci_authentication_requested, pending_auth_handle);
    if (sent > 0){
        printf("AUTH: Authentication Requested sent, handle 0x%04x\n", pending_auth_handle);
        return;
    }
    btstack_run_loop_remove_timer(&auth_retry_timer);
    btstack_run_loop_set_timer(&auth_retry_timer, 100);
    btstack_run_loop_add_timer(&auth_retry_timer);
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t status;
    UNUSED(status);

    bd_addr_t address;
    uint32_t cod;

    // Service Class: Audio, Major Device Class: Audio
    const uint32_t bluetooth_speaker_cod = 0x200000 | 0x000400;

    switch (hci_event_packet_get_type(packet)){
        case  BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            // Autonomous mode: no stdin needed. Connect straight to the target;
            // HCI_EVENT_CONNECTION_COMPLETE then requests authentication.
            {
                static int auto_connected = 0;
                if (!auto_connected){
                    auto_connected = 1;
                    {
                        const char *kh = getenv("LDAC_KEY_HEX");
                        const char *kt = getenv("LDAC_KEY_TYPE");
                        if (kh && strlen(kh) == 32){
                            bd_addr_t ka; link_key_t kk;
                            for (int i = 0; i < 16; i++){
                                unsigned v = 0; sscanf(kh + 2*i, "%2x", &v); kk[i] = (uint8_t)v;
                            }
                            int type = kt ? atoi(kt) : 4;
                            if (sscanf_bd_addr(g_target_addr, ka) > 0){
                                gap_store_link_key_for_bd_addr(ka, kk, (link_key_type_t)type);
                                printf("KEY: injected type %d key %s\n", type, kh);
                            }
                        }
                    }
                    if (g_scan_mode){
                        printf("SCAN: starting inquiry (about 10 s)...\n");
                        scan_active = false;
                        gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
                        break;
                    }
                    if (sscanf_bd_addr(g_target_addr, device_addr) > 0){
                        printf("AUTO: connecting to %s\n", bd_addr_to_str(device_addr));
                        uint8_t st = a2dp_source_establish_stream(device_addr, &media_tracker.a2dp_cid);
                        printf("AUTO: establish_stream status 0x%02x cid 0x%02x\n", st, media_tracker.a2dp_cid);
                    } else {
                        printf("AUTO: bad address\n");
                    }
                }
            }
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            // The headset (a sink) never starts pairing itself: it accepts the
            // baseband link, answers SDP, then drops the link ~10 s later when
            // no authenticated A2DP channel appears.
            // General bonding: request authentication as soon as the link is up.
            if (hci_event_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS){
                pending_auth_handle = hci_event_connection_complete_get_connection_handle(packet);
                g_con_handle = pending_auth_handle;
                printf("Link up (handle 0x%04x), scheduling authentication request\n", pending_auth_handle);
                btstack_run_loop_set_timer(&auth_retry_timer, 50);
                btstack_run_loop_set_timer_handler(&auth_retry_timer, &auth_retry_handler);
                btstack_run_loop_add_timer(&auth_retry_timer);
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            pending_auth_handle = 0xffff;
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
        {
            uint8_t auth_status = hci_event_authentication_complete_get_status(packet);
            printf("Authentication complete, status 0x%02x\n", auth_status);
            if (auth_status == ERROR_CODE_SUCCESS){
                // The AVDTP connect attempted before pairing was refused with
                // 0x66 (security). Retry now that the link is authenticated.
                btstack_run_loop_remove_timer(&post_pair_timer);
                btstack_run_loop_set_timer(&post_pair_timer, 300);
                btstack_run_loop_set_timer_handler(&post_pair_timer, &post_pair_handler);
                btstack_run_loop_add_timer(&post_pair_timer);
            }
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST:
            // 2.0+EDR controller: no SSP, so the headset falls back to legacy
            // pairing. Sony headsets use the fixed PIN 0000.
            hci_event_pin_code_request_get_bd_addr(packet, address);
            printf("Pin code request from %s -> replying 0000 (legacy pairing)\n", bd_addr_to_str(address));
            gap_pin_code_response(address, "0000");
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            printf("SSP User Confirmation Request with numeric value '%06"PRIu32"'\n", hci_event_user_confirmation_request_get_numeric_value(packet));
            printf("Accepting Pairing - TODO: require actual user action\n");
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            gap_ssp_confirmation_response(address);
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            gap_event_inquiry_result_get_bd_addr(packet, address);
            // print info
            printf("Device found: %s ",  bd_addr_to_str(address));
            cod = gap_event_inquiry_result_get_class_of_device(packet);
            printf("with COD: %06" PRIx32, cod);
            if (gap_event_inquiry_result_get_rssi_available(packet)){
                printf(", rssi %d dBm", (int8_t) gap_event_inquiry_result_get_rssi(packet));
            }
            if (gap_event_inquiry_result_get_name_available(packet)){
                char name_buffer[240];
                int name_len = gap_event_inquiry_result_get_name_len(packet);
                memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), name_len);
                name_buffer[name_len] = 0;
                printf(", name '%s'", name_buffer);
            }
            printf("\n");
            if (!g_scan_mode && (cod & bluetooth_speaker_cod) == bluetooth_speaker_cod){
                memcpy(device_addr, address, 6);
                printf("Bluetooth speaker detected, trying to connect to %s...\n", bd_addr_to_str(device_addr));
                scan_active = false;
                gap_inquiry_stop();
                a2dp_source_establish_stream(device_addr, &media_tracker.a2dp_cid);
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (g_scan_mode){
                printf("\nPick one of the addresses above and run with --addr AA:BB:CC:DD:EE:FF\n");
                fflush(stdout);
                exit(0);
            }
            if (scan_active){
                printf("No Bluetooth speakers found, scanning again...\n");
                gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
            }
            break;
        default:
            break;
    }
}

static uint32_t get_vendor_id(const uint8_t *codec_info) {
    uint32_t vendor_id = 0;
    vendor_id |= codec_info[0];
    vendor_id |= codec_info[1] << 8;
    vendor_id |= codec_info[2] << 16;
    vendor_id |= codec_info[3] << 24;
    return vendor_id;
}

static uint16_t get_codec_id(const uint8_t *codec_info) {
    uint16_t codec_id = 0;
    codec_id |= codec_info[4];
    codec_id |= codec_info[5] << 8;
    return codec_id;
}

static void a2dp_source_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t status;
    uint8_t local_seid;
    bd_addr_t address;
    uint16_t cid;

    avdtp_channel_mode_t channel_mode;
    uint8_t allocation_method;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)){
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, address);
            cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            status = a2dp_subevent_signaling_connection_established_get_status(packet);

            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP Source: Connection failed, status 0x%02x, cid 0x%02x, a2dp_cid 0x%02x \n", status, cid, media_tracker.a2dp_cid);
                media_tracker.a2dp_cid = 0;
                break;
            }
            media_tracker.a2dp_cid = cid;
            media_tracker.volume = 32;

            printf("A2DP Source: Connected to address %s, a2dp cid 0x%02x, local seid 0x%02x.\n", bd_addr_to_str(address), media_tracker.a2dp_cid, media_tracker.local_seid);
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            cid  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet);
            if (cid != media_tracker.a2dp_cid) return;

            media_tracker.remote_seid = a2dp_subevent_signaling_media_codec_sbc_configuration_get_remote_seid(packet);

            sbc_configuration.reconfigure = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            sbc_configuration.num_channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            sbc_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            sbc_configuration.block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            sbc_configuration.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            sbc_configuration.min_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            sbc_configuration.max_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);

            channel_mode = (avdtp_channel_mode_t) a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
            allocation_method = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);

            printf("A2DP Source: Received SBC codec configuration, sampling frequency %u, a2dp_cid 0x%02x, local seid 0x%02x, remote seid 0x%02x.\n",
                sbc_configuration.sampling_frequency, cid,
                   a2dp_subevent_signaling_media_codec_sbc_configuration_get_local_seid(packet),
                   a2dp_subevent_signaling_media_codec_sbc_configuration_get_remote_seid(packet));

            // Adapt Bluetooth spec definition to SBC Encoder expected input
            sbc_configuration.allocation_method = (btstack_sbc_allocation_method_t)(allocation_method - 1);
            switch (channel_mode){
                case AVDTP_CHANNEL_MODE_JOINT_STEREO:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_STEREO:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
                    break;
                case AVDTP_CHANNEL_MODE_MONO:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO;
                    break;
                default:
                    btstack_assert(false);
                    break;
            }
            dump_sbc_configuration(&sbc_configuration);

            current_sample_rate = sbc_configuration.sampling_frequency;

            sbc_encoder_instance = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_state);
            sbc_encoder_instance->configure(&sbc_encoder_state, SBC_MODE_STANDARD,
                                            sbc_configuration.block_length, sbc_configuration.subbands,
                                            sbc_configuration.allocation_method, sbc_configuration.sampling_frequency,
                                            sbc_configuration.max_bitpool_value,
                                            sbc_configuration.channel_mode);
            break;

        case A2DP_SUBEVENT_SIGNALING_DELAY_REPORT:
            printf("DELAY_REPORT received: %d.%0d ms, local seid %d\n",
                   a2dp_subevent_signaling_delay_report_get_delay_100us(packet)/10, a2dp_subevent_signaling_delay_report_get_delay_100us(packet)%10,
                   a2dp_subevent_signaling_delay_report_get_local_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY:
            printf("A2DP Source: remote supports delay report, remote seid %d\n",
                   a2dp_subevent_signaling_delay_reporting_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
            printf("CAPABILITY - MEDIA_CODEC: SBC, remote seid %u\n", a2dp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CAPABILITY:
            printf("CAPABILITY - MEDIA_CODEC: MPEG AUDIO, remote seid %u\n", a2dp_subevent_signaling_media_codec_mpeg_audio_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY:
            printf("CAPABILITY - MEDIA_CODEC: MPEG AAC, remote seid %u\n", a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CAPABILITY:
            printf("CAPABILITY - MEDIA_CODEC: ATRAC, remote seid %u\n", a2dp_subevent_signaling_media_codec_atrac_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_D_USAC_CAPABILITY:
            printf("CAPABILITY - MEDIA_CODEC: MPEG_D_USAC, remote seid %u\n", a2dp_subevent_signaling_media_codec_mpeg_d_usac_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:{
            uint8_t remote_seid = a2dp_subevent_signaling_media_codec_other_capability_get_remote_seid(packet);
            const uint8_t *media_info = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);
            uint32_t vendor_id = get_vendor_id(media_info);
            uint16_t codec_id = get_codec_id(media_info);

            if (vendor_id == A2DP_CODEC_VENDOR_ID_SONY && codec_id == A2DP_SONY_CODEC_LDAC){
                printf("CAPABILITY - LDAC, remote seid %u\n", remote_seid);
                if (!ldac_configured){
                    ldac_configured = 1;
                    ldac_remote_seid = remote_seid;
                    uint16_t ev_cid = a2dp_subevent_signaling_media_codec_other_capability_get_a2dp_cid(packet);
                    uint8_t st = a2dp_source_set_config_other(ev_cid, ldac_local_seid, remote_seid,
                                                              media_ldac_codec_configuration,
                                                              sizeof(media_ldac_codec_configuration));
                    printf("LDAC: set_config_other status 0x%02x (local %u -> remote %u)\n", st, ldac_local_seid, remote_seid);
                }
            }
            else if (vendor_id == A2DP_CODEC_VENDOR_ID_APT_LTD && codec_id == A2DP_APT_LTD_CODEC_APTX)
                printf("CAPABILITY - APTX, remote seid %u\n", remote_seid);
            else if (vendor_id == A2DP_CODEC_VENDOR_ID_QUALCOMM && codec_id == A2DP_QUALCOMM_CODEC_APTX_HD)
                printf("CAPABILITY - APTX HD, remote seid %u\n", remote_seid);
            else if (vendor_id == A2DP_CODEC_VENDOR_ID_FRAUNHOFER && codec_id == A2DP_FRAUNHOFER_CODEC_LC3PLUS) {
                printf("CAPABILITY - LC3plus, remote seid %u\n", remote_seid);
            } else {
                printf("CAPABILITY - MEDIA_CODEC: OTHER, remote seid %u\n", remote_seid);
            }
            break;
        }
        case A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE:
            printf("A2DP Source: All streamendpoints capabilities queried\n\n");
            break;
       
        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            a2dp_subevent_stream_established_get_bd_addr(packet, address);
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP Source: Stream failed, status 0x%02x.\n", status);
                break;
            }
            
            local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            
            printf("A2DP Source: Stream established a2dp_cid 0x%02x, local_seid 0x%02x, remote_seid 0x%02x\n", cid, local_seid, a2dp_subevent_stream_established_get_remote_seid(packet));
            
            media_tracker.stream_opened = 1;
            if (local_seid == ldac_local_seid){
                ldac_active = 1;
                g_ldac_cid = media_tracker.a2dp_cid;
                g_ldac_seid = local_seid;
                current_sample_rate = 48000;
                if (ldac_encoder_init() != 0) printf("LDAC: encoder init FAILED\n");
            }
            status = a2dp_source_start_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
            break;

        case A2DP_SUBEVENT_STREAM_RECONFIGURED:
            status = a2dp_subevent_stream_reconfigured_get_status(packet);
            local_seid = a2dp_subevent_stream_reconfigured_get_local_seid(packet);
            cid = a2dp_subevent_stream_reconfigured_get_a2dp_cid(packet);

            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP Source: Stream reconfiguration failed, status 0x%02x\n", status);
                break;
            }

            printf("A2DP Source: Stream reconfigured a2dp_cid 0x%02x, local_seid 0x%02x\n", cid, local_seid);
            status = a2dp_source_start_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            local_seid = a2dp_subevent_stream_started_get_local_seid(packet);
            cid = a2dp_subevent_stream_started_get_a2dp_cid(packet);

            play_info.status = AVRCP_PLAYBACK_STATUS_PLAYING;
            if (media_tracker.avrcp_cid){
                avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, &tracks[data_source], sizeof(tracks)/sizeof(avrcp_track_t));
                avrcp_target_set_playback_status(media_tracker.avrcp_cid, AVRCP_PLAYBACK_STATUS_PLAYING);
            }

            a2dp_source_configure_audio_source(data_source, current_sample_rate);

            a2dp_demo_timer_start(&media_tracker);
            printf("A2DP Source: Stream started, a2dp_cid 0x%02x, local_seid 0x%02x\n", cid, local_seid);
            break;

        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            local_seid = a2dp_subevent_streaming_can_send_media_packet_now_get_local_seid(packet);
            cid = a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet);
            if (ldac_active) ldac_pump(); else a2dp_demo_send_media_packet();
            break;        

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            local_seid = a2dp_subevent_stream_suspended_get_local_seid(packet);
            cid = a2dp_subevent_stream_suspended_get_a2dp_cid(packet);
            
            play_info.status = AVRCP_PLAYBACK_STATUS_PAUSED;
            if (media_tracker.avrcp_cid){
                avrcp_target_set_playback_status(media_tracker.avrcp_cid, AVRCP_PLAYBACK_STATUS_PAUSED);
            }
            printf("A2DP Source: Stream paused, a2dp_cid 0x%02x, local_seid 0x%02x\n", cid, local_seid);
            
            a2dp_demo_timer_stop(&media_tracker);
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            play_info.status = AVRCP_PLAYBACK_STATUS_STOPPED;
            cid = a2dp_subevent_stream_released_get_a2dp_cid(packet);
            local_seid = a2dp_subevent_stream_released_get_local_seid(packet);
            
            printf("A2DP Source: Stream released, a2dp_cid 0x%02x, local_seid 0x%02x\n", cid, local_seid);

            if (cid == media_tracker.a2dp_cid) {
                media_tracker.stream_opened = 0;
                printf("A2DP Source: Stream released.\n");
            }
            if (media_tracker.avrcp_cid){
                avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, NULL, sizeof(tracks)/sizeof(avrcp_track_t));
                avrcp_target_set_playback_status(media_tracker.avrcp_cid, AVRCP_PLAYBACK_STATUS_STOPPED);
            }
            a2dp_demo_timer_stop(&media_tracker);
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            cid = a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet);
            if (cid == media_tracker.a2dp_cid) {
                media_tracker.avrcp_cid = 0;
                media_tracker.a2dp_cid = 0;
                printf("A2DP Source: Signaling released.\n\n");
            }
            break;
        default:
            break; 
    }
}

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    bd_addr_t event_addr;
    uint16_t local_cid;
    uint8_t  status = ERROR_CODE_SUCCESS;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    
    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: 
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("AVRCP: Connection failed, local cid 0x%02x, status 0x%02x\n", local_cid, status);
                return;
            }
            media_tracker.avrcp_cid = local_cid;
            avrcp_subevent_connection_established_get_bd_addr(packet, event_addr);

            printf("AVRCP: Channel to %s successfully opened, avrcp_cid 0x%02x\n", bd_addr_to_str(event_addr), media_tracker.avrcp_cid);
             
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, NULL, sizeof(tracks)/sizeof(avrcp_track_t));
            
            printf("Enable Volume Change notification\n");
            avrcp_controller_enable_notification(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            printf("Enable Battery Status Change notification\n");
            avrcp_controller_enable_notification(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            return;
        
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            printf("AVRCP Target: Disconnected, avrcp_cid 0x%02x\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            media_tracker.avrcp_cid = 0;
            return;
        default:
            break;
    }

    if (status != ERROR_CODE_SUCCESS){
        printf("Responding to event 0x%02x failed, status 0x%02x\n", packet[2], status);
    }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t  status = ERROR_CODE_SUCCESS;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    bool button_pressed;
    char const * button_state;
    avrcp_operation_id_t operation_id;

    switch (packet[2]){
        case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
            status = avrcp_target_play_status(media_tracker.avrcp_cid, play_info.song_length_ms, play_info.song_position_ms, play_info.status);            
            break;
        // case AVRCP_SUBEVENT_NOW_PLAYING_INFO_QUERY:
        //     status = avrcp_target_now_playing_info(avrcp_cid);
        //     break;
        case AVRCP_SUBEVENT_OPERATION:
            operation_id = (avrcp_operation_id_t) avrcp_subevent_operation_get_operation_id(packet);
            button_pressed = avrcp_subevent_operation_get_button_pressed(packet) > 0;
            button_state = button_pressed ? "PRESS" : "RELEASE";

            printf("AVRCP Target: operation %s (%s)\n", avrcp_operation2str(operation_id), button_state);

            if (!button_pressed){
                break;
            }
            switch (operation_id) {
                case AVRCP_OPERATION_ID_PLAY:
                    status = a2dp_source_start_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
                    break;
                case AVRCP_OPERATION_ID_PAUSE:
                    status = a2dp_source_pause_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
                    break;
                case AVRCP_OPERATION_ID_STOP:
                    status = a2dp_source_stop_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    if (status != ERROR_CODE_SUCCESS){
        printf("Responding to event 0x%02x failed, status 0x%02x\n", packet[2], status);
    }
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (!media_tracker.avrcp_cid) return;
    
    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            printf("AVRCP Controller: Notification Absolute Volume %d %%\n", avrcp_subevent_notification_volume_changed_get_absolute_volume(packet) * 100 / 127);
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_BATT_STATUS_CHANGED:
            // see avrcp_battery_status_t
            printf("AVRCP Controller: Notification Battery Status 0x%02x\n", avrcp_subevent_notification_event_batt_status_changed_get_battery_status(packet));
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_STATE:
            printf("AVRCP Controller: Notification %s - %s\n", 
                avrcp_event2str(avrcp_subevent_notification_state_get_event_id(packet)), 
                avrcp_subevent_notification_state_get_enabled(packet) != 0 ? "enabled" : "disabled");
            break;
        default:
            break;
    }  
}

#ifdef HAVE_BTSTACK_STDIN
static void show_usage(void){
    bd_addr_t      iut_address;
    gap_local_bd_addr(iut_address);
    printf("\n--- Bluetooth  A2DP Source/AVRCP Demo %s ---\n", bd_addr_to_str(iut_address));
    printf("a      - Scan for Bluetooth speaker and connect\n");
    printf("b      - A2DP Source create connection to addr %s\n", device_addr_string);
    printf("B      - A2DP Source disconnect\n");
    printf("c      - AVRCP create connection to addr %s\n", device_addr_string);
    printf("C      - AVRCP disconnect\n");
    printf("D      - delete all link keys\n");

    printf("x      - start streaming sine\n");
#ifdef ENABLE_MODPLAYER
    printf("z      - start streaming '%s'\n", mod_titles[MOD_TITLE_DEFAULT].name);
#endif
    printf("p      - pause streaming\n");
    printf("w      - reconfigure stream for 44100 Hz\n");
    printf("e      - reconfigure stream for 48000 Hz\n");
    printf("t      - volume up\n");
    printf("T      - volume down\n");
    printf("v      - volume up (via set absolute volume)\n");
    printf("V      - volume down (via set absolute volume)\n");

    printf("---\n");
}

static void stdin_process(char cmd){
    uint8_t status = ERROR_CODE_SUCCESS;
    int new_sample_rate;
    switch (cmd){
        case 'a':
            a2dp_source_demo_start_scanning();
            break;
        case 'b':
            status = a2dp_source_establish_stream(device_addr, &media_tracker.a2dp_cid);
            printf("%c - Create A2DP Source connection to addr %s, cid 0x%02x.\n", cmd, bd_addr_to_str(device_addr), media_tracker.a2dp_cid);
            break;
        case 'B':
            printf("%c - A2DP Source Disconnect from cid 0x%2x\n", cmd, media_tracker.a2dp_cid);
            status = a2dp_source_disconnect(media_tracker.a2dp_cid);
            break;
        case 'c':
            printf("%c - Create AVRCP connection to addr %s.\n", cmd, bd_addr_to_str(device_addr));
            status = avrcp_connect(device_addr, &media_tracker.avrcp_cid);
            break;
        case 'C':
            printf("%c - AVRCP disconnect\n", cmd);
            status = avrcp_disconnect(media_tracker.avrcp_cid);
            break;
        case 'D':
            printf("Deleting all link keys\n");
            gap_delete_all_link_keys();
            break;
        case '\n':
        case '\r':
            break;

        case 't':
            printf(" - volume up\n");
            status = avrcp_controller_volume_up(media_tracker.avrcp_cid);
            break;
        case 'T':
            printf(" - volume down\n");
            status = avrcp_controller_volume_down(media_tracker.avrcp_cid);
            break;

        case 'v':
            if (media_tracker.volume > 117){
                media_tracker.volume = 127;
            } else {
                media_tracker.volume += 10;
            }
            printf(" - volume up (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
            status = avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
            break;
        case 'V':
            if (media_tracker.volume < 10){
                media_tracker.volume = 0;
            } else {
                media_tracker.volume -= 10;
            }
            printf(" - volume down (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
            status = avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
            break;
        
        case 'x':
            if (media_tracker.avrcp_cid){
                avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, &tracks[data_source], sizeof(tracks)/sizeof(avrcp_track_t));
            }
            printf("%c - Play sine.\n", cmd);
            data_source = STREAM_SINE;
            a2dp_source_configure_audio_source(data_source, current_sample_rate);
            if (!media_tracker.stream_opened) break;
            status = a2dp_source_start_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
            break;
#ifdef ENABLE_MODPLAYER
            case 'z':
            if (media_tracker.avrcp_cid){
                avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, &tracks[data_source], sizeof(tracks)/sizeof(avrcp_track_t));
            }
            printf("%c - Play mod.\n", cmd);
            data_source = STREAM_MOD;
            a2dp_source_configure_audio_source(data_source, current_sample_rate);
            if (!media_tracker.stream_opened) break;
            status = a2dp_source_start_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
            break;
#endif
        case 'p':
            if (!media_tracker.stream_opened) break;
            printf("%c - Pause stream.\n", cmd);
            status = a2dp_source_pause_stream(media_tracker.a2dp_cid, media_tracker.local_seid);
            break;
        
        case 'w':
            if (!media_tracker.stream_opened) break;
            if (play_info.status == AVRCP_PLAYBACK_STATUS_PLAYING){
                printf("Stream cannot be reconfigured while playing, please pause stream first\n");
                break;
            }
            new_sample_rate = 44100;
            if (current_sample_rate == new_sample_rate){
                printf("%c - Stream already configured for %d Hz.\n", cmd, new_sample_rate);
            } else {
                printf("%c - Reconfigure for %d Hz.\n", cmd, new_sample_rate);
                status = a2dp_source_reconfigure_stream_sampling_frequency(media_tracker.a2dp_cid, new_sample_rate);
            }
            break;

        case 'e':
            if (!media_tracker.stream_opened) break;
            if (play_info.status == AVRCP_PLAYBACK_STATUS_PLAYING){
                printf("Stream cannot be reconfigured while playing, please pause stream first\n");
                break;
            }
            new_sample_rate = 48000;
            if (current_sample_rate == new_sample_rate){
                printf("%c - Stream already configured for %d Hz.\n", cmd, new_sample_rate);
            } else {
                printf("%c - Reconfigure for %d Hz.\n", cmd, new_sample_rate);
                status = a2dp_source_reconfigure_stream_sampling_frequency(media_tracker.a2dp_cid, new_sample_rate);
            }
            break;

        default:
            show_usage();
            return;
    }
    if (status != ERROR_CODE_SUCCESS){
        printf("Could not perform command \'%c\', status 0x%02x\n", cmd, status);
    }
}
#endif


int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--addr") && i + 1 < argc){
            strncpy(g_target_addr, argv[++i], sizeof(g_target_addr) - 1);
            g_target_addr[sizeof(g_target_addr) - 1] = 0;
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc){
            ldac_apply_mode(argv[++i]);
        } else if (!strcmp(argv[i], "--scan")){
            g_scan_mode = 1;
        } else if (!strcmp(argv[i], "--endpoint") && i + 1 < argc){
            const char *ep = argv[++i];
            const char *q = ep;
            while (*q >= '0' && *q <= '9') q++;
            if (!strcasecmp(ep, "auto")) {
                g_endpoint_auto = 1;
            } else if (*ep && *q == 0) {
                g_endpoint_index = atoi(ep);      // all digits -> index
            } else {
                strncpy(g_endpoint_name, ep, sizeof(g_endpoint_name) - 1);
                g_endpoint_name[sizeof(g_endpoint_name) - 1] = 0;
            }
        } else if (!strcmp(argv[i], "--list-endpoints")){
            wa_capture_list();
            exit(0);
        } else if (!strcmp(argv[i], "--source") && i + 1 < argc){
            const char *src = argv[++i];
            if (!strcasecmp(src, "system")) g_source_system = 1;
            else if (!strcasecmp(src, "tone")) g_source_system = 0;
            else printf("Unknown source '%s' (tone|system)\n", src);
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc){
            int g = atoi(argv[++i]);
            g_gain_pct = (g < 0) ? 0 : (g > 100 ? 100 : g);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")){
            printf("Usage: a2dp_source_demo [--addr AA:BB:CC:DD:EE:FF] [--mode HQ|SQ|MQ] [--gain 0-100] [--source tone|system]\n");
            printf("  --source  tone = generated sine (default), system = WASAPI loopback\n");
            printf("  --endpoint N|name|auto  render endpoint: index, name substring, or auto (default)\n");
            printf("  --scan    list nearby Bluetooth devices, then exit\n");
            printf("  --gain  test-tone level in percent (default 12)\n");
            printf("  env: LDAC_ADDR, LDAC_MODE, LDAC_GAIN, LDAC_SOURCE\n");
            exit(0);
        } else {
            printf("Unknown argument '%s' (try --help)\n", argv[i]);
        }
    }
    const char *e_addr = getenv("LDAC_ADDR"); if (e_addr && *e_addr) strncpy(g_target_addr, e_addr, sizeof(g_target_addr) - 1);
    ldac_apply_mode(getenv("LDAC_MODE"));
    const char *e_src = getenv("LDAC_SOURCE");
    if (e_src && *e_src) g_source_system = !strcasecmp(e_src, "system");
    const char *e_gain = getenv("LDAC_GAIN");
    if (e_gain && *e_gain){ int g = atoi(e_gain); g_gain_pct = (g < 0) ? 0 : (g > 100 ? 100 : g); }
    
    if (g_source_system){
        int cap;
        if (g_endpoint_name[0])          cap = wa_capture_start_named(g_endpoint_name);
        else if (g_endpoint_index >= 0)  cap = wa_capture_start_ex(g_endpoint_index);
        else                            cap = wa_capture_start_auto();   // follow the audio
        if (cap == 0) {
            unsigned int du, dc, dr, dch, ds, dp;
            char ep_name[128] = "(unknown)";
            wa_capture_stats(&du, &dc, &dr, &dch, &ds, &dp, ep_name, sizeof(ep_name));
            printf("AUDIO: capturing system audio from '%s' (%u Hz) via WASAPI loopback\n", ep_name, dr);
        } else {
            printf("AUDIO: capture failed, falling back to test tone\n");
            g_source_system = 0;
        }
    }

    int err = a2dp_source_and_avrcp_services_init();
    if (err) return err;

    // (stale Windows link key injection removed: the headset is bonded
    // with this stack already, the key lives in BTstack's own store)
    // turn on!
    hci_power_control(HCI_POWER_ON);
    return 0;
}
/* EXAMPLE_END */
