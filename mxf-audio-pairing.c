#include "mxf-audio-pairing.h"
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L)
#include <windows.h>
#define atomic_load_explicit(ptr, order) (*(ptr))
#define atomic_store_explicit(ptr, val, order) do { *(ptr) = (val); MemoryBarrier(); } while (0)
#define atomic_fetch_add_explicit(ptr, val, order) InterlockedAdd64((volatile LONG64 *)(ptr), (LONG64)(val))
#else
#include <stdatomic.h>
#endif

bool spsc_ring_buffer_init(spsc_ring_buffer_t *q, mxf_audio_format_t fmt, uint32_t capacity_samples)
{
	if (!q || capacity_samples == 0)
		return false;

	memset(q, 0, sizeof(*q));

	q->format = fmt;
	q->capacity_samples = capacity_samples;
	q->sample_size = (fmt == MXF_AUDIO_FMT_FLOAT) ? sizeof(float) : sizeof(int16_t);

	/* Pre-allocate buffer for maximum capacity */
	q->buffer = (uint8_t *)malloc((size_t)q->capacity_samples * q->sample_size);
	if (!q->buffer)
		return false;

	atomic_store_explicit(&q->write_idx, 0, memory_order_relaxed);
	atomic_store_explicit(&q->read_idx, 0, memory_order_relaxed);
	atomic_store_explicit(&q->meta_write_idx, 0, memory_order_relaxed);
	atomic_store_explicit(&q->meta_read_idx, 0, memory_order_relaxed);
	atomic_store_explicit(&q->overflow_count, 0, memory_order_relaxed);
	atomic_store_explicit(&q->underrun_count, 0, memory_order_relaxed);

	return true;
}

void spsc_ring_buffer_destroy(spsc_ring_buffer_t *q)
{
	if (!q)
		return;

	if (q->buffer) {
		free(q->buffer);
		q->buffer = NULL;
	}

	q->capacity_samples = 0;
	q->sample_size = 0;
}

void spsc_ring_buffer_reset(spsc_ring_buffer_t *q)
{
	if (!q)
		return;

	atomic_store_explicit(&q->write_idx, 0, memory_order_release);
	atomic_store_explicit(&q->read_idx, 0, memory_order_release);
	atomic_store_explicit(&q->meta_write_idx, 0, memory_order_release);
	atomic_store_explicit(&q->meta_read_idx, 0, memory_order_release);
}

uint32_t spsc_ring_buffer_available_read(const spsc_ring_buffer_t *q)
{
	if (!q)
		return 0;

	uint32_t w = atomic_load_explicit(&q->write_idx, memory_order_acquire);
	uint32_t r = atomic_load_explicit(&q->read_idx, memory_order_relaxed);
	return w - r;
}

uint32_t spsc_ring_buffer_available_write(const spsc_ring_buffer_t *q)
{
	if (!q)
		return 0;

	uint32_t w = atomic_load_explicit(&q->write_idx, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&q->read_idx, memory_order_acquire);
	uint32_t used = w - r;
	return (used < q->capacity_samples) ? (q->capacity_samples - used) : 0;
}

bool spsc_ring_buffer_write(spsc_ring_buffer_t *q, const void *samples, uint32_t count, int64_t pts)
{
	return spsc_ring_buffer_write_epoch(q, samples, count, pts, 0);
}

bool spsc_ring_buffer_write_epoch(spsc_ring_buffer_t *q, const void *samples, uint32_t count, int64_t pts, uint32_t epoch)
{
	if (!q || !samples || count == 0)
		return false;

	uint32_t w = atomic_load_explicit(&q->write_idx, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&q->read_idx, memory_order_acquire);
	uint32_t used = w - r;
	uint32_t free_slots = (used < q->capacity_samples) ? (q->capacity_samples - used) : 0;

	/* Atomic reject if not enough space for the full block - NO partial writes */
	if (count > free_slots) {
		atomic_fetch_add_explicit(&q->overflow_count, count, memory_order_relaxed);
		return false;
	}

	/* Copy sample data into circular buffer */
	uint32_t cap = q->capacity_samples;
	uint32_t offset = w % cap;
	uint32_t first_chunk = cap - offset;
	if (first_chunk > count)
		first_chunk = count;
	uint32_t second_chunk = count - first_chunk;

	memcpy(q->buffer + (size_t)offset * q->sample_size, samples, (size_t)first_chunk * q->sample_size);
	if (second_chunk > 0) {
		const uint8_t *src = (const uint8_t *)samples + (size_t)first_chunk * q->sample_size;
		memcpy(q->buffer, src, (size_t)second_chunk * q->sample_size);
	}

	/* Record block metadata in auxiliary meta ring */
	uint32_t mw = atomic_load_explicit(&q->meta_write_idx, memory_order_relaxed);
	uint32_t mr = atomic_load_explicit(&q->meta_read_idx, memory_order_acquire);
	if ((mw - mr) < MXF_MAX_BLOCK_ENTRIES) {
		uint32_t meta_pos = mw % MXF_MAX_BLOCK_ENTRIES;
		q->meta_ring[meta_pos].pts = pts;
		q->meta_ring[meta_pos].sample_count = count;
		q->meta_ring[meta_pos].epoch = epoch;
		atomic_store_explicit(&q->meta_write_idx, mw + 1, memory_order_release);
	}

	/* Publish written samples to consumer with release semantics */
	atomic_store_explicit(&q->write_idx, w + count, memory_order_release);
	return true;
}

