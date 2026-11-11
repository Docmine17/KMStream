#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>

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

static void configure_realtime_thread(pthread_t thread, int target_priority) {
    int min_prio = sched_get_priority_min(SCHED_FIFO);
    int max_prio = sched_get_priority_max(SCHED_FIFO);
    if (min_prio < 0 || max_prio < 0) {
        min_prio = 1;
        max_prio = 99;
    }
    if (target_priority < min_prio) target_priority = min_prio;
    if (target_priority > max_prio) target_priority = max_prio;

    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = target_priority;
    pthread_setschedparam(thread, SCHED_FIFO, &param);
}

struct client_app_ctx {
    struct decode_v4l2_ctx *dec;
    struct display_kms_ctx *kms;
    struct rx_shared       *sh;
#if KMSTREAM_STATS
    struct frame_meta_slot  meta_ring[64];
    _Atomic uint32_t        last_submitted_id;
#endif
};

static void *v4l2_event_thread(void *arg) {
    struct client_app_ctx *app = (struct client_app_ctx *)arg;
    struct decode_v4l2_ctx *dec = app->dec;
    struct display_kms_ctx *kms = app->kms;
    struct pollfd pfd[2];

    drmEventContext evctx;
    memset(&evctx, 0, sizeof(evctx));
    evctx.version           = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = display_kms_page_flip_handler;

#if KMSTREAM_STATS
    struct kms_stat_win st_conv, st_enc, st_rx_dur, st_net, st_sub, st_dec, st_flip_wait, st_e2e_dq, st_e2e_flip;
    kms_stat_reset(&st_conv);
    kms_stat_reset(&st_enc);
    kms_stat_reset(&st_rx_dur);
    kms_stat_reset(&st_net);
    kms_stat_reset(&st_sub);
    kms_stat_reset(&st_dec);
    kms_stat_reset(&st_flip_wait);
    kms_stat_reset(&st_e2e_dq);
    kms_stat_reset(&st_e2e_flip);

    uint64_t t_window_start_us = now_us();
    uint32_t prev_rx_completed = 0;
    uint32_t prev_flips_done   = 0;
    uint32_t last_recorded_flip_fid = 0;
#endif

    while (!g_stop) {
        pfd[0].fd      = dec->v4l2_fd;
        pfd[0].events  = POLLIN | POLLPRI;
        pfd[0].revents = 0;

        pfd[1].fd      = kms->drm_fd;
        pfd[1].events  = POLLIN;
        pfd[1].revents = 0;

        int ret = poll(pfd, 2, 50);
        if (ret <= 0 || g_stop) continue;

        /* 1. Handle V4L2 Dynamic Resolution / Format Change */
        if (pfd[0].revents & POLLPRI) {
            struct v4l2_event ev;
            memset(&ev, 0, sizeof(ev));
            if (ioctl(dec->v4l2_fd, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    decode_v4l2_handle_drc(dec, kms);
                    continue;
                }
            }
        }

        /* 2. Drain all newly decoded frames BEFORE handling DRM VBlank events */
        if (pfd[0].revents & POLLIN) {
            int latest_buf_idx = -1;
            uint32_t latest_frame_id = 0;
#if KMSTREAM_STATS
            uint64_t latest_dqbuf_us = 0;
#endif

            while (1) {
                struct v4l2_plane planes[1];
                struct v4l2_buffer buf;
                memset(&buf, 0, sizeof(buf));
                memset(planes, 0, sizeof(planes));
                buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                buf.memory   = V4L2_MEMORY_MMAP;
                buf.length   = 1;
                buf.m.planes = planes;

                if (ioctl(dec->v4l2_fd, VIDIOC_DQBUF, &buf) != 0) break;

                if (buf.m.planes[0].bytesused == 0 || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
                    if (!(buf.flags & V4L2_BUF_FLAG_LAST)) {
                        decode_v4l2_requeue_capture(dec, buf.index);
                    }
                    continue;
                }

                if (latest_buf_idx != -1) {
                    decode_v4l2_requeue_capture(dec, latest_buf_idx);
                }
                latest_buf_idx  = (int)buf.index;
                latest_frame_id = (uint32_t)buf.timestamp.tv_sec;
                dec->capture_bufs[latest_buf_idx].frame_id = latest_frame_id;
#if KMSTREAM_STATS
                latest_dqbuf_us = now_us();
                dec->capture_bufs[latest_buf_idx].t_dqbuf_us = latest_dqbuf_us;
#endif
            }

            if (latest_buf_idx != -1) {
                display_kms_present_idx(kms, latest_buf_idx);
#if KMSTREAM_STATS
                struct frame_meta_slot *ms = &app->meta_ring[latest_frame_id & 63];
                uint32_t slot_fid = atomic_load_explicit(&ms->frame_id, memory_order_acquire);

                if (slot_fid == latest_frame_id && latest_frame_id > 0) {
                    uint64_t t_dec_start = ms->t_dec_start_us;
                    uint64_t t_send_done = atomic_load_explicit(&ms->t_send_done_us, memory_order_relaxed);
                    uint64_t rx_dur_us   = ms->t_rx_last_us - ms->t_rx_first_us;
                    uint32_t sub_us      = (t_send_done > t_dec_start) ? (uint32_t)(t_send_done - t_dec_start) : 0;
                    uint32_t dec_us      = (latest_dqbuf_us > t_send_done) ? (uint32_t)(latest_dqbuf_us - t_send_done) : 0;

                    kms_stat_add(&st_rx_dur, (uint32_t)rx_dur_us);
                    kms_stat_add(&st_sub,    sub_us);
                    kms_stat_add(&st_dec,    dec_us);
                    kms_stat_add(&st_conv,   ms->t_convert_us);
                    kms_stat_add(&st_enc,    ms->t_encode_us);

                    pthread_mutex_lock(&app->sh->mtx);
                    int synced         = app->sh->cs.synced;
                    int64_t offset_us  = app->sh->cs.offset_us;
                    pthread_mutex_unlock(&app->sh->mtx);

                    if (synced && ms->capture_ts_us > 0) {
                        int64_t e2e_dq_us = (int64_t)latest_dqbuf_us + offset_us - (int64_t)ms->capture_ts_us;
                        int64_t net_us    = (int64_t)t_dec_start + offset_us -
                                            ((int64_t)ms->capture_ts_us + ms->t_convert_us + ms->t_encode_us);
                        if (e2e_dq_us > 0 && e2e_dq_us < 1000000) kms_stat_add(&st_e2e_dq, (uint32_t)e2e_dq_us);
                        if (net_us > 0 && net_us < 500000)        kms_stat_add(&st_net, (uint32_t)net_us);
                        else if (net_us <= 0 && net_us > -5000)   kms_stat_add(&st_net, 0);
                    }
                }
#endif
            }
        }

        /* 3. Handle DRM VBlank page-flip events */
        if (pfd[1].revents & POLLIN) {
            drmHandleEvent(kms->drm_fd, &evctx);
#if KMSTREAM_STATS
            pthread_mutex_lock(&kms->drm_lock);
            uint32_t flip_fid     = kms->last_flipped_frame_id;
            uint64_t flip_wait_us = kms->last_flip_wait_us;
            uint64_t flip_ts_us   = kms->last_flip_ts_us;
            pthread_mutex_unlock(&kms->drm_lock);

            if (flip_fid > 0 && flip_fid != last_recorded_flip_fid) {
                last_recorded_flip_fid = flip_fid;
                kms_stat_add(&st_flip_wait, (uint32_t)flip_wait_us);

                struct frame_meta_slot *ms = &app->meta_ring[flip_fid & 63];
                uint32_t slot_fid = atomic_load_explicit(&ms->frame_id, memory_order_acquire);
                if (slot_fid == flip_fid && ms->capture_ts_us > 0) {
                    pthread_mutex_lock(&app->sh->mtx);
                    int synced        = app->sh->cs.synced;
                    int64_t offset_us = app->sh->cs.offset_us;
                    pthread_mutex_unlock(&app->sh->mtx);
                    if (synced) {
                        int64_t e2e_flip_us = (int64_t)flip_ts_us + offset_us - (int64_t)ms->capture_ts_us;
                        if (e2e_flip_us > 0 && e2e_flip_us < 1000000) {
                            kms_stat_add(&st_e2e_flip, (uint32_t)e2e_flip_us);
                        }
                    }
                }
            }
#endif
        }

#if KMSTREAM_STATS
        uint64_t t_now = now_us();
        if (t_now - t_window_start_us >= 2000000ULL && st_dec.count > 0) {
            double elapsed_s = (double)(t_now - t_window_start_us) / 1e6;

            pthread_mutex_lock(&app->sh->mtx);
            int64_t  min_rtt_us = app->sh->cs.min_rtt_us;
            uint32_t rx_comp    = app->sh->frames_completed;
            uint32_t inc_cnt    = app->sh->frames_incomplete;
            uint32_t rx_drops   = app->sh->rx_drops;
            uint32_t idr_cnt    = app->sh->idr_requests_sent;
            pthread_mutex_unlock(&app->sh->mtx);

            pthread_mutex_lock(&kms->drm_lock);
            uint32_t flips_done = kms->flips_completed;
            uint32_t mbox_skips = kms->staged_replacements;
            uint32_t disp_fid   = kms->last_flipped_frame_id;
            pthread_mutex_unlock(&kms->drm_lock);

            double rx_fps   = (double)(rx_comp - prev_rx_completed) / elapsed_s;
            double flip_fps = (double)(flips_done - prev_flips_done) / elapsed_s;
            prev_rx_completed = rx_comp;
            prev_flips_done   = flips_done;

            uint32_t avg_conv, p99_conv, avg_enc, p99_enc, avg_rx, p99_rx, avg_net, p99_net;
            uint32_t avg_sub, p99_sub, avg_dec, p99_dec, avg_flip, p99_flip;
            uint32_t avg_e2e_dq, p99_e2e_dq, avg_e2e_fl, p99_e2e_fl;

            kms_stat_compute(&st_conv,      &avg_conv,   &p99_conv);
            kms_stat_compute(&st_enc,       &avg_enc,    &p99_enc);
            kms_stat_compute(&st_rx_dur,    &avg_rx,     &p99_rx);
            kms_stat_compute(&st_net,       &avg_net,    &p99_net);
            kms_stat_compute(&st_sub,       &avg_sub,    &p99_sub);
            kms_stat_compute(&st_dec,       &avg_dec,    &p99_dec);
            kms_stat_compute(&st_flip_wait, &avg_flip,   &p99_flip);
            kms_stat_compute(&st_e2e_dq,    &avg_e2e_dq, &p99_e2e_dq);
            kms_stat_compute(&st_e2e_flip,  &avg_e2e_fl, &p99_e2e_fl);

            uint32_t sub_id = atomic_load_explicit(&app->last_submitted_id, memory_order_relaxed);
            int32_t held_frames = (disp_fid > 0 && sub_id >= disp_fid) ? (int32_t)(sub_id - disp_fid) : 0;

            printf("[Metrics] RX:%5.1f Flip:%5.1f FPS | Conv:%.2f/%.2f Enc:%.2f/%.2f Net+Rx:%.2f/%.2f(span:%.2f) Sub:%.2f Dec:%.2f/%.2f FlipWait:%.2f/%.2f | E2E(dq):%.2f/%.2f E2E(flip):%.2f/%.2f ms (Hold:%df RTT:%.2fms Loss:%u RxDrop:%u MboxSkip:%u IDR:%u)\n",
                   rx_fps, flip_fps,
                   avg_conv / 1000.0, p99_conv / 1000.0,
                   avg_enc  / 1000.0, p99_enc  / 1000.0,
                   avg_net  / 1000.0, p99_net  / 1000.0, avg_rx / 1000.0,
                   avg_sub  / 1000.0,
                   avg_dec  / 1000.0, p99_dec  / 1000.0,
                   avg_flip / 1000.0, p99_flip / 1000.0,
                   avg_e2e_dq / 1000.0, p99_e2e_dq / 1000.0,
                   avg_e2e_fl / 1000.0, p99_e2e_fl / 1000.0,
                   held_frames,
                   (double)min_rtt_us / 1000.0,
                   inc_cnt, rx_drops, mbox_skips, idr_cnt);
            fflush(stdout);

            struct kms_ctrl_stats st_pkt = {
                .h = {
                    .version  = KMS_PROTO_VERSION,
                    .type     = KMS_CTRL_STATS,
                    .reserved = 0,
                    .seq      = disp_fid,
                },
                .frames_ok      = st_dec.count,
                .frames_lost    = inc_cnt,
                .rx_drops       = rx_drops,
                .e2e_us_avg     = avg_e2e_dq,
                .e2e_us_p99     = p99_e2e_dq,
                .decode_us_avg  = (uint16_t)(avg_dec > 65535 ? 65535 : avg_dec),
                .decode_us_p99  = (uint16_t)(p99_dec > 65535 ? 65535 : p99_dec),
                .display_us_avg = (uint16_t)(avg_flip > 65535 ? 65535 : avg_flip),
                .display_us_p99 = (uint16_t)(p99_flip > 65535 ? 65535 : p99_flip),
                .mbox_skips     = (uint16_t)(mbox_skips > 65535 ? 65535 : mbox_skips),
                .idr_reqs       = (uint16_t)(idr_cnt > 65535 ? 65535 : idr_cnt),
            };
            net_rx_send_stats(app->sh, &st_pkt);

            kms_stat_reset(&st_conv);
            kms_stat_reset(&st_enc);
            kms_stat_reset(&st_rx_dur);
            kms_stat_reset(&st_net);
            kms_stat_reset(&st_sub);
            kms_stat_reset(&st_dec);
            kms_stat_reset(&st_flip_wait);
            kms_stat_reset(&st_e2e_dq);
            kms_stat_reset(&st_e2e_flip);

            t_window_start_us = t_now;
        }
#endif
    }

    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [SERVER_IP] [-720 | -1080 | -w <width> -h <height>] [-fps <fps>] [-bitrate <Mbps>] [-rc cbr|vbr|cqp] [-qp <1-51>] [-strict-rc]\n"
            "Examples:\n"
            "  %s 10.42.0.1 -720 -fps 60 -bitrate 10\n"
            "  %s 10.42.0.1 -1080 -fps 60 -bitrate 20 -rc cbr\n"
            "  %s 10.42.0.1 -1080 -fps 60 -rc cqp -qp 20\n",
            prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    const char *server_ip = "10.42.0.1";
    uint16_t req_w      = 1280;
    uint16_t req_h      = 720;
    uint16_t req_fps    = 60;
    uint16_t req_br     = 10;
    uint8_t  req_rc     = KMS_RC_CBR;
    uint8_t  req_strict = 0;
    uint8_t  req_cqp    = 22;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-720") == 0 || strcmp(a, "--720") == 0 || strcmp(a, "-720p") == 0) {
            req_w = 1280;
            req_h = 720;
        } else if (strcmp(a, "-1080") == 0 || strcmp(a, "--1080") == 0 || strcmp(a, "-1080p") == 0) {
            req_w = 1920;
            req_h = 1080;
        } else if ((strcmp(a, "-w") == 0 || strcmp(a, "--width") == 0) && i + 1 < argc) {
            req_w = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(a, "-h") == 0 || strcmp(a, "--height") == 0) && i + 1 < argc &&
                   atoi(argv[i + 1]) > 0) {
            req_h = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(a, "-f") == 0 || strcmp(a, "-fps") == 0 || strcmp(a, "--fps") == 0) &&
                   i + 1 < argc) {
            req_fps = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(a, "-b") == 0 || strcmp(a, "-bitrate") == 0 || strcmp(a, "--bitrate") == 0) &&
                   i + 1 < argc) {
            int br_val = atoi(argv[++i]);
            /* Accept both Mbps (e.g. 10) and Kbps (e.g. 10000 from Moonlight CLI habit) */
            if (br_val >= 1000) br_val /= 1000;
            req_br = (uint16_t)(br_val < 0 ? 0 : br_val);
        } else if ((strcmp(a, "-rc") == 0 || strcmp(a, "--rc") == 0) && i + 1 < argc) {
            const char *rc_arg = argv[++i];
            if (strcasecmp(rc_arg, "cbr") == 0) {
                req_rc = KMS_RC_CBR;
            } else if (strcasecmp(rc_arg, "vbr") == 0) {
                req_rc = KMS_RC_VBR;
            } else if (strcasecmp(rc_arg, "cqp") == 0) {
                req_rc = KMS_RC_CQP;
            } else {
                fprintf(stderr, "[kmstream-pi] Unknown rate control mode '%s' (use cbr, vbr, or cqp)\n", rc_arg);
                return 1;
            }
        } else if ((strcmp(a, "-qp") == 0 || strcmp(a, "--qp") == 0 || strcmp(a, "-cqp") == 0) &&
                   i + 1 < argc) {
            int qp_val = atoi(argv[++i]);
            if (qp_val < 1) qp_val = 1;
            if (qp_val > 51) qp_val = 51;
            req_cqp = (uint8_t)qp_val;
        } else if (strcmp(a, "-strict-rc") == 0 || strcmp(a, "--strict-rc") == 0) {
            req_strict = 1;
        } else if (strcmp(a, "-help") == 0 || strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] != '-') {
            server_ip = a;
        } else {
            fprintf(stderr, "[kmstream-pi] Unknown argument: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct decode_v4l2_ctx dec;
    struct display_kms_ctx kms;
    display_kms_init(&kms, &dec);
    decode_v4l2_init(&dec);

    struct rx_shared *sh = net_rx_init_and_start(server_ip, req_w, req_h, req_fps, req_br,
                                                 req_rc, req_strict, req_cqp, &g_stop);

    struct client_app_ctx app;
    memset(&app, 0, sizeof(app));
    app.dec = &dec;
    app.kms = &kms;
    app.sh  = sh;

    pthread_t event_tid;
    if (pthread_create(&event_tid, NULL, v4l2_event_thread, &app) != 0) {
        perror("[kmstream-pi] Failed to create V4L2/DRM event thread");
        net_rx_cleanup(sh);
        display_kms_cleanup(&kms, &dec);
        decode_v4l2_cleanup(&dec);
        return 1;
    }
    configure_realtime_thread(event_tid, 45);

    const char *rc_name = (req_rc == KMS_RC_CQP) ? "CQP" : ((req_rc == KMS_RC_VBR) ? "VBR" : "CBR");
    printf("[kmstream-pi] Connecting to %s:%d (%s, proto=v%u, SO_RCVBUF=%d KB) -> requesting %ux%u @ %u FPS (RC=%s",
           server_ip, KMS_CTRL_PORT,
           KMSTREAM_STATS ? "STATS" : "RELEASE",
           KMS_PROTO_VERSION,
           sh->actual_rcvbuf / 1024,
           req_w, req_h, req_fps, rc_name);
    if (req_rc == KMS_RC_CQP) {
        printf(" QP=%u)\n", req_cqp);
    } else {
        printf(" %u Mbps%s)\n", req_br, req_strict ? ", Strict 1-Frame VBV" : ", Standard VBV");
    }
    fflush(stdout);

    while (!g_stop) {
        int ready_idx = -1;

        pthread_mutex_lock(&sh->mtx);
        while (!g_stop) {
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

            /* 2. Count ready frames and pick the oldest (FIFO). If > 3 pile up, trim oldest down to 2 without stalling */
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
                while (ready_cnt > 2) {
                    int drop_idx = -1;
                    uint32_t min_id = UINT32_MAX;
                    for (int i = 0; i < RX_RING_SLOTS; i++) {
                        if (sh->slots[i].state == SLOT_READY && sh->slots[i].frame_id < min_id) {
                            min_id = sh->slots[i].frame_id;
                            drop_idx = i;
                        }
                    }
                    if (drop_idx < 0) break;
                    sh->slots[drop_idx].state = SLOT_FREE;
                    sh->rx_drops++;
                    ready_cnt--;
                }
                net_rx_request_idr_locked(sh);
                ready_idx = -1;
                oldest_id = UINT32_MAX;
                for (int i = 0; i < RX_RING_SLOTS; i++) {
                    if (sh->slots[i].state == SLOT_READY && sh->slots[i].frame_id < oldest_id) {
                        oldest_id = sh->slots[i].frame_id;
                        ready_idx = i;
                    }
                }
            }
            if (ready_idx >= 0) {
                sh->slots[ready_idx].state = SLOT_SENDING;
                break;
            }
            if (net_rx_wait_for_ready(sh, &g_stop) < 0) {
                break;
            }
        }

        if (g_stop || ready_idx < 0) {
            pthread_mutex_unlock(&sh->mtx);
            break;
        }

        struct rx_slot *s = &sh->slots[ready_idx];
        uint32_t cur_frame_id = s->frame_id;
        uint32_t cur_size     = s->frame_size;
#if KMSTREAM_STATS
        uint64_t cap_ts_us   = s->capture_ts_us;
        uint64_t rx_first_us = s->t_rx_first_us;
        uint64_t rx_last_us  = s->t_rx_last_us;
        uint16_t t_conv_us   = s->t_convert_us;
        uint16_t t_enc_us    = s->t_encode_us;
#endif
        pthread_mutex_unlock(&sh->mtx);

#if KMSTREAM_STATS
        /* Publish metadata slot BEFORE decode_v4l2_submit() so SCHED_FIFO v4l2_event_thread
         * always sees valid metadata even if it preempts us right after VIDIOC_QBUF. */
        struct frame_meta_slot *ms = &app.meta_ring[cur_frame_id & 63];
        uint64_t t_dec_start = now_us();
        ms->capture_ts_us  = cap_ts_us;
        ms->t_rx_first_us  = rx_first_us;
        ms->t_rx_last_us   = rx_last_us;
        ms->t_convert_us   = t_conv_us;
        ms->t_encode_us    = t_enc_us;
        ms->t_dec_start_us = t_dec_start;
        atomic_store_explicit(&ms->t_send_done_us, t_dec_start, memory_order_relaxed);
        atomic_store_explicit(&ms->frame_id, cur_frame_id, memory_order_release);
        atomic_store_explicit(&app.last_submitted_id, cur_frame_id, memory_order_relaxed);
#endif

        int sret = decode_v4l2_submit(&dec, s->data, cur_size, cur_frame_id, &g_stop);

#if KMSTREAM_STATS
        atomic_store_explicit(&ms->t_send_done_us, now_us(), memory_order_relaxed);
#endif

        pthread_mutex_lock(&sh->mtx);
        s->state = SLOT_FREE;
        if (sret < 0 && sret != -ECANCELED) {
            net_rx_request_idr_locked(sh);
        }
        pthread_mutex_unlock(&sh->mtx);
    }

    g_stop = 1;
    pthread_mutex_lock(&dec.v4l2_lock);
    pthread_cond_broadcast(&dec.drc_cond);
    pthread_mutex_unlock(&dec.v4l2_lock);

    /* 1. Stop event thread first so no concurrent DRM or V4L2 calls happen during cleanup */
    pthread_join(event_tid, NULL);

    /* 2. Notify server (KMS_CTRL_STOP) and stop RX thread */
    net_rx_cleanup(sh);

    /* 3. Drain pending page flips, restore fbcon onto Primary Plane, and free DRM FBs/DMABUFs */
    display_kms_cleanup(&kms, &dec);

    /* 4. Stop V4L2 queues (OUTPUT -> CAPTURE) and close decoder device */
    decode_v4l2_cleanup(&dec);

    printf("[kmstream-pi] Stopped cleanly.\n");
    return 0;
}
