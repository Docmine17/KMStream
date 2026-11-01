#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "protocol.h"
#if KMSTREAM_STATS
#include "stats.h"
#endif
#include "net_rx.h"
#include "decode_v4l2.h"
#include "display_kms.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

#if KMSTREAM_STATS
static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct display_kms_ctx kms;
    display_kms_init(&kms);

    struct decode_v4l2_ctx dec;
    decode_v4l2_init(&dec);

    struct rx_shared *sh = net_rx_init_and_start(&g_stop);

    printf("[KMStream-Pi] Ready on UDP :%d (%s, hdr=%zub, SO_RCVBUF=%d KB, %d-slot ring, Atomic KMS + FB cache)\n",
           KMS_VIDEO_PORT,
           KMSTREAM_STATS ? "STATS" : "RELEASE",
           sizeof(struct kms_video_hdr),
           sh->actual_rcvbuf / 1024, RX_RING_SLOTS);
    fflush(stdout);

#if KMSTREAM_STATS
    struct frame_meta_slot meta_ring[32];
    memset(meta_ring, 0, sizeof(meta_ring));

    struct kms_stat_win st_conv, st_enc, st_rx_dur, st_net, st_dec, st_kms, st_e2e;
    kms_stat_reset(&st_conv);
    kms_stat_reset(&st_enc);
    kms_stat_reset(&st_rx_dur);
    kms_stat_reset(&st_net);
    kms_stat_reset(&st_dec);
    kms_stat_reset(&st_kms);
    kms_stat_reset(&st_e2e);

    uint64_t t_window_start_us = now_us();
    uint32_t frames_displayed  = 0;
#endif

    int is_blanked = 1;

    while (!g_stop) {
        int ready_idx = -1;
        int need_reset = 0;

        pthread_mutex_lock(&sh->mtx);
        while (!g_stop) {
            if (sh->stream_reset_pending) {
                sh->stream_reset_pending = 0;
                need_reset = 1;
                break;
            }

            /* 1. If a newer IDR frame is ready in the ring, discard any ready frames older than that IDR */
            uint32_t newest_idr_id = 0;
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY && (sh->slots[i].flags & KMS_FLAG_IDR)) {
                    if (sh->slots[i].frame_id > newest_idr_id) {
                        newest_idr_id = sh->slots[i].frame_id;
                    }
                }
            }
            if (newest_idr_id > 0) {
                for (int i = 0; i < RX_RING_SLOTS; i++) {
                    if (sh->slots[i].state == SLOT_READY && sh->slots[i].frame_id < newest_idr_id) {
                        sh->slots[i].state = SLOT_FREE;
                    }
                }
            }

            /*
             * 2. Pick the OLDEST ready frame (FIFO order) so we NEVER skip an IDR or
             * break the H.264 P-frame reference chain. If more than 3 P-frames pile up,
             * drop the backlog and request a fresh IDR from the server.
             */
            int ready_cnt = 0;
            uint32_t oldest_id = UINT32_MAX;
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY) {
                    ready_cnt++;
                    if (sh->slots[i].frame_id < oldest_id) {
                        oldest_id = sh->slots[i].frame_id;
                        ready_idx = i;
                    }
                }
            }
            if (ready_cnt > 3 && newest_idr_id == 0) {
                for (int i = 0; i < RX_RING_SLOTS; i++) {
                    if (sh->slots[i].state == SLOT_READY) {
                        sh->slots[i].state = SLOT_FREE;
                    }
                }
                ready_idx = -1;
                net_rx_request_idr_locked(sh);
            }
            if (ready_idx >= 0) {
                sh->slots[ready_idx].state = SLOT_SENDING;
                break;
            }
            int wret = net_rx_wait_for_ready(sh, &g_stop, is_blanked);
            if (wret < 0) {
                break;
            }
            if (wret == 0) {
                /* 2s without packets -> auto-blank KMS plane */
                pthread_mutex_unlock(&sh->mtx);
                display_kms_disable_plane(&kms);
                is_blanked = 1;
                pthread_mutex_lock(&sh->mtx);
            }
        }

        if (g_stop) {
            pthread_mutex_unlock(&sh->mtx);
            break;
        }

        if (need_reset) {
            pthread_mutex_unlock(&sh->mtx);
            /* Strict teardown order: disable KMS + invalidate fb_cache + free frame_ring BEFORE V4L2 reset */
            display_kms_disable_plane(&kms);
            is_blanked = 1;
            decode_v4l2_reset(&dec);

            /*
             * After V4L2 reset (~15 ms), check if an IDR frame is already waiting in the ring;
             * if not (e.g. P-frames arrived while reopening /dev/video10), flush non-IDR slots
             * and immediately request a fresh IDR from the server.
             */
            pthread_mutex_lock(&sh->mtx);
            int has_idr_ready = 0;
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY && (sh->slots[i].flags & KMS_FLAG_IDR)) {
                    has_idr_ready = 1;
                } else if (sh->slots[i].state == SLOT_READY) {
                    sh->slots[i].state = SLOT_FREE;
                }
            }
            if (!has_idr_ready) {
                net_rx_request_idr_locked(sh);
            }
            pthread_mutex_unlock(&sh->mtx);

