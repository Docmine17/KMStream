#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "protocol.h"
#include "stats.h"
#include "net_rx.h"
#include "decode_v4l2.h"
#include "display_kms.h"

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(void) {
    struct display_kms_ctx kms;
    display_kms_init(&kms);

    struct decode_v4l2_ctx dec;
    decode_v4l2_init(&dec);

    struct rx_shared *sh = net_rx_init_and_start();

    printf("[KMStream-Pi] Ready on UDP :%d (SO_RCVBUF=%d KB, %d-slot bitmap ring, Atomic KMS + FB cache)\n",
           KMS_VIDEO_PORT, sh->actual_rcvbuf / 1024, RX_RING_SLOTS);
    fflush(stdout);

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

    while (1) {
        int ready_idx = -1;

        pthread_mutex_lock(&sh->mtx);
        while (1) {
            uint32_t best_id = 0;
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY) {
                    if (ready_idx < 0 || (int32_t)(sh->slots[i].frame_id - best_id) > 0) {
                        ready_idx = i;
                        best_id   = sh->slots[i].frame_id;
                    }
                }
            }
            if (ready_idx >= 0) {
                for (int i = 0; i < RX_RING_SLOTS; i++) {
                    if (i != ready_idx && sh->slots[i].state == SLOT_READY) {
                        sh->slots[i].state = SLOT_FREE;
                    }
                }
                sh->slots[ready_idx].state = SLOT_SENDING;
                break;
            }
            net_rx_wait_for_ready(sh);
        }

        struct rx_slot *s = &sh->slots[ready_idx];
        uint32_t cur_frame_id  = s->frame_id;
        uint32_t cur_size      = s->frame_size;
        uint64_t cap_ts_us     = s->capture_ts_us;
        uint64_t rx_first_us   = s->t_rx_first_us;
        uint64_t rx_last_us    = s->t_rx_last_us;
        uint16_t t_conv_us     = s->t_convert_us;
        uint16_t t_enc_us      = s->t_encode_us;
        pthread_mutex_unlock(&sh->mtx);

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

        pthread_mutex_lock(&sh->mtx);
        s->state = SLOT_FREE;
        pthread_mutex_unlock(&sh->mtx);

        if (latest_frame) {
            uint64_t t_dec_done = now_us();
            uint32_t out_id = (uint32_t)latest_frame->pts;
            int32_t  held_frames = (int32_t)(cur_frame_id - out_id);

            struct frame_meta_slot *out_ms = &meta_ring[out_id % 32];
            int meta_valid = (out_ms->frame_id == out_id);

            if (display_kms_present(&kms, latest_frame) == 0) {
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
            }
        }
    }

    decode_v4l2_cleanup(&dec);
    display_kms_cleanup(&kms);
    return 0;
}
