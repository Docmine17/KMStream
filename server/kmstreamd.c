/* kmstreamd.c - KMStream Server Daemon */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>

#include "protocol.h"
#if KMSTREAM_STATS
#include "stats.h"
#endif
#include "capture_kms.h"
#include "convert_egl.h"
#include "encode_vaapi.h"
#include "net_tx.h"

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

#define CLIENT_TIMEOUT_US 2500000ULL /* 2.5s without client keepalive -> return to idle */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void usage(const char *prog) {
#if KMSTREAM_STATS
    fprintf(stderr, "Usage: %s [--dump <file.h264>] [--frames <N>] [/dev/dri/card1]\n", prog);
#else
    fprintf(stderr, "Usage: %s [/dev/dri/card1]\n", prog);
#endif
}

int main(int argc, char **argv) {
    /* Enable Mesa radeonsi low-latency VCE encoding before opening DRM/GBM/VAAPI (Sunshine parity) */
    setenv("AMD_DEBUG", "lowlatencyenc", 1);

#if KMSTREAM_STATS
    const char *dump_path = NULL;
    uint32_t max_frames = 0;
#endif

    static const struct option long_opts[] = {
#if KMSTREAM_STATS
        { "dump",   required_argument, NULL, 'd' },
        { "frames", required_argument, NULL, 'n' },
#endif
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

#if KMSTREAM_STATS
    const char *optstr = "d:n:h";
#else
    const char *optstr = "h";
#endif

    int opt;
    while ((opt = getopt_long(argc, argv, optstr, long_opts, NULL)) != -1) {
        switch (opt) {
#if KMSTREAM_STATS
        case 'd': dump_path = optarg; break;
        case 'n': max_frames = (uint32_t)strtoul(optarg, NULL, 10); break;
#endif
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    const char *card = (argc - optind >= 1) ? argv[optind] : "/dev/dri/card1";

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

#if KMSTREAM_STATS
    FILE *dump_fp = NULL;
    if (dump_path) {
        dump_fp = fopen(dump_path, "wb");
        DIE_IF(!dump_fp, "failed to open dump file '%s': %s", dump_path, strerror(errno));
    }
#endif

    struct net_tx_ctx net;
    net_tx_init(&net);

    struct kms_capture_ctx cap;
    kms_capture_init(&cap, card);

    struct egl_convert_ctx egl;
    egl_convert_init_device(&egl);

    struct vaapi_enc_ctx enc;
    memset(&enc, 0, sizeof(enc));

    printf("=== KMStream Server Daemon (%s, proto=v%u, hdr=%zub) | Source: %s %dx%d @ %.2f Hz | Waiting on UDP :%d ===\n",
           KMSTREAM_STATS ? "STATS" : "RELEASE",
           KMS_PROTO_VERSION,
           sizeof(struct kms_video_hdr),
           cap.conn_name, cap.src_w, cap.src_h, cap.src_hz,
           KMS_CTRL_PORT);
    fflush(stdout);

    while (!g_stop) {
        net_tx_wait_idle(&net, &g_stop);
        if (g_stop) break;

        net.start_requested  = 0;
        net.stop_requested   = 0;
        net.client_connected = 1;
        net.tx_errors        = 0;
        net.idr_reqs_total   = 0;
#if KMSTREAM_STATS
        net.has_pi_stats     = 0;
#endif

        int     enc_w        = net.req_width;
        int     enc_h        = net.req_height;
        int     target_fps   = net.req_fps;
        int     bitrate_mbps = net.req_bitrate_mbps;
        uint8_t rc_mode      = net.req_rc_mode;
        uint8_t strict_rc    = net.req_strict_rc;
        uint8_t cqp_val      = net.req_cqp;

        VADRMPRIMESurfaceDescriptor prime;
        vaapi_enc_init(&enc, enc_w, enc_h, target_fps, bitrate_mbps,
                       rc_mode, strict_rc, cqp_val, &prime);
        egl_convert_bind_prime(&egl, enc_w, enc_h, &prime);
        vaapi_enc_close_prime(&prime);

        char client_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &net.pi_addr.sin_addr, client_ip, sizeof(client_ip));
        const char *rc_str = (rc_mode == KMS_RC_CQP) ? "CQP" : ((rc_mode == KMS_RC_VBR) ? "VBR" : "CBR");
        printf("[kmstreamd] Session started for %s: Source %dx%d @ %.2f Hz -> Stream %dx%d @ %d FPS (%s",
               client_ip, cap.src_w, cap.src_h, cap.src_hz, enc_w, enc_h, target_fps, rc_str);
        if (rc_mode == KMS_RC_CQP) {
            printf(" QP=%u)\n", cqp_val);
        } else {
            printf(" %d Mbps%s)\n", bitrate_mbps, strict_rc ? ", Strict 1-Frame VBV" : ", Standard VBV");
        }
        fflush(stdout);

        uint32_t frame_id          = 1;
        uint64_t session_anchor_us = now_us();
        uint64_t tick_idx          = 0;
        uint64_t next_frame_tick   = session_anchor_us;
        uint64_t frame_interval_us = 1000000ULL / (uint64_t)target_fps;
        net.force_idr = 1;

#if KMSTREAM_STATS
        uint64_t stat_timer  = session_anchor_us;
        uint64_t prev_cap_us = 0;
        uint64_t bytes_sent_win = 0;
        uint32_t idrs_sent_win  = 0;
        struct kms_stat_win st_dt = {0}, st_cap = {0}, st_conv = {0}, st_enc = {0}, st_tx = {0}, st_sz = {0};
#endif

        while (!g_stop && !net.stop_requested && !net.start_requested
#if KMSTREAM_STATS
               && (!max_frames || frame_id <= max_frames)
#endif
        ) {
            net_tx_pace_frame(&net, next_frame_tick, &g_stop);
            if (g_stop || net.stop_requested || net.start_requested) break;

            uint64_t now = now_us();
            if (now - net.last_client_rx_us > CLIENT_TIMEOUT_US) {
                printf("[kmstreamd] Client %s timed out -> returning to idle.\n", client_ip);
                fflush(stdout);
                break;
            }

            tick_idx++;
            next_frame_tick = session_anchor_us + (tick_idx * 1000000ULL) / (uint64_t)target_fps;
            if (now > next_frame_tick + frame_interval_us) {
                session_anchor_us = now;
                tick_idx          = 1;
                next_frame_tick   = session_anchor_us + frame_interval_us;
            }

#if KMSTREAM_STATS
            uint64_t t_cap0 = now;
            if (prev_cap_us > 0) {
                kms_stat_add(&st_dt, (uint32_t)(t_cap0 - prev_cap_us));
            }
            prev_cap_us = t_cap0;
#endif

            struct kms_captured_plane prim, cur;
            if (kms_capture_acquire(&cap, &prim, &cur) < 0) {
                usleep(1000);
                continue;
            }

#if KMSTREAM_STATS
            uint64_t t_cap_end = now_us();
            uint64_t cap_us = t_cap_end - t_cap0;
#endif

            if (egl_convert_render(&egl, &prim, &cur) < 0) {
                kms_capture_release_plane(&cur);
                kms_capture_release_plane(&prim);
                continue;
            }

#if KMSTREAM_STATS
            uint64_t t_conv_end = now_us();
            uint64_t conv_us = t_conv_end - t_cap_end;
            uint64_t total_cap_conv_us = t_conv_end - t_cap0;
#endif

            net_tx_drain_ctrl(&net);

            int force_idr = net.force_idr;
            net.force_idr = 0;
            vaapi_enc_submit(&enc, frame_id, force_idr);

            const uint8_t *send_ptr = NULL;
            int send_size = 0;
            uint8_t flags = 0;

            while (vaapi_enc_receive(&enc, &send_ptr, &send_size, &flags) == 0) {
#if KMSTREAM_STATS
                uint64_t t_enc_end = now_us();
                uint64_t enc_us = t_enc_end - t_conv_end;

                if (dump_fp) {
                    fwrite(send_ptr, 1, (size_t)send_size, dump_fp);
                    fflush(dump_fp);
                }

                net_tx_send_frame(&net, frame_id, send_ptr, send_size, flags,
                                  t_cap0, total_cap_conv_us, enc_us);
                uint64_t t_tx_end = now_us();
                uint64_t tx_us = t_tx_end - t_enc_end;

                kms_stat_add(&st_cap,  (uint32_t)cap_us);
                kms_stat_add(&st_conv, (uint32_t)conv_us);
                kms_stat_add(&st_enc,  (uint32_t)enc_us);
                kms_stat_add(&st_tx,   (uint32_t)tx_us);
                kms_stat_add(&st_sz,   (uint32_t)send_size);
                bytes_sent_win += (uint64_t)send_size;
                if (flags & KMS_FLAG_IDR) idrs_sent_win++;
#else
                net_tx_send_frame(&net, frame_id, send_ptr, send_size, flags, 0, 0, 0);
#endif
                frame_id++;
                vaapi_enc_unref_pkt(&enc);
            }

            kms_capture_release_plane(&cur);
            kms_capture_release_plane(&prim);
            net_tx_drain_ctrl(&net);

#if KMSTREAM_STATS
            uint64_t t_log = now_us();
            if (t_log - stat_timer >= 2000000ULL && st_conv.count > 0) {
                uint32_t dt_avg, dt_p99, cap_avg, cap_p99, conv_avg, conv_p99;
                uint32_t enc_avg, enc_p99, tx_avg, tx_p99, sz_avg, sz_p99;
                double elapsed_s = (double)(t_log - stat_timer) / 1000000.0;
                double fps       = (double)st_conv.count / elapsed_s;
                double mbps      = ((double)bytes_sent_win * 8.0) / (elapsed_s * 1000000.0);

                kms_stat_compute(&st_dt,   &dt_avg,   &dt_p99);
                kms_stat_compute(&st_cap,  &cap_avg,  &cap_p99);
                kms_stat_compute(&st_conv, &conv_avg, &conv_p99);
                kms_stat_compute(&st_enc,  &enc_avg,  &enc_p99);
                kms_stat_compute(&st_tx,   &tx_avg,   &tx_p99);
                kms_stat_compute(&st_sz,   &sz_avg,   &sz_p99);

                printf("[PC] %5.1f FPS (dt:%.2f/%.2fms) | %5.2f Mbps (Frm:%4.1f/%4.1fKB IDR:%u) | Cap:%.2f/%.2f Conv:%.2f/%.2f Enc:%.2f/%.2f TX:%.2f/%.2f ms (err:%u idr_req:%u)",
                       fps, dt_avg / 1000.0, dt_p99 / 1000.0,
                       mbps, sz_avg / 1024.0, sz_p99 / 1024.0, idrs_sent_win,
                       cap_avg / 1000.0, cap_p99 / 1000.0,
                       conv_avg / 1000.0, conv_p99 / 1000.0,
                       enc_avg / 1000.0, enc_p99 / 1000.0,
                       tx_avg / 1000.0, tx_p99 / 1000.0,
                       net.tx_errors, net.idr_reqs_total);
                if (net.has_pi_stats) {
                    printf(" || [Pi] E2E:%.2f/%.2f Dec:%.2f/%.2f Flip:%.2f/%.2f ms (loss:%u drop:%u mbox:%u)\n",
                           net.last_pi_stats.e2e_us_avg / 1000.0, net.last_pi_stats.e2e_us_p99 / 1000.0,
                           net.last_pi_stats.decode_us_avg / 1000.0, net.last_pi_stats.decode_us_p99 / 1000.0,
                           net.last_pi_stats.display_us_avg / 1000.0, net.last_pi_stats.display_us_p99 / 1000.0,
                           net.last_pi_stats.frames_lost, net.last_pi_stats.rx_drops,
                           net.last_pi_stats.mbox_skips);
                } else {
                    printf("\n");
                }
                fflush(stdout);

                kms_stat_reset(&st_dt);
                kms_stat_reset(&st_cap);
                kms_stat_reset(&st_conv);
                kms_stat_reset(&st_enc);
                kms_stat_reset(&st_tx);
                kms_stat_reset(&st_sz);
                bytes_sent_win = 0;
                idrs_sent_win  = 0;
                stat_timer     = t_log;
            }
#endif
        }

        egl_convert_unbind_prime(&egl);
        vaapi_enc_cleanup(&enc);

        if (g_stop) {
            net_tx_send_stop(&net);
            break;
        }
#if KMSTREAM_STATS
        if (max_frames && frame_id > max_frames) {
            net_tx_send_stop(&net);
            break;
        }
#endif
        if (!net.start_requested) {
            net.client_connected = 0;
            printf("[kmstreamd] Session ended. Waiting for client on UDP :%d...\n", KMS_CTRL_PORT);
            fflush(stdout);
        }
    }

#if KMSTREAM_STATS
    if (dump_fp) fclose(dump_fp);
#endif
    vaapi_enc_cleanup(&enc);
    egl_convert_cleanup(&egl);
    kms_capture_cleanup(&cap);
    net_tx_cleanup(&net);
    printf("[kmstreamd] Stopped cleanly.\n");
    return 0;
}