#if KMSTREAM_STATS
            memset(meta_ring, 0, sizeof(meta_ring));
            kms_stat_reset(&st_conv);
            kms_stat_reset(&st_enc);
            kms_stat_reset(&st_rx_dur);
            kms_stat_reset(&st_net);
            kms_stat_reset(&st_dec);
            kms_stat_reset(&st_kms);
            kms_stat_reset(&st_e2e);
            frames_displayed = 0;
            t_window_start_us = now_us();
#endif
            continue;
        }

        struct rx_slot *s = &sh->slots[ready_idx];
        uint32_t cur_frame_id = s->frame_id;
        uint32_t cur_size     = s->frame_size;
#if KMSTREAM_STATS
        uint64_t cap_ts_us    = s->capture_ts_us;
        uint64_t rx_first_us  = s->t_rx_first_us;
        uint64_t rx_last_us   = s->t_rx_last_us;
        uint16_t t_conv_us    = s->t_convert_us;
        uint16_t t_enc_us     = s->t_encode_us;
#endif
        pthread_mutex_unlock(&sh->mtx);

#if KMSTREAM_STATS
        struct frame_meta_slot *ms = &meta_ring[cur_frame_id % 32];
        ms->frame_id      = cur_frame_id;
        ms->capture_ts_us = cap_ts_us;
        ms->t_rx_first_us = rx_first_us;
        ms->t_convert_us  = t_conv_us;
        ms->t_encode_us   = t_enc_us;

        uint64_t t_dec_start = now_us();
        uint64_t t_send_done = t_dec_start;
        AVFrame *latest_frame = decode_v4l2_step(&dec, s->data, cur_size,
                                                 cur_frame_id, &t_send_done);
#else
        AVFrame *latest_frame = decode_v4l2_step(&dec, s->data, cur_size,
                                                 cur_frame_id, NULL);
