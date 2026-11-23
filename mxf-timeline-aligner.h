#ifndef MXF_TIMELINE_ALIGNER_H
#define MXF_TIMELINE_ALIGNER_H

#include "mxf-audio-pairing.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief States of the Timeline Aligner state machine.
 */
typedef enum {
	MXF_ALIGNER_STATE_INITIALIZING = 0,     /**< Waiting for initial PTS anchor from both tracks */
	MXF_ALIGNER_STATE_WAITING_FOR_TRACK = 1,/**< One track has data, waiting for the partner track */
	MXF_ALIGNER_STATE_ALIGNED = 2,          /**< Both tracks locked on same timeline, streaming stereo */
	MXF_ALIGNER_STATE_RESYNC_REQUIRED = 3,  /**< PTS jump/gap detected, resynchronization needed */
	MXF_ALIGNER_STATE_ERROR = 4,            /**< Unrecoverable error (e.g. invalid format or params) */
} mxf_aligner_state_t;

/**
 * @brief Policy for handling initial PTS offset between Track L and Track R.
 */
typedef enum {
	MXF_ALIGN_POLICY_DROP_EARLY = 0,  /**< Drop unpartnered early samples; play when both exist */
	MXF_ALIGN_POLICY_PAD_SILENCE = 1, /**< Pad silence for later track; play from earliest PTS */
} mxf_initial_align_policy_t;

/**
 * @brief Configuration parameters for timeline alignment.
 */
typedef struct {
	uint32_t sample_rate;                   /**< Sample rate (e.g. 48000 Hz) */
	mxf_audio_format_t format;              /**< Audio format (S16 or FLOAT) */
	mxf_initial_align_policy_t initial_policy; /**< Alignment policy for initial offset */
	uint32_t max_wait_samples;              /**< Maximum samples to buffer before drop/timeout */
	uint32_t discontinuity_threshold_samples;/**< Maximum sample jitter before declaring discontinuity */
	uint64_t timebase_den;                  /**< Units of PTS per second (default: 1,000,000,000 ns) */
} mxf_aligner_config_t;

/**
 * @brief Statistics for diagnostics and monitoring.
 */
typedef struct {
	uint64_t total_stereo_frames_out;       /**< Total interleaved stereo frames produced */
	uint64_t total_samples_dropped_l;       /**< Samples dropped from Track L (offset or overflow) */
	uint64_t total_samples_dropped_r;       /**< Samples dropped from Track R (offset or overflow) */
	uint64_t total_silence_frames_padded;   /**< Silence frames generated for padding */
	uint64_t resync_events;                 /**< Number of discontinuity/resync events */
	int64_t initial_offset_samples;         /**< Detected initial offset in samples (L - R) */
} mxf_aligner_stats_t;

/**
 * @brief Timeline Aligner context.
 */
typedef struct {
	dual_spsc_ring_buffer_t *dual;          /**< Reference to Dual SPSC Ring Buffer (optional) */
	spsc_ring_buffer_t *queue_l;            /**< Left track SPSC queue pointer */
	spsc_ring_buffer_t *queue_r;            /**< Right track SPSC queue pointer */
	mxf_aligner_config_t config;            /**< Configuration */
	mxf_aligner_state_t state;              /**< Current state machine status */
	mxf_aligner_stats_t stats;              /**< Diagnostics counters */

	int64_t current_timeline_pos;           /**< Unified timeline sample position */
	int64_t track_l_next_expected_pos;      /**< Next expected sample position for Track L */
	int64_t track_r_next_expected_pos;      /**< Next expected sample position for Track R */

	/* Temporary scratch buffer for reading from separate L/R queues before interleave */
	uint8_t *scratch_l;
	uint8_t *scratch_r;
	uint32_t scratch_capacity_samples;
	uint32_t active_epoch;                  /**< Current active seek/flush epoch */
} mxf_timeline_aligner_t;

/**
 * @brief Initialize a Timeline Aligner.
 *
 * @param aligner Pointer to the aligner structure.
 * @param dual Pointer to the initialized dual SPSC ring buffer (can be NULL if queues set later).
 * @param config Pointer to configuration parameters (NULL for defaults).
 * @return true on success, false on error.
 */
bool mxf_timeline_aligner_init(mxf_timeline_aligner_t *aligner, dual_spsc_ring_buffer_t *dual,
			       const mxf_aligner_config_t *config);

/**
 * @brief Dynamically set or change the active Left and Right SPSC queues.
 */
void mxf_timeline_aligner_set_queues(mxf_timeline_aligner_t *aligner, spsc_ring_buffer_t *ql,
				     spsc_ring_buffer_t *qr);

/**
 * @brief Destroy a Timeline Aligner and free internal scratch buffers.
 */
void mxf_timeline_aligner_destroy(mxf_timeline_aligner_t *aligner);

/**
 * @brief Reset aligner state back to INITIALIZING without destroying memory.
 */
void mxf_timeline_aligner_reset(mxf_timeline_aligner_t *aligner);

/**
 * @brief Set the active minimum epoch (discards stale blocks from earlier epochs).
 */
void mxf_timeline_aligner_set_epoch(mxf_timeline_aligner_t *aligner, uint32_t epoch);

/**
 * @brief Get current active epoch.
 */
uint32_t mxf_timeline_aligner_get_epoch(const mxf_timeline_aligner_t *aligner);

/**
 * @brief Update audio format and sample rate (resets alignment state).
 */
bool mxf_timeline_aligner_set_format(mxf_timeline_aligner_t *aligner, mxf_audio_format_t fmt, uint32_t sample_rate);

/**
 * @brief Process and interleave aligned stereo frames from Dual SPSC buffers.
 *
 * Consumer thread only.
 *
 * @param aligner Pointer to the aligner structure.
 * @param out_stereo Buffer to receive interleaved stereo samples [L0, R0, L1, R1...].
 * @param max_stereo_frames Maximum number of stereo frames to output (capacity of out_stereo).
 * @param out_pts Pointer to receive the PTS of the first output stereo frame in nanoseconds.
 * @return Number of stereo frames actually produced.
 */
uint32_t mxf_timeline_aligner_process(mxf_timeline_aligner_t *aligner, void *out_stereo,
				      uint32_t max_stereo_frames, int64_t *out_pts);

/**
 * @brief Get current state of the aligner.
 */
mxf_aligner_state_t mxf_timeline_aligner_get_state(const mxf_timeline_aligner_t *aligner);

/**
 * @brief Get copy of current statistics.
 */
void mxf_timeline_aligner_get_stats(const mxf_timeline_aligner_t *aligner, mxf_aligner_stats_t *out_stats);

/**
 * @brief Convert PTS to integer timeline sample position.
 */
int64_t mxf_pts_to_sample_pos(int64_t pts, uint32_t sample_rate, uint64_t timebase_den);

/**
 * @brief Convert integer timeline sample position to PTS.
 */
int64_t mxf_sample_pos_to_pts(int64_t sample_pos, uint32_t sample_rate, uint64_t timebase_den);

#ifdef __cplusplus
}
#endif

#endif /* MXF_TIMELINE_ALIGNER_H */
