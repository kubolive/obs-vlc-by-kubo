#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libvlc_media_t libvlc_media_t;

typedef enum libvlc_meta_t {
    libvlc_meta_Title,
    libvlc_meta_Artist,
    libvlc_meta_Genre,
    libvlc_meta_Copyright,
    libvlc_meta_Album,
    libvlc_meta_TrackNumber,
    libvlc_meta_Description,
    libvlc_meta_Rating,
    libvlc_meta_Date,
    libvlc_meta_Setting,
    libvlc_meta_URL,
    libvlc_meta_Language,
    libvlc_meta_NowPlaying,
    libvlc_meta_Publisher,
    libvlc_meta_EncodedBy,
    libvlc_meta_ArtworkURL,
    libvlc_meta_TrackID,
    libvlc_meta_TrackTotal,
    libvlc_meta_Director,
    libvlc_meta_Season,
    libvlc_meta_Episode,
    libvlc_meta_ShowName,
    libvlc_meta_Actors,
    libvlc_meta_AlbumArtist,
    libvlc_meta_DiscNumber,
    libvlc_meta_DiscTotal
} libvlc_meta_t;

typedef enum libvlc_track_type_t {
    libvlc_track_unknown = -1,
    libvlc_track_audio = 0,
    libvlc_track_video = 1,
    libvlc_track_text = 2
} libvlc_track_type_t;

typedef enum libvlc_video_orient_t {
    libvlc_video_orient_top_left,
    libvlc_video_orient_left_top,
    libvlc_video_orient_left_bottom,
    libvlc_video_orient_right_top,
    libvlc_video_orient_right_bottom,
    libvlc_video_orient_bottom_left,
    libvlc_video_orient_bottom_right,
    libvlc_video_orient_top_right
} libvlc_video_orient_t;

typedef enum libvlc_video_projection_t {
    libvlc_video_projection_rectangular = 0,
    libvlc_video_projection_equirectangular = 1,
    libvlc_video_projection_cubemap_layout_standard = 256
} libvlc_video_projection_t;

typedef enum libvlc_video_multiview_t {
    libvlc_video_multiview_2d = 0,
    libvlc_video_multiview_stereoscopic_top_bottom = 1,
    libvlc_video_multiview_stereoscopic_left_right = 2
} libvlc_video_multiview_t;

typedef struct libvlc_audio_track_t {
    unsigned i_channels;
    unsigned i_rate;
} libvlc_audio_track_t;

typedef struct libvlc_video_track_t {
    unsigned i_height;
    unsigned i_width;
    unsigned i_sar_num;
    unsigned i_sar_den;
    unsigned i_frame_rate_num;
    unsigned i_frame_rate_den;
    libvlc_video_orient_t i_orientation;
    libvlc_video_projection_t i_projection;
    libvlc_video_multiview_t i_multiview;
} libvlc_video_track_t;

typedef struct libvlc_subtitle_track_t {
    char *psz_encoding;
} libvlc_subtitle_track_t;

typedef struct libvlc_media_track_t {
    uint32_t i_codec;
    uint32_t i_original_fourcc;
    int i_id;
    libvlc_track_type_t i_type;
    int i_profile;
    int i_level;
    union {
        libvlc_audio_track_t *audio;
        libvlc_video_track_t *video;
        libvlc_subtitle_track_t *subtitle;
    };
    unsigned int i_bitrate;
    char *psz_language;
    char *psz_description;
} libvlc_media_track_t;

#ifdef __cplusplus
}
#endif
