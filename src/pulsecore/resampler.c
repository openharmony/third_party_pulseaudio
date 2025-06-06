/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <math.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/core-util.h>

#include "resampler.h"
#include "downmix.h"
#include "resampleLoader.h"

/* Number of samples of extra space we allow the resamplers to return */
#define EXTRA_FRAMES 128
#define RESAMPLER_CACHE_SIZE_RATIO 20

struct ffmpeg_data { /* data specific to ffmpeg */
    struct AVResampleContext *state;
};

static int copy_init(pa_resampler *r);
static int (*ProResamlerInit) (pa_resampler *r) = NULL;

static void setup_remap(const pa_resampler *r, pa_remap_t *m, bool *lfe_remixed);
static void free_remap(pa_remap_t *m);

static int (* const init_table[])(pa_resampler *r) = {
#ifdef HAVE_LIBSAMPLERATE
    [PA_RESAMPLER_SRC_SINC_BEST_QUALITY]   = pa_resampler_libsamplerate_init,
    [PA_RESAMPLER_SRC_SINC_MEDIUM_QUALITY] = pa_resampler_libsamplerate_init,
    [PA_RESAMPLER_SRC_SINC_FASTEST]        = pa_resampler_libsamplerate_init,
    [PA_RESAMPLER_SRC_ZERO_ORDER_HOLD]     = pa_resampler_libsamplerate_init,
    [PA_RESAMPLER_SRC_LINEAR]              = pa_resampler_libsamplerate_init,
#else
    [PA_RESAMPLER_SRC_SINC_BEST_QUALITY]   = NULL,
    [PA_RESAMPLER_SRC_SINC_MEDIUM_QUALITY] = NULL,
    [PA_RESAMPLER_SRC_SINC_FASTEST]        = NULL,
    [PA_RESAMPLER_SRC_ZERO_ORDER_HOLD]     = NULL,
    [PA_RESAMPLER_SRC_LINEAR]              = NULL,
#endif
    [PA_RESAMPLER_TRIVIAL]                 = pa_resampler_trivial_init,
#ifdef HAVE_SPEEX
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+0]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+1]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+2]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+3]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+4]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+5]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+6]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+7]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+8]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+9]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+10]     = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+0]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+1]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+2]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+3]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+4]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+5]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+6]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+7]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+8]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+9]      = pa_resampler_speex_init,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+10]     = pa_resampler_speex_init,
#else
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+0]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+1]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+2]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+3]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+4]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+5]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+6]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+7]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+8]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+9]      = NULL,
    [PA_RESAMPLER_SPEEX_FLOAT_BASE+10]     = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+0]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+1]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+2]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+3]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+4]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+5]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+6]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+7]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+8]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+9]      = NULL,
    [PA_RESAMPLER_SPEEX_FIXED_BASE+10]     = NULL,
#endif
    [PA_RESAMPLER_FFMPEG]                  = pa_resampler_ffmpeg_init,
    [PA_RESAMPLER_AUTO]                    = NULL,
    [PA_RESAMPLER_COPY]                    = copy_init,
    [PA_RESAMPLER_PEAKS]                   = pa_resampler_peaks_init,
#ifdef HAVE_SOXR
    [PA_RESAMPLER_SOXR_MQ]                 = pa_resampler_soxr_init,
    [PA_RESAMPLER_SOXR_HQ]                 = pa_resampler_soxr_init,
    [PA_RESAMPLER_SOXR_VHQ]                = pa_resampler_soxr_init,
#else
    [PA_RESAMPLER_SOXR_MQ]                 = NULL,
    [PA_RESAMPLER_SOXR_HQ]                 = NULL,
    [PA_RESAMPLER_SOXR_VHQ]                = NULL,
#endif
};

static void calculate_gcd(pa_resampler *r) {
    unsigned gcd, n;

    pa_assert(r);

    gcd = r->i_ss.rate;
    n = r->o_ss.rate;

    while (n != 0) {
        unsigned tmp = gcd;

        gcd = n;
        n = tmp % n;
    }

    r->gcd = gcd;
}

static pa_resample_method_t choose_auto_resampler(pa_resample_flags_t flags,
    const uint32_t rate_a, const uint32_t rate_b) {
    pa_resample_method_t method;
    if (pa_resample_method_supported(PA_PRORESAMPLER_BASE + 1) && (rate_a != rate_b) &&
        LoadProResampler(&ProResamlerInit)) {
        method = PA_PRORESAMPLER_BASE + 1;
    } else if (pa_resample_method_supported(PA_RESAMPLER_SPEEX_FLOAT_BASE + 1) && (rate_a != rate_b)) {
        method = PA_RESAMPLER_SPEEX_FLOAT_BASE + 1;
    } else {
        method = PA_RESAMPLER_TRIVIAL;
    }

    return method;
}

static pa_resample_method_t fix_method(
                pa_resample_flags_t flags,
                pa_resample_method_t method,
                const uint32_t rate_a,
                const uint32_t rate_b) {

    pa_assert(pa_sample_rate_valid(rate_a));
    pa_assert(pa_sample_rate_valid(rate_b));
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    if (!(flags & PA_RESAMPLER_VARIABLE_RATE) && rate_a == rate_b) {
        pa_log_info("Forcing resampler 'copy', because of fixed, identical sample rates.");
        method = PA_RESAMPLER_COPY;
    }

    if (!pa_resample_method_supported(method)) {
        pa_log_warn("Support for resampler '%s' not compiled in, reverting to 'auto'.", pa_resample_method_to_string(method));
        method = PA_RESAMPLER_AUTO;
    }

    switch (method) {
        case PA_RESAMPLER_COPY:
            if (rate_a != rate_b) {
                pa_log_info("Resampler 'copy' cannot change sampling rate, reverting to resampler 'auto'.");
                method = PA_RESAMPLER_AUTO;
                break;
            }
            /* Else fall through */
        case PA_RESAMPLER_FFMPEG:
            if (flags & PA_RESAMPLER_VARIABLE_RATE) {
                pa_log_info("Resampler '%s' cannot do variable rate, reverting to resampler 'auto'.", pa_resample_method_to_string(method));
                method = PA_RESAMPLER_AUTO;
            }
            break;

        /* The Peaks resampler only supports downsampling.
         * Revert to auto if we are upsampling */
        case PA_RESAMPLER_PEAKS:
            if (rate_a < rate_b) {
                pa_log_warn("The 'peaks' resampler only supports downsampling, reverting to resampler 'auto'.");
                method = PA_RESAMPLER_AUTO;
            }
            break;

        default:
            break;
    }

    if (method != PA_RESAMPLER_AUTO) {
        AUDIO_INFO_LOG("resampler method is not auto, reset to auto!");
        method = PA_RESAMPLER_AUTO;
    }

    if (method == PA_RESAMPLER_AUTO)
        method = choose_auto_resampler(flags, rate_a, rate_b);

#ifdef HAVE_SPEEX
    /* At this point, method is supported in the sense that it
     * has an init function and supports the required flags. However,
     * speex-float implementation in PulseAudio relies on the
     * assumption that is invalid if speex has been compiled with
     * --enable-fixed-point. Besides, speex-fixed is more efficient
     * in this configuration. So use it instead.
     */
    if (method >= PA_RESAMPLER_SPEEX_FLOAT_BASE && method <= PA_RESAMPLER_SPEEX_FLOAT_MAX) {
        if (pa_speex_is_fixed_point()) {
            pa_log_info("Speex appears to be compiled with --enable-fixed-point. "
                        "Switching to a fixed-point resampler because it should be faster.");
            method = method - PA_RESAMPLER_SPEEX_FLOAT_BASE + PA_RESAMPLER_SPEEX_FIXED_BASE;
        }
    }
#endif

    return method;
}

