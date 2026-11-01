/* kmstreamd.c - KMStream Server Main Loop & Orchestration */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

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
    fprintf(stderr,
            "Usage: %s [--fps <FPS>] [--bitrate <Mbps>] [--dump <file.h264>] [--frames <N>] <PI_IP> [/dev/dri/card1] [1080|720]\n",
            prog);
#else
    fprintf(stderr,
            "Usage: %s [--fps <FPS>] [--bitrate <Mbps>] <PI_IP> [/dev/dri/card1] [1080|720]\n",
            prog);
#endif
}

int main(int argc, char **argv) {
#if KMSTREAM_STATS
    const char *dump_path = NULL;
    uint32_t max_frames = 0;
#endif
    int bitrate_mbps = 0;
    int target_fps = 0;

    static const struct option long_opts[] = {
        { "fps",     required_argument, NULL, 'f' },
        { "bitrate", required_argument, NULL, 'b' },
#if KMSTREAM_STATS
        { "dump",    required_argument, NULL, 'd' },
        { "frames",  required_argument, NULL, 'n' },
#endif
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

#if KMSTREAM_STATS
    const char *optstr = "f:b:d:n:h";
#else
    const char *optstr = "f:b:h";
#endif

    int opt;
    while ((opt = getopt_long(argc, argv, optstr, long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f': target_fps = atoi(optarg); break;
        case 'b': bitrate_mbps = atoi(optarg); break;
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

    if (argc - optind < 1) {
        usage(argv[0]);
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const char *pi_ip = argv[optind];
    const char *card = (argc - optind > 1) ? argv[optind + 1] : "/dev/dri/card1";
    int use_720p = (argc - optind > 2 && atoi(argv[optind + 2]) == 720) ? 1 : 0;
    if (target_fps <= 0) target_fps = use_720p ? 60 : 50;
    uint64_t frame_interval_us = 1000000ULL / (uint64_t)target_fps;

#if KMSTREAM_STATS
    FILE *dump_fp = NULL;
    if (dump_path) {
        dump_fp = fopen(dump_path, "wb");
        DIE_IF(!dump_fp, "failed to open dump file '%s': %s", dump_path, strerror(errno));
    }
#endif

    struct net_tx_ctx net;
    net_tx_init(&net, pi_ip);

    struct kms_capture_ctx cap;
    kms_capture_init(&cap, card);

    int enc_w = use_720p ? 1280 : cap.src_w;
    int enc_h = use_720p ? 720  : cap.src_h;

    struct vaapi_enc_ctx enc;
    VADRMPRIMESurfaceDescriptor prime;
    vaapi_enc_init(&enc, enc_w, enc_h, target_fps, bitrate_mbps, &prime);

    struct egl_convert_ctx egl;
    egl_convert_init(&egl, enc_w, enc_h, &prime);
    vaapi_enc_close_prime(&prime);

    printf("=== KMStream Server (%s, hdr=%zub, %s @ %d FPS) (%dx%d -> %dx%d) | Primary: %u | Cursor: %u ===\n",
           KMSTREAM_STATS ? "STATS" : "RELEASE",
           sizeof(struct kms_video_hdr),
           bitrate_mbps > 0 ? "VBR" : (use_720p ? "CQP24" : "CQP26"),
           target_fps, cap.src_w, cap.src_h, enc_w, enc_h,
           cap.primary_plane_id, cap.cursor_plane_id);
    fflush(stdout);

    uint32_t frame_id = 1;
    uint64_t next_frame_tick = now_us();

#if KMSTREAM_STATS
    uint64_t stat_timer = next_frame_tick;
    struct kms_stat_win st_conv = {0}, st_enc = {0}, st_tx = {0}, st_sz = {0};
#endif

    while (!g_stop
#if KMSTREAM_STATS
           && (!max_frames || frame_id <= max_frames)
#endif
    ) {
        net_tx_wait_until(&net, next_frame_tick, &g_stop);
        if (g_stop) break;

        uint64_t now = now_us();
        next_frame_tick += frame_interval_us;
        if (now > next_frame_tick + 30000) next_frame_tick = now;

#if KMSTREAM_STATS
        uint64_t t_cap0 = now;
#endif

        struct kms_captured_plane prim, cur;
        if (kms_capture_acquire(&cap, &prim, &cur) < 0) {
            usleep(2000);
            continue;
        }

        if (egl_convert_render(&egl, &prim, &cur) < 0) {
            kms_capture_release_plane(&cur);
            kms_capture_release_plane(&prim);
            continue;
        }

#if KMSTREAM_STATS
        uint64_t t_conv_end = now_us();
        uint64_t conv_us = t_conv_end - t_cap0;
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
                              t_cap0, conv_us, enc_us);
            uint64_t t_tx_end = now_us();
            uint64_t tx_us = t_tx_end - t_enc_end;

            kms_stat_add(&st_conv, (uint32_t)conv_us);
            kms_stat_add(&st_enc,  (uint32_t)enc_us);
            kms_stat_add(&st_tx,   (uint32_t)tx_us);
            kms_stat_add(&st_sz,   (uint32_t)send_size);
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
            uint32_t conv_avg, conv_p99, enc_avg, enc_p99, tx_avg, tx_p99, sz_avg, sz_p99;
            double fps = st_conv.count * 1000000.0 / (double)(t_log - stat_timer);
            kms_stat_compute(&st_conv, &conv_avg, &conv_p99);
            kms_stat_compute(&st_enc,  &enc_avg,  &enc_p99);
            kms_stat_compute(&st_tx,   &tx_avg,   &tx_p99);
            kms_stat_compute(&st_sz,   &sz_avg,   &sz_p99);

            if (net.has_pi_stats) {
                printf("[PC] %4.1f FPS | Size: %4.1f/%4.1f KB | Conv: %.2f/%.2f ms | Enc: %.2f/%.2f ms | TX: %.2f/%.2f ms (err:%u idr_req:%u) | Pi E2E: %.2f/%.2f ms (inc:%u)\n",
                       fps,
                       sz_avg / 1024.0, sz_p99 / 1024.0,
                       conv_avg / 1000.0, conv_p99 / 1000.0,
                       enc_avg / 1000.0, enc_p99 / 1000.0,
                       tx_avg / 1000.0, tx_p99 / 1000.0,
                       net.tx_errors, net.idr_reqs_total,
                       net.last_pi_stats.e2e_us_avg / 1000.0, net.last_pi_stats.e2e_us_p99 / 1000.0,
                       net.last_pi_stats.frames_lost);
            } else {
                printf("[PC] %4.1f FPS | Size: %4.1f/%4.1f KB | Conv: %.2f/%.2f ms | Enc: %.2f/%.2f ms | TX: %.2f/%.2f ms (err:%u idr_req:%u)\n",
                       fps,
                       sz_avg / 1024.0, sz_p99 / 1024.0,
                       conv_avg / 1000.0, conv_p99 / 1000.0,
                       enc_avg / 1000.0, enc_p99 / 1000.0,
                       tx_avg / 1000.0, tx_p99 / 1000.0,
                       net.tx_errors, net.idr_reqs_total);
            }
            fflush(stdout);
            kms_stat_reset(&st_conv);
            kms_stat_reset(&st_enc);
            kms_stat_reset(&st_tx);
            kms_stat_reset(&st_sz);
            stat_timer = t_log;
        }
#endif
    }

#if KMSTREAM_STATS
    if (dump_fp) fclose(dump_fp);
#endif
    net_tx_cleanup(&net);
    if (cap.drm_fd >= 0) close(cap.drm_fd);
    printf("[kmstreamd] Stopped cleanly.\n");
    return 0;
}
