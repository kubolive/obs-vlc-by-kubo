#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vlc/libvlc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libvlc_media_player_t libvlc_media_player_t;

typedef enum libvlc_state_t {
    libvlc_NothingSpecial = 0,
    libvlc_Opening,
    libvlc_Buffering,
    libvlc_Playing,
    libvlc_Paused,
    libvlc_Stopped,
    libvlc_Ended,
    libvlc_Error
} libvlc_state_t;

typedef void *(*libvlc_video_lock_cb)(void *opaque, void **planes);
typedef void (*libvlc_video_unlock_cb)(void *opaque, void *picture, void *const *planes);
typedef void (*libvlc_video_display_cb)(void *opaque, void *picture);
typedef unsigned (*libvlc_video_format_cb)(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches, unsigned *lines);
typedef void (*libvlc_video_cleanup_cb)(void *opaque);

typedef void (*libvlc_audio_play_cb)(void *data, const void *samples, unsigned count, int64_t pts);
typedef void (*libvlc_audio_pause_cb)(void *data, int64_t pts);
typedef void (*libvlc_audio_resume_cb)(void *data, int64_t pts);
typedef void (*libvlc_audio_flush_cb)(void *data, int64_t pts);
typedef void (*libvlc_audio_drain_cb)(void *data);
typedef int (*libvlc_audio_setup_cb)(void **data, char *format, unsigned *rate, unsigned *channels);
typedef void (*libvlc_audio_cleanup_cb)(void *data);

#ifdef __cplusplus
}
#endif
