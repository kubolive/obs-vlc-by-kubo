#ifndef MXF_AUDIO_PAIRING_H
#define MXF_AUDIO_PAIRING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define MXF_ATOMIC(T) _Atomic(T)
#elif defined(_MSC_VER)
/* MSVC fallback if C11 stdatomic is not active */
#include <windows.h>
#define MXF_ATOMIC(T) volatile T
#else
#include <stdatomic.h>
#define MXF_ATOMIC(T) _Atomic(T)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supported audio sample formats in the SPSC ring buffer.
 */
typedef enum {
	MXF_AUDIO_FMT_S16 = 0,   /**< 16-bit signed integer (int16_t, 2 bytes/sample) */
	MXF_AUDIO_FMT_FLOAT = 1, /**< 32-bit floating point (float, 4 bytes/sample) */
} mxf_audio_format_t;

/**
 * @brief Maximum metadata entries tracking contiguous audio blocks in the ring buffer.
 */
#define MXF_MAX_BLOCK_ENTRIES 256

/**
 * @brief Metadata for a single block written into the SPSC buffer.
 */
typedef struct {
	int64_t pts;             /**< Presentation timestamp in nanoseconds */
	uint32_t sample_count;   /**< Number of samples in this block */
	uint32_t epoch;          /**< Flush/seek generation epoch of this block */
} mxf_block_meta_t;

/**
 * @brief Single Producer Single Consumer (SPSC) Lock-Free Audio Ring Buffer.
 *
 * Thread Ownership Rules:
 * - PRODUCER: Exactly one thread (the LibVLC audio playback callback for this track).
 *             Only the producer invokes `spsc_ring_buffer_write()`.
 *             Producer updates `write_idx` with memory_order_release and
 *             reads `read_idx` with memory_order_acquire.
 *
 * - CONSUMER: Exactly one thread (the audio worker / OBS audio output pipeline).
 *             Only the consumer invokes `spsc_ring_buffer_read()`,
 *             `spsc_ring_buffer_peek()`, and `spsc_ring_buffer_discard()`.
 *             Consumer updates `read_idx` with memory_order_release and
 *             reads `write_idx` with memory_order_acquire.
 *
 * Memory Safety:
 * - NO dynamic allocations during runtime (malloc/free) in write/read calls.
 * - NO mutex locks in audio callback data paths.
 */
typedef struct {
	uint8_t *buffer;                    /**< Pre-allocated memory for samples */
	uint32_t capacity_samples;          /**< Total sample capacity (must be > 0) */
	uint32_t sample_size;               /**< Size in bytes per sample (2 for S16, 4 for Float) */
	mxf_audio_format_t format;          /**< Audio sample format */

	/* Lock-free sample buffer indices (in units of samples, not bytes) */
	MXF_ATOMIC(uint32_t) write_idx;     /**< Producer writes here (atomic release) */
	MXF_ATOMIC(uint32_t) read_idx;      /**< Consumer reads here (atomic release) */

	/* Auxiliary lock-free metadata ring for PTS tracking */
	mxf_block_meta_t meta_ring[MXF_MAX_BLOCK_ENTRIES];
	MXF_ATOMIC(uint32_t) meta_write_idx;
	MXF_ATOMIC(uint32_t) meta_read_idx;

	/* Operational stats */
	MXF_ATOMIC(uint64_t) overflow_count;  /**< Number of samples dropped due to full buffer */
	MXF_ATOMIC(uint64_t) underrun_count; /**< Number of times consumer requested data when empty */
} spsc_ring_buffer_t;

/**
 * @brief Container holding independent SPSC ring buffers for Left and Right channels.
 */
typedef struct {
	spsc_ring_buffer_t queue_l;         /**< Left track SPSC queue */
	spsc_ring_buffer_t queue_r;         /**< Right track SPSC queue */
	uint32_t sample_rate;               /**< Audio sample rate (e.g. 48000 Hz) */
	mxf_audio_format_t format;          /**< Audio sample format */
	bool initialized;                   /**< Initialization status flag */
} dual_spsc_ring_buffer_t;

/* ========================================================================= */
/* Single SPSC Ring Buffer API                                               */
/* ========================================================================= */

/**
 * @brief Initialize an SPSC ring buffer with pre-allocated capacity.
 *
 * Must be called from a thread-safe context before any producer/consumer starts.
 *
 * @param q Pointer to the SPSC ring buffer structure.
 * @param fmt Sample format (MXF_AUDIO_FMT_S16 or MXF_AUDIO_FMT_FLOAT).
 * @param capacity_samples Total sample slots to allocate (e.g. 4800 for 100ms at 48kHz).
 * @return true on success, false if allocation fails or invalid arguments.
 */
bool spsc_ring_buffer_init(spsc_ring_buffer_t *q, mxf_audio_format_t fmt, uint32_t capacity_samples);

