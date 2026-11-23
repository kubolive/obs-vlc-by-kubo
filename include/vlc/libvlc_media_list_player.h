#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vlc/libvlc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libvlc_media_list_player_t libvlc_media_list_player_t;

typedef enum libvlc_playback_mode_t {
    libvlc_playback_mode_default,
    libvlc_playback_mode_loop,
    libvlc_playback_mode_repeat
} libvlc_playback_mode_t;

#ifdef __cplusplus
}
#endif
