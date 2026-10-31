#ifndef KMSTREAM_STATS_H
#define KMSTREAM_STATS_H

#include <stdint.h>

#define KMS_STAT_SAMPLES 256

struct kms_stat_win {
    uint32_t samples[KMS_STAT_SAMPLES];
    uint32_t count;
    uint64_t sum;
};

static inline void kms_stat_reset(struct kms_stat_win *w) {
    w->count = 0;
    w->sum   = 0;
}

static inline void kms_stat_add(struct kms_stat_win *w, uint32_t val) {
    if (w->count < KMS_STAT_SAMPLES) {
        w->samples[w->count++] = val;
        w->sum += val;
    }
}

static inline void kms_stat_compute(struct kms_stat_win *w, uint32_t *avg_out, uint32_t *p99_out) {
    if (w->count == 0) {
        *avg_out = 0;
        *p99_out = 0;
        return;
    }
    /* Insertion sort on up to 256 samples (~120 at 60 fps over 2s) */
    for (uint32_t i = 1; i < w->count; i++) {
        uint32_t key = w->samples[i];
        int j = (int)i - 1;
        while (j >= 0 && w->samples[j] > key) {
            w->samples[j + 1] = w->samples[j];
            j--;
        }
        w->samples[j + 1] = key;
    }
    *avg_out = (uint32_t)(w->sum / w->count);
    uint32_t idx = (w->count * 99) / 100;
    if (idx >= w->count) idx = w->count - 1;
    *p99_out = w->samples[idx];
}

#endif /* KMSTREAM_STATS_H */