/**
 * @brief Destroy an SPSC ring buffer and free internal pre-allocated buffers.
 *
 * Must be called only after both producer and consumer threads have terminated.
 *
 * @param q Pointer to the SPSC ring buffer structure.
 */
void spsc_ring_buffer_destroy(spsc_ring_buffer_t *q);

/**
 * @brief Reset read and write pointers and stats to empty state.
 *
 * Must be called when playback stops or flushes.
 */
void spsc_ring_buffer_reset(spsc_ring_buffer_t *q);

/**
 * @brief Write a contiguous block of samples with presentation timestamp (Producer only).
 *
 * Thread-safe for single producer. Non-blocking.
 * If available space is less than count, the write fails atomically without partial corruption,
 * and overflow_count is incremented by count.
 *
 * @param q Pointer to the SPSC ring buffer structure.
 * @param samples Pointer to input sample array.
 * @param count Number of samples in the block.
 * @param pts Presentation timestamp for the first sample in nanoseconds.
 * @return true if entire block was written, false if buffer full (atomic reject).
 */
bool spsc_ring_buffer_write(spsc_ring_buffer_t *q, const void *samples, uint32_t count, int64_t pts);

/**
 * @brief Write a contiguous block of samples with PTS and epoch metadata (Producer only).
 */
bool spsc_ring_buffer_write_epoch(spsc_ring_buffer_t *q, const void *samples, uint32_t count, int64_t pts, uint32_t epoch);

/**
 * @brief Peek front block metadata without advancing indices (Consumer only).
 */
bool spsc_ring_buffer_peek_meta(const spsc_ring_buffer_t *q, int64_t *out_pts, uint32_t *out_count, uint32_t *out_epoch);

/**
 * @brief Discard all queued blocks with epoch older than min_valid_epoch (Consumer only).
 */
uint32_t spsc_ring_buffer_discard_stale_epochs(spsc_ring_buffer_t *q, uint32_t min_valid_epoch);

/**
 * @brief Read samples and advance the read index (Consumer only).
 *
 * Thread-safe for single consumer. Non-blocking.
 *
 * @param q Pointer to the SPSC ring buffer structure.
 * @param out_samples Destination buffer (or NULL if only discarding).
 * @param count Number of samples to read.
 * @param out_pts Optional pointer to receive the calculated PTS of the first read sample.
 * @return Number of samples actually read (may be less than count if underrun).
 */
uint32_t spsc_ring_buffer_read(spsc_ring_buffer_t *q, void *out_samples, uint32_t count, int64_t *out_pts);

/**
 * @brief Peek samples without advancing the read index (Consumer only).
 *
 * @param q Pointer to the SPSC ring buffer structure.
 * @param out_samples Destination buffer.
 * @param count Number of samples to peek.
 * @param out_pts Optional pointer to receive PTS of the first sample.
 * @return Number of samples actually peeked.
 */
uint32_t spsc_ring_buffer_peek(const spsc_ring_buffer_t *q, void *out_samples, uint32_t count, int64_t *out_pts);

/**
 * @brief Query the PTS of the front sample currently in the queue without reading data.
 *
 * @param q Pointer to the SPSC ring buffer structure.
 * @param out_pts Pointer to receive the PTS.
 * @return true if queue has at least one sample and PTS is valid, false if queue is empty.
 */
bool spsc_ring_buffer_get_front_pts(const spsc_ring_buffer_t *q, int64_t *out_pts);

/**
 * @brief Discard a given number of samples from the head of the buffer (Consumer only).
 *
 * @param q Pointer to the SPSC ring buffer structure.
 * @param count Number of samples to discard.
 * @return Number of samples actually discarded.
 */
uint32_t spsc_ring_buffer_discard(spsc_ring_buffer_t *q, uint32_t count);

/**
 * @brief Number of samples currently available to read (Consumer only).
 */
uint32_t spsc_ring_buffer_available_read(const spsc_ring_buffer_t *q);

/**
 * @brief Number of sample slots currently free to write (Producer only).
 */
uint32_t spsc_ring_buffer_available_write(const spsc_ring_buffer_t *q);

/**
 * @brief Query overflow and underrun statistics.
 */
void spsc_ring_buffer_get_stats(const spsc_ring_buffer_t *q, uint64_t *out_overflows, uint64_t *out_underruns);

/* ========================================================================= */
/* Dual SPSC Container API                                                   */
/* ========================================================================= */

/**
 * @brief Initialize a Dual SPSC container for Left and Right channels.
 */
bool dual_spsc_init(dual_spsc_ring_buffer_t *dual, mxf_audio_format_t fmt, uint32_t sample_rate, uint32_t capacity_samples);

/**
 * @brief Destroy a Dual SPSC container.
 */
void dual_spsc_destroy(dual_spsc_ring_buffer_t *dual);

/**
 * @brief Reset both Left and Right queues to empty.
 */
void dual_spsc_reset(dual_spsc_ring_buffer_t *dual);

#ifdef __cplusplus
}
#endif

#endif /* MXF_AUDIO_PAIRING_H */
