#include "mxf-timeline-aligner.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMEBASE_DEN 1000000000ULL /* Nanoseconds */
#define DEFAULT_MAX_WAIT_SAMPLES 4800       /* 100ms at 48kHz */
#define DEFAULT_DISCONTINUITY_THRES 1920    /* 40ms at 48kHz (~1 frame at 25fps) */
#define SCRATCH_MAX_FRAMES 4096

int64_t mxf_pts_to_sample_pos(int64_t pts, uint32_t sample_rate, uint64_t timebase_den)
{
	if (timebase_den == 0 || sample_rate == 0)
		return 0;

#if defined(__SIZEOF_INT128__)
	__int128 p = (__int128)pts * (__int128)sample_rate;
	return (int64_t)(p / (__int128)timebase_den);
#else
	int64_t sec = pts / (int64_t)timebase_den;
	int64_t rem = pts % (int64_t)timebase_den;
	return sec * (int64_t)sample_rate + (rem * (int64_t)sample_rate) / (int64_t)timebase_den;
#endif
}

int64_t mxf_sample_pos_to_pts(int64_t sample_pos, uint32_t sample_rate, uint64_t timebase_den)
{
	if (sample_rate == 0)
		return 0;

#if defined(__SIZEOF_INT128__)
	__int128 p = (__int128)sample_pos * (__int128)timebase_den;
	return (int64_t)(p / (__int128)sample_rate);
#else
	int64_t sec = sample_pos / (int64_t)sample_rate;
	int64_t rem = sample_pos % (int64_t)sample_rate;
	return sec * (int64_t)timebase_den + (rem * (int64_t)timebase_den) / (int64_t)sample_rate;
#endif
}

bool mxf_timeline_aligner_init(mxf_timeline_aligner_t *aligner, dual_spsc_ring_buffer_t *dual,
			       const mxf_aligner_config_t *config)
{
	if (!aligner)
		return false;

	memset(aligner, 0, sizeof(*aligner));
	aligner->dual = dual;
	if (dual) {
		aligner->queue_l = &dual->queue_l;
		aligner->queue_r = &dual->queue_r;
	}

	if (config) {
		aligner->config = *config;
	} else {
		aligner->config.sample_rate = (dual && dual->sample_rate) ? dual->sample_rate : 48000;
		aligner->config.format = dual ? dual->format : MXF_AUDIO_FMT_S16;
		aligner->config.initial_policy = MXF_ALIGN_POLICY_DROP_EARLY;
		aligner->config.max_wait_samples = DEFAULT_MAX_WAIT_SAMPLES;
		aligner->config.discontinuity_threshold_samples = DEFAULT_DISCONTINUITY_THRES;
		aligner->config.timebase_den = DEFAULT_TIMEBASE_DEN;
	}

	if (aligner->config.timebase_den == 0)
		aligner->config.timebase_den = DEFAULT_TIMEBASE_DEN;
	if (aligner->config.max_wait_samples == 0)
		aligner->config.max_wait_samples = DEFAULT_MAX_WAIT_SAMPLES;
	if (aligner->config.discontinuity_threshold_samples == 0)
		aligner->config.discontinuity_threshold_samples = DEFAULT_DISCONTINUITY_THRES;

	/* Pre-allocate scratch buffers for non-interleaved reads */
	uint32_t sample_size = (aligner->config.format == MXF_AUDIO_FMT_FLOAT) ? sizeof(float) : sizeof(int16_t);
	aligner->scratch_capacity_samples = SCRATCH_MAX_FRAMES;
	aligner->scratch_l = (uint8_t *)malloc((size_t)aligner->scratch_capacity_samples * sample_size);
	aligner->scratch_r = (uint8_t *)malloc((size_t)aligner->scratch_capacity_samples * sample_size);

	if (!aligner->scratch_l || !aligner->scratch_r) {
		mxf_timeline_aligner_destroy(aligner);
		return false;
	}

	aligner->state = MXF_ALIGNER_STATE_INITIALIZING;
	return true;
}