bool spsc_ring_buffer_peek_meta(const spsc_ring_buffer_t *q, int64_t *out_pts, uint32_t *out_count, uint32_t *out_epoch)
{
	if (!q)
		return false;

	uint32_t mw = atomic_load_explicit(&q->meta_write_idx, memory_order_acquire);
	uint32_t mr = atomic_load_explicit(&q->meta_read_idx, memory_order_relaxed);
	if (mw == mr)
		return false;

	uint32_t pos = mr % MXF_MAX_BLOCK_ENTRIES;
	if (out_pts)
		*out_pts = q->meta_ring[pos].pts;
	if (out_count)
		*out_count = q->meta_ring[pos].sample_count;
	if (out_epoch)
		*out_epoch = q->meta_ring[pos].epoch;
	return true;
}

uint32_t spsc_ring_buffer_discard_stale_epochs(spsc_ring_buffer_t *q, uint32_t min_valid_epoch)
{
	if (!q)
		return 0;

	uint32_t total_dropped = 0;
	while (true) {
		uint32_t mw = atomic_load_explicit(&q->meta_write_idx, memory_order_acquire);
		uint32_t mr = atomic_load_explicit(&q->meta_read_idx, memory_order_relaxed);
		if (mw == mr)
			break;

		uint32_t pos = mr % MXF_MAX_BLOCK_ENTRIES;
		if (q->meta_ring[pos].epoch >= min_valid_epoch)
			break; // Up to date!

		// Stale block detected! Discard this block's samples
		uint32_t cnt = q->meta_ring[pos].sample_count;
		spsc_ring_buffer_discard(q, cnt);
		total_dropped += cnt;
	}
	return total_dropped;
}

static uint32_t spsc_read_internal(const spsc_ring_buffer_t *q, void *out_samples, uint32_t count, int64_t *out_pts, bool advance)
{
	if (!q || count == 0)
		return 0;

	spsc_ring_buffer_t *non_const_q = (spsc_ring_buffer_t *)q;
	uint32_t w = atomic_load_explicit(&q->write_idx, memory_order_acquire);
	uint32_t r = atomic_load_explicit(&q->read_idx, memory_order_relaxed);
	uint32_t available = w - r;

	if (available == 0) {
		if (advance) {
#if defined(_MSC_VER) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L)
			atomic_fetch_add_explicit(&non_const_q->underrun_count, 1, memory_order_relaxed);
#else
			atomic_fetch_add_explicit(&non_const_q->underrun_count, 1, memory_order_relaxed);
#endif
		}
		return 0;
	}

	uint32_t to_read = (count < available) ? count : available;

	/* Determine PTS of the first sample being read */
	if (out_pts) {
		uint32_t mw = atomic_load_explicit(&q->meta_write_idx, memory_order_acquire);
		uint32_t mr = atomic_load_explicit(&q->meta_read_idx, memory_order_relaxed);
		if (mw != mr) {
			*out_pts = q->meta_ring[mr % MXF_MAX_BLOCK_ENTRIES].pts;
		} else {
			*out_pts = 0;
		}
	}

	/* Copy sample data from circular buffer if out_samples provided */
	if (out_samples) {
		uint32_t cap = q->capacity_samples;
		uint32_t offset = r % cap;
		uint32_t first_chunk = cap - offset;
		if (first_chunk > to_read)
			first_chunk = to_read;
		uint32_t second_chunk = to_read - first_chunk;

		memcpy(out_samples, q->buffer + (size_t)offset * q->sample_size, (size_t)first_chunk * q->sample_size);
		if (second_chunk > 0) {
			uint8_t *dst = (uint8_t *)out_samples + (size_t)first_chunk * q->sample_size;
			memcpy(dst, q->buffer, (size_t)second_chunk * q->sample_size);
		}
	}

	if (advance) {
		/* Update metadata ring: advance blocks that have been fully read */
		uint32_t mw = atomic_load_explicit(&q->meta_write_idx, memory_order_acquire);
		uint32_t mr = atomic_load_explicit(&q->meta_read_idx, memory_order_relaxed);
		uint32_t remaining_to_advance = to_read;

		while (mr != mw && remaining_to_advance > 0) {
			uint32_t meta_pos = mr % MXF_MAX_BLOCK_ENTRIES;
			if (q->meta_ring[meta_pos].sample_count <= remaining_to_advance) {
				remaining_to_advance -= q->meta_ring[meta_pos].sample_count;
				mr++;
			} else {
				non_const_q->meta_ring[meta_pos].sample_count -= remaining_to_advance;
				remaining_to_advance = 0;
			}
		}
		atomic_store_explicit(&non_const_q->meta_read_idx, mr, memory_order_release);

		/* Advance sample read pointer with release semantics */
		atomic_store_explicit(&non_const_q->read_idx, r + to_read, memory_order_release);
	}

	return to_read;
}