/* Return true if a is a more precise sample format than b, else return false */
static bool sample_format_more_precise(pa_sample_format_t a, pa_sample_format_t b) {
    pa_assert(pa_sample_format_valid(a));
    pa_assert(pa_sample_format_valid(b));

    switch (a) {
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_ULAW:
            return false;
            break;

        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
            if (b == PA_SAMPLE_ULAW || b == PA_SAMPLE_ALAW || b == PA_SAMPLE_U8)
                return true;
            else
                return false;
            break;

        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            if (b == PA_SAMPLE_ULAW || b == PA_SAMPLE_ALAW || b == PA_SAMPLE_U8 ||
                b == PA_SAMPLE_S16LE || b == PA_SAMPLE_S16BE)
                return true;
            else
                return false;
            break;

        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
            if (b == PA_SAMPLE_FLOAT32LE || b == PA_SAMPLE_FLOAT32BE ||
                b == PA_SAMPLE_S32LE || b == PA_SAMPLE_S32BE)
                return false;
            else
                return true;
            break;

        default:
            return false;
    }
}

static pa_sample_format_t choose_work_format(
                    pa_resample_method_t method,
                    pa_sample_format_t a,
                    pa_sample_format_t b,
                    bool map_required) {
    pa_sample_format_t work_format;

    pa_assert(pa_sample_format_valid(a));
    pa_assert(pa_sample_format_valid(b));
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    if (method >= PA_RESAMPLER_SPEEX_FIXED_BASE && method <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        method = PA_RESAMPLER_SPEEX_FIXED_BASE;

    switch (method) {
        /* This block is for resampling functions that only
         * support the S16 sample format. */
        case PA_RESAMPLER_SPEEX_FIXED_BASE:
        case PA_RESAMPLER_FFMPEG:
            work_format = PA_SAMPLE_S16NE;
            break;

        /* This block is for resampling functions that support
         * any sample format. */
        case PA_RESAMPLER_COPY:
        case PA_RESAMPLER_TRIVIAL:
            if (!map_required && a == b) {
                work_format = a;
                break;
            }
            /* If both input and output are using S32NE and we don't
             * need any resampling we can use S32NE directly, avoiding
             * converting back and forth between S32NE and
             * FLOAT32NE. */
            if ((a == PA_SAMPLE_S32NE) && (b == PA_SAMPLE_S32NE)) {
                work_format = PA_SAMPLE_S32NE;
                break;
            }
            /* Else fall through */
        case PA_RESAMPLER_PEAKS:
            /* PEAKS, COPY and TRIVIAL do not benefit from increased
             * working precision, so for better performance use s16ne
             * if either input or output fits in it. */
            if (a == PA_SAMPLE_S16NE || b == PA_SAMPLE_S16NE) {
                work_format = PA_SAMPLE_S16NE;
                break;
            }
            /* Else fall through */
        case PA_RESAMPLER_SOXR_MQ:
        case PA_RESAMPLER_SOXR_HQ:
        case PA_RESAMPLER_SOXR_VHQ:
            /* Do processing with max precision of input and output. */
            if (sample_format_more_precise(a, PA_SAMPLE_S16NE) ||
                sample_format_more_precise(b, PA_SAMPLE_S16NE))
                work_format = PA_SAMPLE_FLOAT32NE;
            else
                work_format = PA_SAMPLE_S16NE;
            break;

        default:
            work_format = PA_SAMPLE_FLOAT32NE;
    }

    return work_format;
}

pa_resampler* pa_resampler_new(
        pa_mempool *pool,
        const pa_sample_spec *a,
        const pa_channel_map *am,
        const pa_sample_spec *b,
        const pa_channel_map *bm,
        unsigned crossover_freq,
        pa_resample_method_t method,
        pa_resample_flags_t flags) {

    pa_resampler *r = NULL;
    bool lfe_remixed = false;

    pa_assert(pool);
    pa_assert(a);
    pa_assert(b);
    pa_assert(pa_sample_spec_valid(a));
    pa_assert(pa_sample_spec_valid(b));
    pa_assert(method >= 0);
    pa_assert(method < PA_RESAMPLER_MAX);

    method = fix_method(flags, method, a->rate, b->rate);

    r = pa_xnew0(pa_resampler, 1);
    r->mempool = pool;
    r->method = method;
    r->flags = flags;
    r->in_frames = 0;
    r->out_frames = 0;

    /* Fill sample specs */
    r->i_ss = *a;
    r->o_ss = *b;
    calculate_gcd(r);

    if (am)
        r->i_cm = *am;
    else if (!pa_channel_map_init_auto(&r->i_cm, r->i_ss.channels, PA_CHANNEL_MAP_DEFAULT))
        goto fail;

    if (bm)
        r->o_cm = *bm;
    else if (!pa_channel_map_init_auto(&r->o_cm, r->o_ss.channels, PA_CHANNEL_MAP_DEFAULT))
        goto fail;

    r->i_fz = pa_frame_size(a);
    r->o_fz = pa_frame_size(b);

    r->map_required = (r->i_ss.channels != r->o_ss.channels || (!(r->flags & PA_RESAMPLER_NO_REMAP) &&
        !pa_channel_map_equal(&r->i_cm, &r->o_cm)));

    r->work_format = choose_work_format(method, a->format, b->format, r->map_required);
    r->w_sz = pa_sample_size_of_format(r->work_format);

    if (r->i_ss.format != r->work_format) {
        if (r->work_format == PA_SAMPLE_FLOAT32NE) {
            if (!(r->to_work_format_func = pa_get_convert_to_float32ne_function(r->i_ss.format)))
                goto fail;
        } else {
            pa_assert(r->work_format == PA_SAMPLE_S16NE);
            if (!(r->to_work_format_func = pa_get_convert_to_s16ne_function(r->i_ss.format)))
                goto fail;
        }
    }

    if (r->o_ss.format != r->work_format) {
        if (r->work_format == PA_SAMPLE_FLOAT32NE) {
            if (!(r->from_work_format_func = pa_get_convert_from_float32ne_function(r->o_ss.format)))
                goto fail;
        } else {
            pa_assert(r->work_format == PA_SAMPLE_S16NE);
            if (!(r->from_work_format_func = pa_get_convert_from_s16ne_function(r->o_ss.format)))
                goto fail;
        }
    }

    if (r->o_ss.channels <= r->i_ss.channels) {
        /* pipeline is: format conv. -> remap -> resample -> format conv. */
        r->work_channels = r->o_ss.channels;

        /* leftover buffer is remap output buffer (before resampling) */
        r->leftover_buf = &r->remap_buf;
        r->leftover_buf_size = &r->remap_buf_size;
        r->have_leftover = &r->leftover_in_remap;
    } else {
        /* pipeline is: format conv. -> resample -> remap -> format conv. */
        r->work_channels = r->i_ss.channels;

        /* leftover buffer is to_work output buffer (before resampling) */
        r->leftover_buf = &r->to_work_format_buf;
        r->leftover_buf_size = &r->to_work_format_buf_size;
        r->have_leftover = &r->leftover_in_to_work;
    }
    r->w_fz = pa_sample_size_of_format(r->work_format) * r->work_channels;

    AUDIO_INFO_LOG("pa_resampler_new: rate %{public}u -> %{public}u (method %{public}s), "
        "format %{public}s -> %{public}s (intermediate %{public}s), "
        "channels %{public}u -> %{public}u (resampling %{public}u)",
        a->rate, b->rate, pa_resample_method_to_string(r->method), pa_sample_format_to_string(a->format),
        pa_sample_format_to_string(b->format), pa_sample_format_to_string(r->work_format), a->channels, b->channels,
        r->work_channels);

    /* set up the remap structure */
    if (r->map_required)
        setup_remap(r, &r->remap, &lfe_remixed);

    if (lfe_remixed && crossover_freq > 0) {
        pa_sample_spec wss = r->o_ss;
        wss.format = r->work_format;
        /* FIXME: For now just hardcode maxrewind to 3 seconds */
        r->lfe_filter = pa_lfe_filter_new(&wss, &r->o_cm, (float)crossover_freq, b->rate * 3);
        pa_log_debug("  lfe filter activated (LR4 type), the crossover_freq = %uHz", crossover_freq);
    }

    /* initialize implementation */
    if (method >= PA_PRORESAMPLER_BASE && method <= PA_PRORESAMPLER_MAX) {
        if (ProResamlerInit(r) < 0) {
            goto fail;
        }
    } else {
        if (init_table[method](r) < 0) {
            goto fail;
        }
    }
    return r;

fail:
    if (r->lfe_filter)
      pa_lfe_filter_free(r->lfe_filter);
    pa_xfree(r);

    return NULL;
}

void pa_resampler_free(pa_resampler *r) {
    pa_assert(r);

    if (r->impl.free)
        r->impl.free(r);
    else
        pa_xfree(r->impl.data);

    if (r->lfe_filter)
        pa_lfe_filter_free(r->lfe_filter);

    if (r->to_work_format_buf.memblock)
        pa_memblock_unref(r->to_work_format_buf.memblock);
    if (r->remap_buf.memblock)
        pa_memblock_unref(r->remap_buf.memblock);
    if (r->resample_buf.memblock)
        pa_memblock_unref(r->resample_buf.memblock);
    if (r->from_work_format_buf.memblock)
        pa_memblock_unref(r->from_work_format_buf.memblock);

    free_remap(&r->remap);

    pa_xfree(r);
}

void pa_resampler_set_input_rate(pa_resampler *r, uint32_t rate) {
    pa_assert(r);
    pa_assert(rate > 0);
    pa_assert(r->impl.update_rates);

    if (r->i_ss.rate == rate)
        return;

    /* Recalculate delay counters */
    r->in_frames = pa_resampler_get_delay(r, false);
    r->out_frames = 0;

    r->i_ss.rate = rate;
    calculate_gcd(r);

    r->impl.update_rates(r);
}

void pa_resampler_set_output_rate(pa_resampler *r, uint32_t rate) {
    pa_assert(r);
    pa_assert(rate > 0);
    pa_assert(r->impl.update_rates);

    if (r->o_ss.rate == rate)
        return;

    /* Recalculate delay counters */
    r->in_frames = pa_resampler_get_delay(r, false);
    r->out_frames = 0;

    r->o_ss.rate = rate;
    calculate_gcd(r);

    r->impl.update_rates(r);

    if (r->lfe_filter)
        pa_lfe_filter_update_rate(r->lfe_filter, rate);
}

/* pa_resampler_request() and pa_resampler_result() should be as exact as
 * possible to ensure that no samples are lost or duplicated during rewinds.
 * Ignore the leftover buffer, the value appears to be wrong for ffmpeg
 * and 0 in all other cases. If the resampler is NULL it means that no
 * resampling is necessary and the input length equals the output length.
 * FIXME: These functions are not exact for the soxr resamplers because
 * soxr uses a different algorithm. */
size_t pa_resampler_request(pa_resampler *r, size_t out_length) {
    size_t in_length;

    if (!r || out_length == 0)
        return out_length;

    /* Convert to output frames */
    out_length = out_length / r->o_fz;

    /* Convert to input frames. The equation matches exactly the
     * behavior of the used resamplers and will calculate the
     * minimum number of input frames that are needed to produce
     * the given number of output frames. */
     if (r->o_ss.rate % 50 != 0 || r->i_ss.rate % 50 != 0) {  // 1s/20ms(oh frmLen) = 50
        in_length = (out_length - 1) * r->i_ss.rate / r->o_ss.rate + 1;
     } else {
        in_length = out_length * r->i_ss.rate / r->o_ss.rate;
     }

    /* Convert to input length */
    return in_length * r->i_fz;
}

size_t pa_resampler_result(pa_resampler *r, size_t in_length) {
    size_t out_length;

    if (!r)
        return in_length;

    /* Convert to intput frames */
    in_length = in_length / r->i_fz;

     /* soxr processes samples in blocks, depending on the ratio.
      * Therefore samples  that do not fit into a block must be
      * ignored. */
    if (r->method == PA_RESAMPLER_SOXR_MQ || r->method == PA_RESAMPLER_SOXR_HQ || r->method == PA_RESAMPLER_SOXR_VHQ) {
        double ratio;
        size_t block_size;
        int k;

        ratio = (double)r->i_ss.rate / (double)r->o_ss.rate;

        for (k = 0; k < 7; k++) {
            if (ratio < pow(2, k + 1))
                break;
        }
        block_size = pow(2, k);
        in_length = in_length - in_length % block_size;
    }

    /* Convert to output frames. This matches exactly the algorithm
     * used by the resamplers except for the soxr resamplers. */

     out_length = in_length * r->o_ss.rate / r->i_ss.rate;
     if ((double)in_length * (double)r->o_ss.rate / (double)r->i_ss.rate - out_length > 0)
         out_length++;
     /* The libsamplerate resamplers return one sample more if the result is integral and the ratio is not integral. */
     else if (r->method >= PA_RESAMPLER_SRC_SINC_BEST_QUALITY && r->method <= PA_RESAMPLER_SRC_SINC_FASTEST && r->i_ss.rate > r->o_ss.rate && r->i_ss.rate % r->o_ss.rate > 0 && (double)in_length * (double)r->o_ss.rate / (double)r->i_ss.rate - out_length <= 0)
         out_length++;
     else if (r->method == PA_RESAMPLER_SRC_ZERO_ORDER_HOLD && r->i_ss.rate > r->o_ss.rate && (double)in_length * (double)r->o_ss.rate / (double)r->i_ss.rate - out_length <= 0)
         out_length++;

    /* Convert to output length */
    return out_length * r->o_fz;
}

size_t pa_resampler_max_block_size(pa_resampler *r) {
    size_t block_size_max;
    pa_sample_spec max_ss;
    size_t max_fs;
    size_t frames;

    pa_assert(r);

    block_size_max = pa_mempool_block_size_max(r->mempool);

    /* We deduce the "largest" sample spec we're using during the
     * conversion */
    max_ss.channels = (uint8_t) (PA_MAX(r->i_ss.channels, r->o_ss.channels));

    max_ss.format = r->i_ss.format;

    if (pa_sample_size_of_format(max_ss.format) < pa_sample_size_of_format(r->o_ss.format))
        max_ss.format = r->o_ss.format;

    if (pa_sample_size_of_format(max_ss.format) < pa_sample_size_of_format(r->work_format))
        max_ss.format = r->work_format;

    max_ss.rate = PA_MAX(r->i_ss.rate, r->o_ss.rate);

    max_fs = pa_frame_size(&max_ss);
    frames = block_size_max / max_fs - EXTRA_FRAMES;

    pa_assert(frames >= (r->leftover_buf->length / r->w_fz));
    if (*r->have_leftover)
        frames -= r->leftover_buf->length / r->w_fz;

    block_size_max = ((uint64_t) frames * r->i_ss.rate / max_ss.rate) * r->i_fz;

    if (block_size_max > 0)
        return block_size_max;
    else
        /* A single input frame may result in so much output that it doesn't
         * fit in one standard memblock (e.g. converting 1 Hz to 44100 Hz). In
         * this case the max block size will be set to one frame, and some
         * memory will be probably be allocated with malloc() instead of using
         * the memory pool.
         *
         * XXX: Should we support this case at all? We could also refuse to
         * create resamplers whose max block size would exceed the memory pool
         * block size. In this case also updating the resampler rate should
         * fail if the new rate would cause an excessive max block size (in
         * which case the stream would probably have to be killed). */
        return r->i_fz;
}

void pa_resampler_reset(pa_resampler *r) {
    pa_assert(r);

    if (r->impl.reset)
        r->impl.reset(r);

    if (r->lfe_filter)
        pa_lfe_filter_reset(r->lfe_filter);

    *r->have_leftover = false;

    r->in_frames = 0;
    r->out_frames = 0;
}

/* This function runs amount bytes of data from the history queue through the
 * resampler and discards the result. The history queue is unchanged after the
 * call. This is used to preload a resampler after a reset. Returns the number
 * of frames produced by the resampler. */
size_t pa_resampler_prepare(pa_resampler *r, pa_memblockq *history_queue, size_t amount) {
    size_t history_bytes, max_block_size, out_size;
    int64_t to_run;

    pa_assert(r);

    if (!history_queue || amount == 0)
        return 0;

    /* Rewind the LFE filter by the amount of history data. */
    history_bytes = pa_resampler_result(r, amount);
    if (r->lfe_filter)
        pa_lfe_filter_rewind(r->lfe_filter, history_bytes);

    pa_memblockq_rewind(history_queue, amount);
    max_block_size = pa_resampler_max_block_size(r);
    to_run = amount;
    out_size = 0;

    while (to_run > 0) {
        pa_memchunk in_chunk, out_chunk;
        size_t current;

        current = PA_MIN(to_run, (int64_t) max_block_size);

        /* Get data from memblockq */
        if (pa_memblockq_peek_fixed_size(history_queue, current, &in_chunk) < 0) {
            pa_log_warn("Could not read history data for resampler.");

            /* Restore queue to original state and reset resampler */
            pa_memblockq_drop(history_queue, to_run);
            pa_resampler_reset(r);
            return out_size;
        }

        /* Run the resampler */
        pa_resampler_run(r, &in_chunk, &out_chunk);

        /* Discard result */
        if (out_chunk.length != 0) {
            out_size += out_chunk.length;
            pa_memblock_unref(out_chunk.memblock);
        }

        pa_memblock_unref(in_chunk.memblock);
        pa_memblockq_drop(history_queue, current);
        to_run -= current;
    }

    return out_size;
}

size_t pa_resampler_rewind(pa_resampler *r, size_t out_bytes, pa_memblockq *history_queue, size_t amount) {
    pa_assert(r);

    /* For now, we don't have any rewindable resamplers, so we just reset
     * the resampler if we cannot rewind using pa_resampler_prepare(). */
    if (r->impl.reset && !history_queue)
        r->impl.reset(r);

    if (r->lfe_filter)
        pa_lfe_filter_rewind(r->lfe_filter, out_bytes);

    if (!history_queue) {
        *r->have_leftover = false;

        r->in_frames = 0;
        r->out_frames = 0;
    }

    if (history_queue && amount > 0)
        return pa_resampler_prepare(r, history_queue, amount);

    return 0;
}

pa_resample_method_t pa_resampler_get_method(pa_resampler *r) {
    pa_assert(r);

    return r->method;
}

const pa_channel_map* pa_resampler_input_channel_map(pa_resampler *r) {
    pa_assert(r);

    return &r->i_cm;
}

const pa_sample_spec* pa_resampler_input_sample_spec(pa_resampler *r) {
    pa_assert(r);

    return &r->i_ss;
}

const pa_channel_map* pa_resampler_output_channel_map(pa_resampler *r) {
    pa_assert(r);

    return &r->o_cm;
}

const pa_sample_spec* pa_resampler_output_sample_spec(pa_resampler *r) {
    pa_assert(r);

    return &r->o_ss;
}

static const char * const resample_methods[] = {
    "src-sinc-best-quality",
    "src-sinc-medium-quality",
    "src-sinc-fastest",
    "src-zero-order-hold",
    "src-linear",
    "trivial",
    "speex-float-0",
    "speex-float-1",
    "speex-float-2",
    "speex-float-3",
    "speex-float-4",
    "speex-float-5",
    "speex-float-6",
    "speex-float-7",
    "speex-float-8",
    "speex-float-9",
    "speex-float-10",
    "speex-fixed-0",
    "speex-fixed-1",
    "speex-fixed-2",
    "speex-fixed-3",
    "speex-fixed-4",
    "speex-fixed-5",
    "speex-fixed-6",
    "speex-fixed-7",
    "speex-fixed-8",
    "speex-fixed-9",
    "speex-fixed-10",
    "ffmpeg",
    "auto",
    "copy",
    "peaks",
    "soxr-mq",
    "soxr-hq",
    "soxr-vhq",
    "proresampler-0",
    "proresampler-1",
    "proresampler-2",
    "proresampler-3",
    "proresampler-4",
    "proresampler-5",
    "proresampler-6",
    "proresampler-7",
    "proresampler-8",
    "proresampler-9",
    "proresampler-10",
};

const char *pa_resample_method_to_string(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return NULL;

    return resample_methods[m];
}

int pa_resample_method_supported(pa_resample_method_t m) {

    if (m < 0 || m >= PA_RESAMPLER_MAX)
        return 0;

#ifndef HAVE_LIBSAMPLERATE
    if (m <= PA_RESAMPLER_SRC_LINEAR)
        return 0;
#endif

#ifndef HAVE_SPEEX
    if (m >= PA_RESAMPLER_SPEEX_FLOAT_BASE && m <= PA_RESAMPLER_SPEEX_FLOAT_MAX)
        return 0;
    if (m >= PA_RESAMPLER_SPEEX_FIXED_BASE && m <= PA_RESAMPLER_SPEEX_FIXED_MAX)
        return 0;
#endif

#ifndef HAVE_SOXR
    if (m >= PA_RESAMPLER_SOXR_MQ && m <= PA_RESAMPLER_SOXR_VHQ)
        return 0;
#endif

    return 1;
}

pa_resample_method_t pa_parse_resample_method(const char *string) {
    pa_resample_method_t m;

    pa_assert(string);

    for (m = 0; m < PA_RESAMPLER_MAX; m++)
        if (pa_streq(string, resample_methods[m]))
            return m;

    if (pa_streq(string, "speex-fixed"))
        return PA_RESAMPLER_SPEEX_FIXED_BASE + 1;

    if (pa_streq(string, "speex-float"))
        return PA_RESAMPLER_SPEEX_FLOAT_BASE + 1;

    return PA_RESAMPLER_INVALID;
}

static bool on_left(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_SIDE_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_LEFT;
}

static bool on_right(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_SIDE_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_RIGHT;
}

static bool on_center(pa_channel_position_t p) {

    return
        p == PA_CHANNEL_POSITION_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_REAR_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_REAR_CENTER;
}

static bool on_lfe(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_LFE;
}

static bool on_front(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_FRONT_CENTER ||
        p == PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER ||
        p == PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
}

static bool on_rear(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_REAR_CENTER ||
        p == PA_CHANNEL_POSITION_TOP_REAR_LEFT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_REAR_CENTER;
}

static bool on_side(pa_channel_position_t p) {
    return
        p == PA_CHANNEL_POSITION_SIDE_LEFT ||
        p == PA_CHANNEL_POSITION_SIDE_RIGHT ||
        p == PA_CHANNEL_POSITION_TOP_CENTER;
}

typedef enum pa_channel_direction {
    ON_FRONT,
    ON_REAR,
    ON_SIDE,
    ON_OTHER
} pa_channel_direction_t;

static int front_rear_side(pa_channel_position_t p) {
    if (on_front(p))
        return ON_FRONT;
    if (on_rear(p))
        return ON_REAR;
    if (on_side(p))
        return ON_SIDE;
    return ON_OTHER;
}

/* For downmixing other output format, usage [direction_of_input_channel][direction_of_output_channel] */
/*           layout            */
/*        front rear side      */
/* front |     |    |    |     */
/* rear  |     |    |    |     */
/* side  |     |    |    |     */
static const float directionDownMixMatrix[ON_OTHER][ON_OTHER] = {
    {1.0f, 0.5f, 0.7071f},
    {0.5f, 1.0f, 0.7071f},
    {0.7071f, 0.7071f, 1.0f},
};

/* Fill a map of which output channels should get mono from input, not including
 * LFE output channels. (The LFE output channels are mapped separately.)
 */
static void setup_oc_mono_map(const pa_resampler *r, float *oc_mono_map) {
    unsigned oc;
    unsigned n_oc;
    bool found_oc_for_mono = false;

    pa_assert(r);
    pa_assert(oc_mono_map);

    n_oc = r->o_ss.channels;

    if (!(r->flags & PA_RESAMPLER_NO_FILL_SINK)) {
        /* Mono goes to all non-LFE output channels and we're done. */
        for (oc = 0; oc < n_oc; oc++)
            oc_mono_map[oc] = on_lfe(r->o_cm.map[oc]) ? 0.0f : 1.0f;
        return;
    } else {
        /* Initialize to all zero so we can select individual channels below. */
        for (oc = 0; oc < n_oc; oc++)
            oc_mono_map[oc] = 0.0f;
    }

    for (oc = 0; oc < n_oc; oc++) {
        if (r->o_cm.map[oc] == PA_CHANNEL_POSITION_MONO) {
            oc_mono_map[oc] = 1.0f;
            found_oc_for_mono = true;
        }
    }
    if (found_oc_for_mono)
        return;

    for (oc = 0; oc < n_oc; oc++) {
        if (r->o_cm.map[oc] == PA_CHANNEL_POSITION_FRONT_CENTER) {
            oc_mono_map[oc] = 1.0f;
            found_oc_for_mono = true;
        }
    }
    if (found_oc_for_mono)
        return;

    for (oc = 0; oc < n_oc; oc++) {
        if (r->o_cm.map[oc] == PA_CHANNEL_POSITION_FRONT_LEFT || r->o_cm.map[oc] == PA_CHANNEL_POSITION_FRONT_RIGHT) {
            oc_mono_map[oc] = 1.0f;
            found_oc_for_mono = true;
        }
    }
    if (found_oc_for_mono)
        return;

    /* Give up on finding a suitable map for mono, and just send it to all
     * non-LFE output channels.
     */
    for (oc = 0; oc < n_oc; oc++)
        oc_mono_map[oc] = on_lfe(r->o_cm.map[oc]) ? 0.0f : 1.0f;
}

static void setup_remap(const pa_resampler *r, pa_remap_t *m, bool *lfe_remixed) {
    unsigned oc, ic;
    unsigned n_oc, n_ic;
    bool ic_connected[PA_CHANNELS_MAX];
    pa_strbuf *s;
    char *t;

    pa_assert(r);
    pa_assert(m);
    pa_assert(lfe_remixed);

    n_oc = r->o_ss.channels;
    n_ic = r->i_ss.channels;

    m->format = r->work_format;
    m->i_ss = r->i_ss;
    m->o_ss = r->o_ss;

    memset(m->map_table_f, 0, sizeof(m->map_table_f));
    memset(m->map_table_i, 0, sizeof(m->map_table_i));

    memset(ic_connected, 0, sizeof(ic_connected));
    *lfe_remixed = false;

    if (r->flags & PA_RESAMPLER_NO_REMAP) {
        for (oc = 0; oc < PA_MIN(n_ic, n_oc); oc++)
            m->map_table_f[oc][oc] = 1.0f;

    } else if (r->flags & PA_RESAMPLER_NO_REMIX) {
        for (oc = 0; oc < n_oc; oc++) {
            pa_channel_position_t b = r->o_cm.map[oc];

            for (ic = 0; ic < n_ic; ic++) {
                pa_channel_position_t a = r->i_cm.map[ic];

                /* We shall not do any remixing. Hence, just check by name */
                if (a == b)
                    m->map_table_f[oc][ic] = 1.0f;
            }
        }
    } else {

        /* OK, we shall do the full monty: upmixing and downmixing. Our
         * algorithm is relatively simple, does not do spacialization, or delay
         * elements. LFE filters are done after the remap step. Patches are always
         * welcome, though. Oh, and it doesn't do any matrix decoding. (Which
         * probably wouldn't make any sense anyway.)
         *
         * This code is not idempotent: downmixing an upmixed stereo stream is
         * not identical to the original. The volume will not match, and the
         * two channels will be a linear combination of both.
         *
         * This is loosely based on random suggestions found on the Internet,
         * such as this:
         * http://www.halfgaar.net/surround-sound-in-linux and the alsa upmix
         * plugin.
         *
         * The algorithm works basically like this:
         *
         * 1) Connect all channels with matching names.
         *    This also includes fixing confusion between "5.1" and
         *    "5.1 (Side)" layouts, done by mpv.
         *
         * 2) Mono Handling:
         *    S:Mono: See setup_oc_mono_map().
         *    D:Mono: Avg all S:channels
         *
         * 3) Mix D:Left, D:Right (if PA_RESAMPLER_NO_FILL_SINK is clear):
         *    D:Left: If not connected, avg all S:Left
         *    D:Right: If not connected, avg all S:Right
         *
         * 4) Mix D:Center (if PA_RESAMPLER_NO_FILL_SINK is clear):
         *    If not connected, avg all S:Center
         *    If still not connected, avg all S:Left, S:Right
         *
         * 5) Mix D:LFE
         *    If not connected, avg all S:*
         *
         * 6) Make sure S:Left/S:Right is used: S:Left/S:Right: If not
         *    connected, mix into all D:left and all D:right channels. Gain is
         *    1/9.
         *
         * 7) Make sure S:Center, S:LFE is used:
         *
         *    S:Center, S:LFE: If not connected, mix into all D:left, all
         *    D:right, all D:center channels. Gain is 0.5 for center and 0.375
         *    for LFE. C-front is only mixed into L-front/R-front if available,
         *    otherwise into all L/R channels. Similarly for C-rear.
         *
         * 8) Normalize each row in the matrix such that the sum for each row is
         *    not larger than 1.0 in order to avoid clipping.
         *
         * S: and D: shall relate to the source resp. destination channels.
         *
         * Rationale: 1, 2 are probably obvious. For 3: this copies front to
         * rear if needed. For 4: we try to find some suitable C source for C,
         * if we don't find any, we avg L and R. For 5: LFE is mixed from all
         * channels. For 6: the rear channels should not be dropped entirely,
         * however have only minimal impact. For 7: movies usually encode
         * speech on the center channel. Thus we have to make sure this channel
         * is distributed to L and R if not available in the output. Also, LFE
         * is used to achieve a greater dynamic range, and thus we should try
         * to do our best to pass it to L+R.
         */

        unsigned
            ic_left = 0,
            ic_right = 0,
            ic_center = 0,
            ic_unconnected_center = 0,
            ic_unconnected_lfe = 0;
        bool ic_unconnected_center_mixed_in = 0;
        float oc_mono_map[PA_CHANNELS_MAX];

        for (ic = 0; ic < n_ic; ic++) {
            if (on_left(r->i_cm.map[ic]))
                ic_left++;
            if (on_right(r->i_cm.map[ic]))
                ic_right++;
            if (on_center(r->i_cm.map[ic]))
                ic_center++;
        }

        setup_oc_mono_map(r, oc_mono_map);

        // mono ouput or input channel = output channel
        for (oc = 0; oc < n_oc; oc++) {
            bool oc_connected = false;
            pa_channel_position_t b = r->o_cm.map[oc];

            for (ic = 0; ic < n_ic; ic++) {
                pa_channel_position_t a = r->i_cm.map[ic];

                if (a == b) {
                    m->map_table_f[oc][ic] = 1.0f;

                    oc_connected = true;
                    ic_connected[ic] = true;
                }
                else if (a == PA_CHANNEL_POSITION_MONO && oc_mono_map[oc] > 0.0f) {
                    m->map_table_f[oc][ic] = oc_mono_map[oc];

                    oc_connected = true;
                    ic_connected[ic] = true;
                }
                else if (b == PA_CHANNEL_POSITION_MONO) {
                    m->map_table_f[oc][ic] = 1.0f / (float) n_ic;

                    oc_connected = true;
                    ic_connected[ic] = true;
                }
            }
            // output channel has no relating input channel, upmix here
            if (!oc_connected) {
                /* Try to find matching input ports for this output port */

                if (on_left(b) && !(r->flags & PA_RESAMPLER_NO_FILL_SINK)) {

                    /* We are not connected and on the left side, let's
                     * average all left side input channels. */

                    if (ic_left > 0)
                        for (ic = 0; ic < n_ic; ic++)
                            if (on_left(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) ic_left;
                                ic_connected[ic] = true;
                            }

                    /* We ignore the case where there is no left input channel.
                     * Something is really wrong in this case anyway. */

                } else if (on_right(b) && !(r->flags & PA_RESAMPLER_NO_FILL_SINK)) {

                    /* We are not connected and on the right side, let's
                     * average all right side input channels. */

                    if (ic_right > 0)
                        for (ic = 0; ic < n_ic; ic++)
                            if (on_right(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) ic_right;
                                ic_connected[ic] = true;
                            }

                    /* We ignore the case where there is no right input
                     * channel. Something is really wrong in this case anyway.
                     * */

                } else if (on_center(b) && !(r->flags & PA_RESAMPLER_NO_FILL_SINK)) {

                    if (ic_center > 0) {

                        /* We are not connected and at the center. Let's average
                         * all center input channels. */

                        for (ic = 0; ic < n_ic; ic++)
                            if (on_center(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) ic_center;
                                ic_connected[ic] = true;
                            }

                    } else if (ic_left + ic_right > 0) {

                        /* Hmm, no center channel around, let's synthesize it
                         * by mixing L and R.*/

                        for (ic = 0; ic < n_ic; ic++)
                            if (on_left(r->i_cm.map[ic]) || on_right(r->i_cm.map[ic])) {
                                m->map_table_f[oc][ic] = 1.0f / (float) (ic_left + ic_right);
                                ic_connected[ic] = true;
                            }
                    }

                    /* We ignore the case where there is not even a left or
                     * right input channel. Something is really wrong in this
                     * case anyway. */

                } else if (on_lfe(b) && (r->flags & PA_RESAMPLER_PRODUCE_LFE)) {

                    /* We are not connected and an LFE. Let's average all
                     * channels for LFE. */

                    for (ic = 0; ic < n_ic; ic++)
                        m->map_table_f[oc][ic] = 1.0f / (float) n_ic;

                    /* Please note that a channel connected to LFE doesn't
                     * really count as connected. */

                    *lfe_remixed = true;
                }
            }
        } /* upmix done, so far every output should be connected*/
        /* downmix here, connect unconnected input here */
        /* check if output format is supported with downmix table */
        pa_channel_layout_index_t output_layout_index = pa_channel_map_to_index(&r->o_cm);
        
        for (ic = 0; ic < n_ic; ic++) {
            pa_channel_position_t a = r->i_cm.map[ic];
            if (ic_connected[ic]) {
                continue;
            } else if (on_center(a)) {
                ic_unconnected_center++;
            } else if (on_lfe(a)) {
                ic_unconnected_lfe++;
            }
        }

        if (output_layout_index != PA_CHANNEL_LAYOUT_OTHER) {
            for (ic = 0; ic < n_ic; ic++) {
                if (ic_connected[ic]) { continue; }
                pa_channel_position_t a = r->i_cm.map[ic];
                int a_downmix = pa_to_downmix_position(a);
                for (oc = 0; oc < n_oc; oc++) {
                    pa_channel_position_t b = r->o_cm.map[oc];
                    int b_downmix = pa_to_downmix_position(b);
                    m->map_table_f[oc][ic] =
                    (float)channelDownmixMatrix[output_layout_index][a_downmix][b_downmix]/(float)RESCALE_COEF;
                    /* force lfe downmix*/
                    if (on_lfe(a))
                        m->map_table_f[oc][ic] = .375f/(float)ic_unconnected_lfe;
                }
            }
        } else { /* channels that are not supported by downmix table */
            for (ic = 0; ic < n_ic; ic++) {
                pa_channel_position_t a = r->i_cm.map[ic];
                pa_channel_direction_t ic_direction = front_rear_side(r->i_cm.map[ic]);
                if (ic_connected[ic]) {
                    continue;
                }
                for (oc = 0; oc < n_oc; oc++) {
                    pa_channel_position_t b = r->o_cm.map[oc];
                    pa_channel_direction_t oc_direction = front_rear_side(r->o_cm.map[oc]);
                    if (on_left(a) && on_left(b)) {
                        m->map_table_f[oc][ic] = directionDownMixMatrix[ic_direction][oc_direction];
                    } else if (on_right(a) && on_right(b)) {
                        m->map_table_f[oc][ic] = directionDownMixMatrix[ic_direction][oc_direction];
                    } else if (on_center(a) && on_center(b)) {
                        m->map_table_f[oc][ic] = directionDownMixMatrix[ic_direction][oc_direction];
                        ic_unconnected_center_mixed_in = true;
                    } else if (on_lfe(a)) {
                        /* force lfe downmix */
                        m->map_table_f[oc][ic] = .375f / (float) ic_unconnected_lfe;
                    }
                }
            }
            if (ic_unconnected_center > 0 && !ic_unconnected_center_mixed_in) {
                unsigned ncenter[PA_CHANNELS_MAX];
                bool found_frs[PA_CHANNELS_MAX];

                memset(ncenter, 0, sizeof(ncenter));
                memset(found_frs, 0, sizeof(found_frs));

                /* Hmm, as it appears there was no center channel we
                could mix our center channel in. In this case, mix it into
                left and right. Using .5 as the factor. */

                for (ic = 0; ic < n_ic; ic++) {

                    if (ic_connected[ic])
                        continue;

                    if (!on_center(r->i_cm.map[ic]))
                        continue;

                    for (oc = 0; oc < n_oc; oc++) {

                        if (!on_left(r->o_cm.map[oc]) && !on_right(r->o_cm.map[oc]))
                            continue;

                        if (front_rear_side(r->i_cm.map[ic]) == front_rear_side(r->o_cm.map[oc])) {
                            found_frs[ic] = true;
                            break;
                        }
                    }

                    for (oc = 0; oc < n_oc; oc++) {

                        if (!on_left(r->o_cm.map[oc]) && !on_right(r->o_cm.map[oc]))
                            continue;

                        if (!found_frs[ic] || front_rear_side(r->i_cm.map[ic]) == front_rear_side(r->o_cm.map[oc]))
                            ncenter[oc]++;
                    }
                }

                for (oc = 0; oc < n_oc; oc++) {

                    if (!on_left(r->o_cm.map[oc]) && !on_right(r->o_cm.map[oc]))
                        continue;

                    if (ncenter[oc] <= 0)
                        continue;

                    for (ic = 0; ic < n_ic; ic++) {

                        if (!on_center(r->i_cm.map[ic]))
                            continue;

                        if (!found_frs[ic] || front_rear_side(r->i_cm.map[ic]) == front_rear_side(r->o_cm.map[oc]))
                            m->map_table_f[oc][ic] = .5f / (float) ncenter[oc];
                    }
                }
            }
        }
    }

    // uniform with maximum sum
    float max_sum = 0.0f;

    for (oc = 0; oc < n_oc; oc++) {
        float sum = 0.0f;
        for (ic = 0; ic < n_ic; ic++) {
            sum += m->map_table_f[oc][ic];
        }
        if (sum > max_sum) { max_sum = sum; }
    }
    for (oc = 0; oc < n_oc; oc++) {
        for (ic = 0; ic < n_ic; ic++) {
            m->map_table_f[oc][ic] /= max_sum;
        }
    }
    /* make an 16:16 int version of the matrix */
    for (oc = 0; oc < n_oc; oc++)
        for (ic = 0; ic < n_ic; ic++)
            m->map_table_i[oc][ic] = (int32_t) (m->map_table_f[oc][ic] * 0x10000);

    s = pa_strbuf_new();

    pa_strbuf_printf(s, "     ");
    for (ic = 0; ic < n_ic; ic++)
        pa_strbuf_printf(s, "  I%02u ", ic);
    pa_strbuf_puts(s, "\n    +");

    for (ic = 0; ic < n_ic; ic++)
        pa_strbuf_printf(s, "------");
    pa_strbuf_puts(s, "\n");

    for (oc = 0; oc < n_oc; oc++) {
        pa_strbuf_printf(s, "O%02u |", oc);

        for (ic = 0; ic < n_ic; ic++)
            pa_strbuf_printf(s, " %1.3f", m->map_table_f[oc][ic]);

        pa_strbuf_puts(s, "\n");
    }

    pa_log_debug("Channel matrix:\n%s", t = pa_strbuf_to_string_free(s));
    pa_xfree(t);

    /* initialize the remapping function */
    pa_init_remap_func(m);
}

static void free_remap(pa_remap_t *m) {
    pa_assert(m);

    pa_xfree(m->state);
}

/* check if buf's memblock is large enough to hold 'len' bytes; create a
 * new memblock if necessary and optionally preserve 'copy' data bytes */
static void fit_buf(pa_resampler *r, pa_memchunk *buf, size_t len, size_t *size, size_t copy) {
    pa_assert(size);

    if (!buf->memblock || len > *size) {
        pa_memblock *new_block = pa_memblock_new(r->mempool, len);

        if (buf->memblock) {
            if (copy > 0) {
                void *src = pa_memblock_acquire(buf->memblock);
                void *dst = pa_memblock_acquire(new_block);
                pa_assert(copy <= len);
                memcpy(dst, src, copy);
                pa_memblock_release(new_block);
                pa_memblock_release(buf->memblock);
            }

            pa_memblock_unref(buf->memblock);
        }

        buf->memblock = new_block;
        *size = len;
    }

    buf->length = len;
}

static pa_memchunk* convert_to_work_format(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_samples, out_n_samples;
    void *src, *dst;
    bool have_leftover;
    size_t leftover_length = 0;

    pa_assert(r);
    pa_assert(input);
    pa_assert(input->memblock);

    /* Convert the incoming sample into the work sample format and place them
     * in to_work_format_buf. The leftover data is already converted, so it's
     * part of the output buffer. */

    have_leftover = r->leftover_in_to_work;
    r->leftover_in_to_work = false;

    if (!have_leftover && (!r->to_work_format_func || !input->length))
        return input;
    else if (input->length <= 0)
        return &r->to_work_format_buf;

    in_n_samples = out_n_samples = (unsigned) ((input->length / r->i_fz) * r->i_ss.channels);

    if (have_leftover) {
        leftover_length = r->to_work_format_buf.length;
        out_n_samples += (unsigned) (leftover_length / r->w_sz);
    }

    fit_buf(r, &r->to_work_format_buf, r->w_sz * out_n_samples, &r->to_work_format_buf_size, leftover_length);

    src = pa_memblock_acquire_chunk(input);
    dst = (uint8_t *) pa_memblock_acquire(r->to_work_format_buf.memblock) + leftover_length;

    if (r->to_work_format_func)
        r->to_work_format_func(in_n_samples, src, dst);
    else
        memcpy(dst, src, input->length);

    pa_memblock_release(input->memblock);
    pa_memblock_release(r->to_work_format_buf.memblock);

    return &r->to_work_format_buf;
}

static pa_memchunk *remap_channels(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_samples, out_n_samples, in_n_frames, out_n_frames;
    void *src, *dst;
    size_t leftover_length = 0;
    bool have_leftover;

    pa_assert(r);
    pa_assert(input);
    pa_assert(input->memblock);

    /* Remap channels and place the result in remap_buf. There may be leftover
     * data in the beginning of remap_buf. The leftover data is already
     * remapped, so it's not part of the input, it's part of the output. */

    have_leftover = r->leftover_in_remap;
    r->leftover_in_remap = false;

    if (!have_leftover && (!r->map_required || input->length <= 0))
        return input;
    else if (input->length <= 0)
        return &r->remap_buf;

    in_n_samples = (unsigned) (input->length / r->w_sz);
    in_n_frames = out_n_frames = in_n_samples / r->i_ss.channels;

    if (have_leftover) {
        leftover_length = r->remap_buf.length;
        out_n_frames += leftover_length / r->w_fz;
    }

    out_n_samples = out_n_frames * r->o_ss.channels;
    fit_buf(r, &r->remap_buf, out_n_samples * r->w_sz, &r->remap_buf_size, leftover_length);

    src = pa_memblock_acquire_chunk(input);
    dst = (uint8_t *) pa_memblock_acquire(r->remap_buf.memblock) + leftover_length;

    if (r->map_required) {
        pa_remap_t *remap = &r->remap;

        pa_assert(remap->do_remap);
        remap->do_remap(remap, dst, src, in_n_frames);

    } else
        memcpy(dst, src, input->length);

    pa_memblock_release(input->memblock);
    pa_memblock_release(r->remap_buf.memblock);

    return &r->remap_buf;
}

static void save_leftover(pa_resampler *r, void *buf, size_t len) {
    void *dst;

    pa_assert(r);
    pa_assert(buf);
    pa_assert(len > 0);

    /* Store the leftover data. */
    fit_buf(r, r->leftover_buf, len, r->leftover_buf_size, 0);
    *r->have_leftover = true;

    dst = pa_memblock_acquire(r->leftover_buf->memblock);
    memmove(dst, buf, len);
    pa_memblock_release(r->leftover_buf->memblock);
}

static pa_memchunk *resample(pa_resampler *r, pa_memchunk *input) {
    unsigned in_n_frames, out_n_frames, leftover_n_frames;

    pa_assert(r);
    pa_assert(input);

    /* Resample the data and place the result in resample_buf. */

    if (!r->impl.resample || !input->length)
        return input;

    in_n_frames = (unsigned) (input->length / r->w_fz);

    out_n_frames = ((in_n_frames*r->o_ss.rate)/r->i_ss.rate)+EXTRA_FRAMES;
    fit_buf(r, &r->resample_buf, r->w_fz * out_n_frames, &r->resample_buf_size, 0);

    leftover_n_frames = r->impl.resample(r, input, in_n_frames, &r->resample_buf, &out_n_frames);

    if (leftover_n_frames > 0) {
        void *leftover_data = (uint8_t *) pa_memblock_acquire_chunk(input) + (in_n_frames - leftover_n_frames) * r->w_fz;
        save_leftover(r, leftover_data, leftover_n_frames * r->w_fz);
        pa_memblock_release(input->memblock);
    }

    r->resample_buf.length = out_n_frames * r->w_fz;

    return &r->resample_buf;
}

static pa_memchunk *convert_from_work_format(pa_resampler *r, pa_memchunk *input) {
    unsigned n_samples, n_frames;
    void *src, *dst;

    pa_assert(r);
    pa_assert(input);

    /* Convert the data into the correct sample type and place the result in
     * from_work_format_buf. */

    if (!r->from_work_format_func || !input->length)
        return input;

    n_samples = (unsigned) (input->length / r->w_sz);
    n_frames = n_samples / r->o_ss.channels;
    fit_buf(r, &r->from_work_format_buf, r->o_fz * n_frames, &r->from_work_format_buf_size, 0);

    src = pa_memblock_acquire_chunk(input);
    dst = pa_memblock_acquire(r->from_work_format_buf.memblock);
    r->from_work_format_func(n_samples, src, dst);
    pa_memblock_release(input->memblock);
    pa_memblock_release(r->from_work_format_buf.memblock);

    return &r->from_work_format_buf;
}

void pa_resampler_run(pa_resampler *r, const pa_memchunk *in, pa_memchunk *out) {
    pa_memchunk *buf;

    pa_assert(r);
    pa_assert(in);
    pa_assert(out);
    pa_assert(in->length);
    pa_assert(in->memblock);
    pa_assert(in->length % r->i_fz == 0);

    /* If first frame and resampler doesn't init, push one frame to init.
     * Otherwise, resamplers demand 2 or 3 times to output*/
    if ((r->in_frames == 0) && (r->i_ss.rate != r->o_ss.rate)) {
        pa_memchunk dumpBuf;
        buf = &dumpBuf;
        size_t tempLength = r->i_fz * RESAMPLER_CACHE_SIZE_RATIO * r->o_fz;
        buf->length = tempLength;
        buf->memblock = pa_memblock_new(r->mempool, tempLength);
        buf->index = 0;
        
        // silence memblock
        void *data = pa_memblock_acquire(buf->memblock);
        memset(data, 0, buf->length);

        resample(r, buf);
        pa_memblock_release(buf->memblock);
        pa_memblock_unref(buf->memblock);
    }

    buf = (pa_memchunk*) in;
    r->in_frames += buf->length / r->i_fz;
    buf = convert_to_work_format(r, buf);

    /* Try to save resampling effort: if we have more output channels than
     * input channels, do resampling first, then remapping. */
    if (r->o_ss.channels <= r->i_ss.channels) {
        buf = remap_channels(r, buf);
        buf = resample(r, buf);
    } else {
        buf = resample(r, buf);
        buf = remap_channels(r, buf);
    }

    if (r->lfe_filter)
        buf = pa_lfe_filter_process(r->lfe_filter, buf);

    if (buf->length) {
        buf = convert_from_work_format(r, buf);
        *out = *buf;
        r->out_frames += buf->length / r->o_fz;

        if (buf == in)
            pa_memblock_ref(buf->memblock);
        else
            pa_memchunk_reset(buf);
    } else
        pa_memchunk_reset(out);
}

/* Get delay in input frames. Some resamplers may have negative delay. */
double pa_resampler_get_delay(pa_resampler *r, bool allow_negative) {
    double frames;

    frames = r->out_frames * r->i_ss.rate / r->o_ss.rate;
    if (frames >= r->in_frames && !allow_negative)
        return 0;
    return r->in_frames - frames;
}

/* Get delay in usec */
pa_usec_t pa_resampler_get_delay_usec(pa_resampler *r) {

    if (!r)
        return 0;

    return (pa_usec_t) (pa_resampler_get_delay(r, false) * PA_USEC_PER_SEC / r->i_ss.rate);
}

/* Get GCD of input and output rate. */
unsigned pa_resampler_get_gcd(pa_resampler *r) {
    pa_assert(r);

    return r->gcd;
}

/* Get maximum resampler history. The resamplers have finite impulse response, so really old
 * data (more than 2x the resampler latency) cannot affect the output. This means, that in an
 * ideal case, we should re-run 2 - 3 times the resampler delay through the resampler when it
 * is rewound. On the other hand this would mean for high sample rates that more than 25000
 * samples would need to be used (384k * 33ms). Therefore limit the history to 1.5 times the
 * maximum resampler delay, which should be fully sufficient in most cases and allows to run
 * at least more than one delay through the resampler in case of high rates. */
size_t pa_resampler_get_max_history(pa_resampler *r) {

    if (!r)
        return 0;

    return (uint64_t) PA_RESAMPLER_MAX_DELAY_USEC * r->i_ss.rate * 3 / PA_USEC_PER_SEC / 2;
}

/*** copy (noop) implementation ***/

static int copy_init(pa_resampler *r) {
    pa_assert(r);

    pa_assert(r->o_ss.rate == r->i_ss.rate);

    return 0;
}