void mxf_timeline_aligner_set_queues(mxf_timeline_aligner_t *aligner, spsc_ring_buffer_t *ql,
				     spsc_ring_buffer_t *qr)
{
	if (!aligner)
		return;
	if (aligner->queue_l != ql || aligner->queue_r != qr) {
		aligner->queue_l = ql;
		aligner->queue_r = qr;
		mxf_timeline_aligner_reset(aligner);
	}
}

void mxf_timeline_aligner_destroy(mxf_timeline_aligner_t *aligner)
{
	if (!aligner)
		return;

	if (aligner->scratch_l) {
		free(aligner->scratch_l);
		aligner->scratch_l = NULL;
	}
	if (aligner->scratch_r) {
		free(aligner->scratch_r);
		aligner->scratch_r = NULL;
	}
	aligner->scratch_capacity_samples = 0;
	aligner->state = MXF_ALIGNER_STATE_ERROR;
}

void mxf_timeline_aligner_reset(mxf_timeline_aligner_t *aligner)
{
	if (!aligner)
		return;

	aligner->state = MXF_ALIGNER_STATE_INITIALIZING;
	aligner->current_timeline_pos = 0;
	aligner->track_l_next_expected_pos = 0;
	aligner->track_r_next_expected_pos = 0;
}

void mxf_timeline_aligner_set_epoch(mxf_timeline_aligner_t *aligner, uint32_t epoch)
{
	if (!aligner)
		return;

	aligner->active_epoch = epoch;
	mxf_timeline_aligner_reset(aligner);
	if (aligner->queue_l)
		spsc_ring_buffer_discard_stale_epochs(aligner->queue_l, epoch);
	if (aligner->queue_r)
		spsc_ring_buffer_discard_stale_epochs(aligner->queue_r, epoch);
}

uint32_t mxf_timeline_aligner_get_epoch(const mxf_timeline_aligner_t *aligner)
{
	return aligner ? aligner->active_epoch : 0;
}

bool mxf_timeline_aligner_set_format(mxf_timeline_aligner_t *aligner, mxf_audio_format_t fmt, uint32_t sample_rate)
{
	if (!aligner || sample_rate == 0)
		return false;

	aligner->config.format = fmt;
	aligner->config.sample_rate = sample_rate;

	/* Reallocate scratch if sample size changed */
	uint32_t sample_size = (fmt == MXF_AUDIO_FMT_FLOAT) ? sizeof(float) : sizeof(int16_t);
	uint8_t *nl = (uint8_t *)realloc(aligner->scratch_l, (size_t)aligner->scratch_capacity_samples * sample_size);
	uint8_t *nr = (uint8_t *)realloc(aligner->scratch_r, (size_t)aligner->scratch_capacity_samples * sample_size);
	if (!nl || !nr)
		return false;

	aligner->scratch_l = nl;
	aligner->scratch_r = nr;

	mxf_timeline_aligner_reset(aligner);
	return true;
}

mxf_aligner_state_t mxf_timeline_aligner_get_state(const mxf_timeline_aligner_t *aligner)
{
	return aligner ? aligner->state : MXF_ALIGNER_STATE_ERROR;
}

void mxf_timeline_aligner_get_stats(const mxf_timeline_aligner_t *aligner, mxf_aligner_stats_t *out_stats)
{
	if (aligner && out_stats)
		*out_stats = aligner->stats;
}

/* ------------------------------------------------------------------------- */
/* State machine processing logic                                            */
/* ------------------------------------------------------------------------- */