uint32_t spsc_ring_buffer_read(spsc_ring_buffer_t *q, void *out_samples, uint32_t count, int64_t *out_pts)
{
	return spsc_read_internal(q, out_samples, count, out_pts, true);
}

uint32_t spsc_ring_buffer_peek(const spsc_ring_buffer_t *q, void *out_samples, uint32_t count, int64_t *out_pts)
{
	return spsc_read_internal(q, out_samples, count, out_pts, false);
}

bool spsc_ring_buffer_get_front_pts(const spsc_ring_buffer_t *q, int64_t *out_pts)
{
	if (!q || !out_pts)
		return false;

	uint32_t w = atomic_load_explicit(&q->write_idx, memory_order_acquire);
	uint32_t r = atomic_load_explicit(&q->read_idx, memory_order_relaxed);
	if (w == r)
		return false;

	uint32_t mw = atomic_load_explicit(&q->meta_write_idx, memory_order_acquire);
	uint32_t mr = atomic_load_explicit(&q->meta_read_idx, memory_order_relaxed);
	if (mw != mr) {
		*out_pts = q->meta_ring[mr % MXF_MAX_BLOCK_ENTRIES].pts;
		return true;
	}
	*out_pts = 0;
	return true;
}

uint32_t spsc_ring_buffer_discard(spsc_ring_buffer_t *q, uint32_t count)
{
	return spsc_read_internal(q, NULL, count, NULL, true);
}

void spsc_ring_buffer_get_stats(const spsc_ring_buffer_t *q, uint64_t *out_overflows, uint64_t *out_underruns)
{
	if (!q)
		return;

	if (out_overflows)
		*out_overflows = atomic_load_explicit(&q->overflow_count, memory_order_relaxed);
	if (out_underruns)
		*out_underruns = atomic_load_explicit(&q->underrun_count, memory_order_relaxed);
}

/* ========================================================================= */
/* Dual SPSC Container Implementation                                        */
/* ========================================================================= */

bool dual_spsc_init(dual_spsc_ring_buffer_t *dual, mxf_audio_format_t fmt, uint32_t sample_rate, uint32_t capacity_samples)
{
	if (!dual)
		return false;

	memset(dual, 0, sizeof(*dual));
	dual->format = fmt;
	dual->sample_rate = sample_rate;

	if (!spsc_ring_buffer_init(&dual->queue_l, fmt, capacity_samples))
		return false;

	if (!spsc_ring_buffer_init(&dual->queue_r, fmt, capacity_samples)) {
		spsc_ring_buffer_destroy(&dual->queue_l);
		return false;
	}

	dual->initialized = true;
	return true;
}

void dual_spsc_destroy(dual_spsc_ring_buffer_t *dual)
{
	if (!dual || !dual->initialized)
		return;

	spsc_ring_buffer_destroy(&dual->queue_l);
	spsc_ring_buffer_destroy(&dual->queue_r);
	dual->initialized = false;
}

void dual_spsc_reset(dual_spsc_ring_buffer_t *dual)
{
	if (!dual || !dual->initialized)
		return;

	spsc_ring_buffer_reset(&dual->queue_l);
	spsc_ring_buffer_reset(&dual->queue_r);
}