#endif

        pthread_mutex_lock(&sh->mtx);
        s->state = SLOT_FREE;
        pthread_mutex_unlock(&sh->mtx);

        if (latest_frame) {
#if KMSTREAM_STATS
            uint64_t t_dec_done = now_us();
            uint32_t out_id = (uint32_t)latest_frame->pts;
            int32_t  held_frames = (int32_t)(cur_frame_id - out_id);

            struct frame_meta_slot *out_ms = &meta_ring[out_id % 32];
            int meta_valid = (out_ms->frame_id == out_id);
#endif
            if (display_kms_present(&kms, latest_frame) == 0) {
                is_blanked = 0;
#if KMSTREAM_STATS
                uint64_t t_kms_done = now_us();

                kms_stat_add(&st_rx_dur, (uint32_t)(rx_last_us - rx_first_us));
                kms_stat_add(&st_dec,    (uint32_t)(t_dec_done - t_dec_start));
                kms_stat_add(&st_kms,    (uint32_t)(t_kms_done - t_dec_done));

                pthread_mutex_lock(&sh->mtx);
                int synced         = sh->cs.synced;
                int64_t offset_us  = sh->cs.offset_us;
                int64_t min_rtt_us = sh->cs.min_rtt_us;
                uint32_t inc_cnt   = sh->frames_incomplete;
                uint32_t idr_cnt   = sh->idr_requests_sent;
                pthread_mutex_unlock(&sh->mtx);

                if (meta_valid) {
                    kms_stat_add(&st_conv, out_ms->t_convert_us);
                    kms_stat_add(&st_enc,  out_ms->t_encode_us);
                    if (synced && out_ms->capture_ts_us > 0) {
                        int64_t e2e_us = (int64_t)t_kms_done + offset_us - (int64_t)out_ms->capture_ts_us;
                        int64_t net_us = (int64_t)t_dec_start + offset_us -
                                         ((int64_t)ms->capture_ts_us + ms->t_convert_us + ms->t_encode_us);
                        if (e2e_us > 0 && e2e_us < 1000000) kms_stat_add(&st_e2e, (uint32_t)e2e_us);
                        if (net_us > 0 && net_us < 500000)  kms_stat_add(&st_net, (uint32_t)net_us);
                        else if (net_us <= 0 && net_us > -5000) kms_stat_add(&st_net, 0);
                    }
                }

                frames_displayed++;
                if (frames_displayed >= 60) {
                    double elapsed_s = (double)(t_kms_done - t_window_start_us) / 1e6;
                    double fps = (double)frames_displayed / elapsed_s;

                    uint32_t avg_conv, p99_conv, avg_enc, p99_enc, avg_rx, p99_rx;
                    uint32_t avg_net, p99_net, avg_dec, p99_dec, avg_kms, p99_kms, avg_e2e, p99_e2e;

                    kms_stat_compute(&st_conv,   &avg_conv, &p99_conv);
                    kms_stat_compute(&st_enc,    &avg_enc,  &p99_enc);
                    kms_stat_compute(&st_rx_dur, &avg_rx,   &p99_rx);
                    kms_stat_compute(&st_net,    &avg_net,  &p99_net);
                    kms_stat_compute(&st_dec,    &avg_dec,  &p99_dec);
                    kms_stat_compute(&st_kms,    &avg_kms,  &p99_kms);
                    kms_stat_compute(&st_e2e,    &avg_e2e,  &p99_e2e);

                    printf("[Metrics] %.1f FPS | avg/p99(ms) Conv:%.2f/%.2f Enc:%.2f/%.2f Net+Rx:%.2f/%.2f(span:%.2f) Dec:%.2f/%.2f(send:%.2f) KMS:%.2f/%.2f | E2E:%.2f/%.2f ms (Hold:%df RTT:%.2fms Loss:%u IDR:%u FB:%u/%u)\n",
                           fps,
                           avg_conv / 1000.0, p99_conv / 1000.0,
                           avg_enc  / 1000.0, p99_enc  / 1000.0,
                           avg_net  / 1000.0, p99_net  / 1000.0, avg_rx / 1000.0,
                           avg_dec  / 1000.0, p99_dec  / 1000.0, (double)(t_send_done - t_dec_start) / 1000.0,
                           avg_kms  / 1000.0, p99_kms  / 1000.0,
                           avg_e2e  / 1000.0, p99_e2e  / 1000.0,
                           held_frames,
                           (double)min_rtt_us / 1000.0,
                           inc_cnt, idr_cnt,
                           kms.cache_hits, kms.cache_misses);
                    fflush(stdout);

                    kms_stat_reset(&st_conv);
                    kms_stat_reset(&st_enc);
                    kms_stat_reset(&st_rx_dur);
                    kms_stat_reset(&st_net);
                    kms_stat_reset(&st_dec);
                    kms_stat_reset(&st_kms);
                    kms_stat_reset(&st_e2e);

                    frames_displayed = 0;
                    t_window_start_us = t_kms_done;
                }
#endif
            }
        }
    }

    /* Strict teardown order: KMS disable + FB cache invalidation + frame_ring unref BEFORE V4L2 cleanup */
    display_kms_cleanup(&kms);
    decode_v4l2_cleanup(&dec);
    printf("[KMStream-Pi] Stopped cleanly.\n");
    return 0;
}