static bool process_initial_alignment(mxf_timeline_aligner_t *aligner, void *out_stereo,
				      uint32_t max_stereo_frames, int64_t *out_pts, uint32_t *out_produced)
{
	*out_produced = 0;
	spsc_ring_buffer_t *q_l = aligner->queue_l;
	spsc_ring_buffer_t *q_r = aligner->queue_r;

	if (!q_l || !q_r)
		return false;

	uint32_t avail_l = spsc_ring_buffer_available_read(q_l);
	uint32_t avail_r = spsc_ring_buffer_available_read(q_r);

	/* Check if one track is missing and the other has exceeded max wait buffer */
	if (avail_l > 0 && avail_r == 0) {
		if (avail_l >= aligner->config.max_wait_samples) {
			aligner->state = MXF_ALIGNER_STATE_WAITING_FOR_TRACK;
			/* Drop excess to prevent overflow while waiting */
			uint32_t to_drop = avail_l - aligner->config.max_wait_samples;
			if (to_drop > 0) {
				spsc_ring_buffer_discard(q_l, to_drop);
				aligner->stats.total_samples_dropped_l += to_drop;
			}
		}
		return false;
	}

	if (avail_r > 0 && avail_l == 0) {
		if (avail_r >= aligner->config.max_wait_samples) {
			aligner->state = MXF_ALIGNER_STATE_WAITING_FOR_TRACK;
			uint32_t to_drop = avail_r - aligner->config.max_wait_samples;
			if (to_drop > 0) {
				spsc_ring_buffer_discard(q_r, to_drop);
				aligner->stats.total_samples_dropped_r += to_drop;
			}
		}
		return false;
	}

	if (avail_l == 0 || avail_r == 0)
		return false;

	/* Both tracks have data: get their respective starting PTS */
	int64_t pts_l = 0, pts_r = 0;
	if (!spsc_ring_buffer_get_front_pts(q_l, &pts_l) ||
	    !spsc_ring_buffer_get_front_pts(q_r, &pts_r)) {
		return false;
	}

	int64_t pos_l = mxf_pts_to_sample_pos(pts_l, aligner->config.sample_rate, aligner->config.timebase_den);
	int64_t pos_r = mxf_pts_to_sample_pos(pts_r, aligner->config.sample_rate, aligner->config.timebase_den);
	int64_t offset = pos_l - pos_r;
	aligner->stats.initial_offset_samples = offset;

	if (pos_l == pos_r) {
		/* Perfectly aligned */
		aligner->current_timeline_pos = pos_l;
		aligner->track_l_next_expected_pos = pos_l;
		aligner->track_r_next_expected_pos = pos_r;
		aligner->state = MXF_ALIGNER_STATE_ALIGNED;
		return true;
	}

	if (pos_l < pos_r) {
		/* Track L started earlier than Track R by diff samples */
		uint32_t diff = (uint32_t)(pos_r - pos_l);

		if (aligner->config.initial_policy == MXF_ALIGN_POLICY_DROP_EARLY) {
			uint32_t dropped = spsc_ring_buffer_discard(q_l, diff);
			aligner->stats.total_samples_dropped_l += dropped;
			if (dropped < diff) {
				/* Queue L didn't have enough samples to reach pos_r yet */
				return false;
			}
			aligner->current_timeline_pos = pos_r;
			aligner->track_l_next_expected_pos = pos_r;
			aligner->track_r_next_expected_pos = pos_r;
			aligner->state = MXF_ALIGNER_STATE_ALIGNED;
			return true;
		} else {
			/* PAD_SILENCE policy: play Track L and pad R with silence up to diff */
			uint32_t pad_frames = diff;
			if (pad_frames > max_stereo_frames)
				pad_frames = max_stereo_frames;
			if (pad_frames > avail_l)
				pad_frames = avail_l;
			if (pad_frames > aligner->scratch_capacity_samples)
				pad_frames = aligner->scratch_capacity_samples;

			if (pad_frames > 0) {
				spsc_ring_buffer_read(q_l, aligner->scratch_l, pad_frames, NULL);
				if (out_pts)
					*out_pts = mxf_sample_pos_to_pts(pos_l, aligner->config.sample_rate, aligner->config.timebase_den);

				if (aligner->config.format == MXF_AUDIO_FMT_S16) {
					int16_t *src_l = (int16_t *)aligner->scratch_l;
					int16_t *dst = (int16_t *)out_stereo;
					for (uint32_t i = 0; i < pad_frames; i++) {
						dst[2 * i] = src_l[i];
						dst[2 * i + 1] = 0; /* Silence on R */
					}
				} else {
					float *src_l = (float *)aligner->scratch_l;
					float *dst = (float *)out_stereo;
					for (uint32_t i = 0; i < pad_frames; i++) {
						dst[2 * i] = src_l[i];
						dst[2 * i + 1] = 0.0f;
					}
				}

				aligner->stats.total_silence_frames_padded += pad_frames;
				aligner->stats.total_stereo_frames_out += pad_frames;
				*out_produced = pad_frames;

				if (pad_frames == diff) {
					aligner->current_timeline_pos = pos_r;
					aligner->track_l_next_expected_pos = pos_r;
					aligner->track_r_next_expected_pos = pos_r;
					aligner->state = MXF_ALIGNER_STATE_ALIGNED;
				}
				return true;
			}
			return false;
		}
	} else {
		/* Track R started earlier than Track L by diff samples */
		uint32_t diff = (uint32_t)(pos_l - pos_r);

		if (aligner->config.initial_policy == MXF_ALIGN_POLICY_DROP_EARLY) {
			uint32_t dropped = spsc_ring_buffer_discard(q_r, diff);
			aligner->stats.total_samples_dropped_r += dropped;
			if (dropped < diff) {
				return false;
			}
			aligner->current_timeline_pos = pos_l;
			aligner->track_l_next_expected_pos = pos_l;
			aligner->track_r_next_expected_pos = pos_l;
			aligner->state = MXF_ALIGNER_STATE_ALIGNED;
			return true;
		} else {
			/* PAD_SILENCE on L */
			uint32_t pad_frames = diff;
			if (pad_frames > max_stereo_frames)
				pad_frames = max_stereo_frames;
			if (pad_frames > avail_r)
				pad_frames = avail_r;
			if (pad_frames > aligner->scratch_capacity_samples)
				pad_frames = aligner->scratch_capacity_samples;

			if (pad_frames > 0) {
				spsc_ring_buffer_read(q_r, aligner->scratch_r, pad_frames, NULL);
				if (out_pts)
					*out_pts = mxf_sample_pos_to_pts(pos_r, aligner->config.sample_rate, aligner->config.timebase_den);

				if (aligner->config.format == MXF_AUDIO_FMT_S16) {
					int16_t *src_r = (int16_t *)aligner->scratch_r;
					int16_t *dst = (int16_t *)out_stereo;
					for (uint32_t i = 0; i < pad_frames; i++) {
						dst[2 * i] = 0; /* Silence on L */
						dst[2 * i + 1] = src_r[i];
					}
				} else {
					float *src_r = (float *)aligner->scratch_r;
					float *dst = (float *)out_stereo;
					for (uint32_t i = 0; i < pad_frames; i++) {
						dst[2 * i] = 0.0f;
						dst[2 * i + 1] = src_r[i];
					}
				}

				aligner->stats.total_silence_frames_padded += pad_frames;
				aligner->stats.total_stereo_frames_out += pad_frames;
				*out_produced = pad_frames;

				if (pad_frames == diff) {
					aligner->current_timeline_pos = pos_l;
					aligner->track_l_next_expected_pos = pos_l;
					aligner->track_r_next_expected_pos = pos_l;
					aligner->state = MXF_ALIGNER_STATE_ALIGNED;
				}
				return true;
			}
			return false;
		}
	}
}

uint32_t mxf_timeline_aligner_process(mxf_timeline_aligner_t *aligner, void *out_stereo,
				      uint32_t max_stereo_frames, int64_t *out_pts)
{
	if (!aligner || !aligner->queue_l || !aligner->queue_r || !out_stereo || max_stereo_frames == 0)
		return 0;

	spsc_ring_buffer_t *q_l = aligner->queue_l;
	spsc_ring_buffer_t *q_r = aligner->queue_r;

	/* --- STALE BLOCK DROPPING & EPOCH COORDINATION --- */
	/* 1. Drop any stale blocks older than active_epoch */
	if (aligner->active_epoch > 0) {
		spsc_ring_buffer_discard_stale_epochs(q_l, aligner->active_epoch);
		spsc_ring_buffer_discard_stale_epochs(q_r, aligner->active_epoch);
	}

	/* 2. Check if a newer epoch has arrived on either queue */
	int64_t meta_pts_l = 0, meta_pts_r = 0;
	uint32_t meta_cnt_l = 0, meta_cnt_r = 0;
	uint32_t epoch_l = 0, epoch_r = 0;
	bool has_meta_l = spsc_ring_buffer_peek_meta(q_l, &meta_pts_l, &meta_cnt_l, &epoch_l);
	bool has_meta_r = spsc_ring_buffer_peek_meta(q_r, &meta_pts_r, &meta_cnt_r, &epoch_r);

	if (has_meta_l && epoch_l > aligner->active_epoch) {
		aligner->active_epoch = epoch_l;
		mxf_timeline_aligner_reset(aligner);
		spsc_ring_buffer_discard_stale_epochs(q_r, aligner->active_epoch);
	}
	if (has_meta_r && epoch_r > aligner->active_epoch) {
		aligner->active_epoch = epoch_r;
		mxf_timeline_aligner_reset(aligner);
		spsc_ring_buffer_discard_stale_epochs(q_l, aligner->active_epoch);
	}

	/* 3. Cross-Epoch Isolation: Enforce invariant epoch_l == epoch_r == active_epoch */
	if (has_meta_l && has_meta_r) {
		if (epoch_l != epoch_r || epoch_l != aligner->active_epoch) {
			if (epoch_l < epoch_r) {
				spsc_ring_buffer_discard_stale_epochs(q_l, epoch_r);
				aligner->active_epoch = epoch_r;
				mxf_timeline_aligner_reset(aligner);
			} else if (epoch_r < epoch_l) {
				spsc_ring_buffer_discard_stale_epochs(q_r, epoch_l);
				aligner->active_epoch = epoch_l;
				mxf_timeline_aligner_reset(aligner);
			}
			return 0; // Never interleave across mismatched epochs!
		}
	}

	/* State: RESYNC_REQUIRED -> Reset pointers and transition back to INITIALIZING */
	if (aligner->state == MXF_ALIGNER_STATE_RESYNC_REQUIRED) {
		aligner->state = MXF_ALIGNER_STATE_INITIALIZING;
	}

	/* State: WAITING_FOR_TRACK -> check if partner has arrived */
	if (aligner->state == MXF_ALIGNER_STATE_WAITING_FOR_TRACK) {
		uint32_t al = spsc_ring_buffer_available_read(q_l);
		uint32_t ar = spsc_ring_buffer_available_read(q_r);
		if (al > 0 && ar > 0) {
			aligner->state = MXF_ALIGNER_STATE_INITIALIZING;
		} else {
			return 0;
		}
	}

	/* State: INITIALIZING -> Perform timeline anchor alignment */
	if (aligner->state == MXF_ALIGNER_STATE_INITIALIZING) {
		uint32_t produced = 0;
		if (!process_initial_alignment(aligner, out_stereo, max_stereo_frames, out_pts, &produced))
			return 0;
		if (produced > 0)
			return produced;
	}

	/* Check for PTS discontinuity on incoming queue blocks */
	int64_t check_pts_l = 0, check_pts_r = 0;
	if (spsc_ring_buffer_get_front_pts(q_l, &check_pts_l)) {
		int64_t actual_pos_l = mxf_pts_to_sample_pos(check_pts_l, aligner->config.sample_rate, aligner->config.timebase_den);
		int64_t diff_l = actual_pos_l - aligner->track_l_next_expected_pos;
		if (diff_l < 0) diff_l = -diff_l;
		if (diff_l > (int64_t)aligner->config.discontinuity_threshold_samples) {
			aligner->stats.resync_events++;
			aligner->state = MXF_ALIGNER_STATE_RESYNC_REQUIRED;
			return 0;
		}
	}

	if (spsc_ring_buffer_get_front_pts(q_r, &check_pts_r)) {
		int64_t actual_pos_r = mxf_pts_to_sample_pos(check_pts_r, aligner->config.sample_rate, aligner->config.timebase_den);
		int64_t diff_r = actual_pos_r - aligner->track_r_next_expected_pos;
		if (diff_r < 0) diff_r = -diff_r;
		if (diff_r > (int64_t)aligner->config.discontinuity_threshold_samples) {
			aligner->stats.resync_events++;
			aligner->state = MXF_ALIGNER_STATE_RESYNC_REQUIRED;
			return 0;
		}
	}

	/* Interleave aligned samples */
	uint32_t avail_l = spsc_ring_buffer_available_read(q_l);
	uint32_t avail_r = spsc_ring_buffer_available_read(q_r);

	/* Check if one track was lost while in aligned state */
	if (avail_l >= aligner->config.max_wait_samples && avail_r == 0) {
		aligner->state = MXF_ALIGNER_STATE_WAITING_FOR_TRACK;
		uint32_t to_drop = avail_l - aligner->config.max_wait_samples;
		if (to_drop > 0) {
			spsc_ring_buffer_discard(q_l, to_drop);
			aligner->stats.total_samples_dropped_l += to_drop;
		}
		return 0;
	}
	if (avail_r >= aligner->config.max_wait_samples && avail_l == 0) {
		aligner->state = MXF_ALIGNER_STATE_WAITING_FOR_TRACK;
		uint32_t to_drop = avail_r - aligner->config.max_wait_samples;
		if (to_drop > 0) {
			spsc_ring_buffer_discard(q_r, to_drop);
			aligner->stats.total_samples_dropped_r += to_drop;
		}
		return 0;
	}

	uint32_t frames_to_interleave = (avail_l < avail_r) ? avail_l : avail_r;
	if (frames_to_interleave == 0)
		return 0;

	if (frames_to_interleave > max_stereo_frames)
		frames_to_interleave = max_stereo_frames;
	if (frames_to_interleave > aligner->scratch_capacity_samples)
		frames_to_interleave = aligner->scratch_capacity_samples;

	/* Read from Queue L and Queue R into separate scratch buffers */
	uint32_t read_l = spsc_ring_buffer_read(q_l, aligner->scratch_l, frames_to_interleave, NULL);
	uint32_t read_r = spsc_ring_buffer_read(q_r, aligner->scratch_r, frames_to_interleave, NULL);
	if (read_l != frames_to_interleave || read_r != frames_to_interleave) {
		/* Unexpected read mismatch */
		aligner->state = MXF_ALIGNER_STATE_ERROR;
		return 0;
	}

	if (out_pts) {
		*out_pts = mxf_sample_pos_to_pts(aligner->current_timeline_pos, aligner->config.sample_rate, aligner->config.timebase_den);
	}

	/* Interleave into out_stereo: [L0, R0, L1, R1, ...] */
	if (aligner->config.format == MXF_AUDIO_FMT_S16) {
		const int16_t *src_l = (const int16_t *)aligner->scratch_l;
		const int16_t *src_r = (const int16_t *)aligner->scratch_r;
		int16_t *dst = (int16_t *)out_stereo;

		for (uint32_t i = 0; i < frames_to_interleave; i++) {
			dst[2 * i] = src_l[i];
			dst[2 * i + 1] = src_r[i];
		}
	} else {
		const float *src_l = (const float *)aligner->scratch_l;
		const float *src_r = (const float *)aligner->scratch_r;
		float *dst = (float *)out_stereo;

		for (uint32_t i = 0; i < frames_to_interleave; i++) {
			dst[2 * i] = src_l[i];
			dst[2 * i + 1] = src_r[i];
		}
	}

	aligner->current_timeline_pos += frames_to_interleave;
	aligner->track_l_next_expected_pos += frames_to_interleave;
	aligner->track_r_next_expected_pos += frames_to_interleave;
	aligner->stats.total_stereo_frames_out += frames_to_interleave;

	return frames_to_interleave;
}
