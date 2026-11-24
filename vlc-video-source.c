#include "vlc-video-plugin.h"
#include "mxf-audio-pairing.h"
#include "mxf-timeline-aligner.h"
#include <media-io/video-frame.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define do_log(level, format, ...) \
	blog(level, "[vlc_source: '%s'] " format, obs_source_get_name(ss->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)

/* clang-format off */

#define S_PLAYLIST                     "playlist"
#define S_LOOP                         "loop"
#define S_SHUFFLE                      "shuffle"
#define S_BEHAVIOR                     "playback_behavior"
#define S_BEHAVIOR_STOP_RESTART        "stop_restart"
#define S_BEHAVIOR_PAUSE_UNPAUSE       "pause_unpause"
#define S_BEHAVIOR_ALWAYS_PLAY         "always_play"
#define S_NETWORK_CACHING              "network_caching"
#define S_TRACK                        "track"
#define S_STEREO_PAIRING_MODE          "stereo_pairing_mode"
#define S_HW_DECODE                    "hw_decode"
#define S_SUBTITLE_ENABLE              "subtitle_enable"
#define S_SUBTITLE_TRACK               "subtitle"
#define S_SUBTITLE_CUSTOM_ENABLE       "sub_custom_enable"
#define S_SUBTITLE_FONT                "sub_font"
#define S_SUBTITLE_COLOR               "sub_color"
#define S_SUBTITLE_OUTLINE_ENABLE      "sub_outline_enable"
#define S_SUBTITLE_OUTLINE             "sub_outline"
#define S_SUBTITLE_OUTLINE_COLOR       "sub_outline_color"
#define S_SUBTITLE_SHADOW_ENABLE       "sub_shadow_enable"
#define S_SUBTITLE_SHADOW              "sub_shadow"
#define S_SUBTITLE_SHADOW_COLOR        "sub_shadow_color"
#define S_SUBTITLE_SHADOW_OPACITY      "sub_shadow_opacity"
#define S_SUBTITLE_BG_ENABLE           "sub_bg_enable"
#define S_SUBTITLE_BG_COLOR            "sub_bg_color"
#define S_SUBTITLE_BG_OPACITY          "sub_bg_opacity"
#define S_SUBTITLE_MARGIN              "sub_margin"
#define S_SUBTITLE_OVERRIDE_ASS        "sub_override_ass"

#define T_(text) obs_module_text(text)
#define T_PLAYLIST                     T_("Playlist")
#define T_LOOP                         T_("LoopPlaylist")
#define T_SHUFFLE                      T_("Shuffle")
#define T_BEHAVIOR                     T_("PlaybackBehavior")
#define T_BEHAVIOR_STOP_RESTART        T_("PlaybackBehavior.StopRestart")
#define T_BEHAVIOR_PAUSE_UNPAUSE       T_("PlaybackBehavior.PauseUnpause")
#define T_BEHAVIOR_ALWAYS_PLAY         T_("PlaybackBehavior.AlwaysPlay")
#define T_NETWORK_CACHING              T_("NetworkCaching")
#define T_TRACK                        T_("AudioTrack")
#define T_STEREO_PAIRING_MODE          T_("AudioTrackPairing")
#define T_STEREO_PAIRING_AUTO          T_("AudioTrackPairing.Auto")
#define T_STEREO_PAIRING_PASSTHROUGH   T_("AudioTrackPairing.PassThrough")
#define T_STEREO_PAIRING_1_2           T_("AudioTrackPairing.Track1_2")
#define T_STEREO_PAIRING_1_3           T_("AudioTrackPairing.Track1_3")
#define T_STEREO_PAIRING_3_4           T_("AudioTrackPairing.Track3_4")

#define T_HW_DECODE                    T_("HardwareDecode")
#define T_SUBTITLE_ENABLE              T_("SubtitleEnable")
#define T_SUBTITLE_TRACK               T_("SubtitleTrack")
#define T_SUBTITLE_CUSTOM_ENABLE       T_("SubtitleCustomEnable")
#define T_SUBTITLE_FONT                T_("SubtitleFont")
#define T_SUBTITLE_COLOR               T_("SubtitleColor")
#define T_SUBTITLE_OUTLINE_ENABLE      T_("SubtitleOutlineEnable")
#define T_SUBTITLE_OUTLINE             T_("SubtitleOutline")
#define T_SUBTITLE_OUTLINE_THIN        T_("SubtitleOutline.Thin")
#define T_SUBTITLE_OUTLINE_NORMAL      T_("SubtitleOutline.Normal")
#define T_SUBTITLE_OUTLINE_THICK       T_("SubtitleOutline.Thick")
#define T_SUBTITLE_OUTLINE_COLOR       T_("SubtitleOutlineColor")
#define T_SUBTITLE_SHADOW_ENABLE       T_("SubtitleShadowEnable")
#define T_SUBTITLE_SHADOW              T_("SubtitleShadow")
#define T_SUBTITLE_SHADOW_COLOR        T_("SubtitleShadowColor")
#define T_SUBTITLE_SHADOW_OPACITY      T_("SubtitleShadowOpacity")
#define T_SUBTITLE_BG_ENABLE           T_("SubtitleBgEnable")
#define T_SUBTITLE_BG_COLOR            T_("SubtitleBgColor")
#define T_SUBTITLE_BG_OPACITY          T_("SubtitleBgOpacity")
#define T_SUBTITLE_MARGIN              T_("SubtitleMargin")
#define T_SUBTITLE_OVERRIDE_ASS        T_("SubtitleOverrideAss")

/* clang-format on */

/* ------------------------------------------------------------------------- */

struct media_file_data {
	uint64_t id;
	char *path;
	libvlc_media_t *media;
};

typedef DARRAY(struct media_file_data) media_file_array_t;

enum behavior {
	BEHAVIOR_STOP_RESTART,
	BEHAVIOR_PAUSE_UNPAUSE,
	BEHAVIOR_ALWAYS_PLAY,
};

enum mxf_pairing_mode {
	PAIR_MODE_AUTO = 0,
	PAIR_MODE_PASSTHROUGH = 1,
	PAIR_MODE_MANUAL_1_2 = 2,
	PAIR_MODE_MANUAL_1_3 = 3,
	PAIR_MODE_MANUAL_3_4 = 4,
};

#define MAX_STREAM_OUTPUTS 4
#define PTS_WATERMARK_DELAY_MS 5
#define PTS_WATERMARK_DELAY_NS (PTS_WATERMARK_DELAY_MS * 1000000LL)
#define MXF_STREAM_MAGIC 0x4D584653ULL /* "MXFS" */

typedef enum {
	STREAM_ROLE_UNKNOWN = 0,
	STREAM_ROLE_LEFT,
	STREAM_ROLE_RIGHT,
} mxf_stream_role_t;

struct vlc_source;

struct mxf_stream_ctx {
	uint64_t magic;
	int slot_id;
	_Atomic(bool) is_active;
	mxf_stream_role_t role;
	int64_t first_pts;
	uint32_t generation;
	struct vlc_source *source;
	_Atomic(uint32_t) stream_epoch;
	uint32_t channels;
	uint32_t sample_rate;
	_Atomic(uint64_t) total_energy_accum;
	_Atomic(uint64_t) total_samples_received;
};

struct mxf_pairing_session {
	uint32_t generation;
	_Atomic(int) active_streams;
	struct mxf_stream_ctx streams[MAX_STREAM_OUTPUTS];
	spsc_ring_buffer_t stream_queues[MAX_STREAM_OUTPUTS];
	mxf_timeline_aligner_t aligner;
	_Atomic(int) left_slot;
	_Atomic(int) right_slot;
	_Atomic(bool) routing_locked;
	_Atomic(bool) direct_stereo;
	bool initialized;

	/* Dedicated Single Audio Consumer Worker (Phase E4A) */
	pthread_t worker_thread;
	os_event_t *worker_event;
	_Atomic(bool) worker_running;
	_Atomic(bool) worker_created;

	_Atomic(uint64_t) diagnostic_frames_processed;
};

struct vlc_source {
	obs_source_t *source;

	libvlc_media_player_t *media_player;
	libvlc_media_list_player_t *media_list_player;
	libvlc_media_list_t *media_list;

	int current_audio_track;
	int current_subtitle_track;
	bool current_subtitle_enable;
	int network_caching;

	struct obs_source_frame frame;
	struct obs_source_audio audio;
	size_t audio_capacity;

	pthread_mutex_t mutex;
	media_file_array_t files;
	enum behavior behavior;
	enum mxf_pairing_mode pairing_mode;
	struct mxf_pairing_session pairing_session;
	bool loop;
	bool shuffle;
	bool hw_decode;
	uint32_t video_frames_received;

	struct {
		bool custom_enable;
		char *font_face;
		int font_size;
		bool font_bold;
		bool font_italic;
		uint32_t text_color;
		bool outline_enable;
		int outline_thickness;
		uint32_t outline_color;
		bool shadow_enable;
		int shadow_distance;
		uint32_t shadow_color;
		int shadow_opacity;
		bool bg_enable;
		uint32_t bg_color;
		int bg_opacity;
		int margin;
		bool override_ass;
	} sub_settings;

	obs_hotkey_id play_pause_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id stop_hotkey;
	obs_hotkey_id playlist_next_hotkey;
	obs_hotkey_id playlist_prev_hotkey;
};

static _Atomic(uint64_t) s_item_id_counter = 1;

static uint64_t generate_stable_item_id(void)
{
	return atomic_fetch_add(&s_item_id_counter, 1);
}

static uint64_t get_or_create_item_id(obs_data_t *item)
{
	long long id = obs_data_get_int(item, "id");
	if (id <= 0) {
		id = (long long)generate_stable_item_id();
		obs_data_set_int(item, "id", id);
	}
	return (uint64_t)id;
}

static libvlc_media_t *get_media(media_file_array_t *files, const char *path)
{
	libvlc_media_t *media = NULL;

	for (size_t i = 0; i < files->num; i++) {
		const char *cur_path = files->array[i].path;

		if (strcmp(path, cur_path) == 0) {
			media = files->array[i].media;
			libvlc_media_retain_(media);
			break;
		}
	}

	return media;
}

static struct media_file_data *get_current_playing_file(struct vlc_source *c)
{
	if (!c || !c->media_player || c->files.num == 0)
		return NULL;

	libvlc_state_t state = libvlc_media_player_get_state_(c->media_player);
	if (state != libvlc_Playing && state != libvlc_Paused &&
	    state != libvlc_Buffering && state != libvlc_Opening)
		return NULL;

	libvlc_media_t *cur_media = libvlc_media_player_get_media_(c->media_player);
	if (!cur_media)
		return NULL;

	struct media_file_data *found = NULL;
	for (size_t i = 0; i < c->files.num; i++) {
		if (c->files.array[i].media == cur_media) {
			found = &c->files.array[i];
			break;
		}
	}

	if (!found && libvlc_media_get_mrl_) {
		char *cur_mrl = libvlc_media_get_mrl_(cur_media);
		if (cur_mrl) {
			for (size_t i = 0; i < c->files.num; i++) {
				char *mrl = libvlc_media_get_mrl_(c->files.array[i].media);
				if (mrl) {
					if (strcmp(cur_mrl, mrl) == 0) {
						found = &c->files.array[i];
						free(mrl);
						break;
					}
					free(mrl);
				}
			}
			free(cur_mrl);
		}
	}

	libvlc_media_release_(cur_media);
	return found;
}

static void apply_audio_track_runtime(struct vlc_source *c, int track_index)
{
	if (!c || !c->media_player || !libvlc_audio_set_track_)
		return;

	if (c->pairing_mode != PAIR_MODE_PASSTHROUGH && c->pairing_mode != PAIR_MODE_AUTO)
		return;

	if (!libvlc_audio_get_track_description_ || !libvlc_track_description_list_release_) {
		libvlc_audio_set_track_(c->media_player, track_index);
		return;
	}

	libvlc_track_description_t *desc = libvlc_audio_get_track_description_(c->media_player);
	if (!desc) {
		libvlc_audio_set_track_(c->media_player, track_index);
		return;
	}

	libvlc_track_description_t *cur = desc;
	int count = 0;
	int target_id = -1;

	while (cur) {
		if (cur->i_id >= 0) {
			count++;
			if (count == track_index) {
				target_id = cur->i_id;
				break;
			}
		}
		cur = cur->p_next;
	}

	if (target_id >= 0) {
		libvlc_audio_set_track_(c->media_player, target_id);
		blog(LOG_INFO, "[vlc-video] Switched audio track to index %d (VLC ID %d)", track_index, target_id);
	}

	libvlc_track_description_list_release_(desc);
}

static void apply_subtitle_track_runtime(struct vlc_source *c, bool enable, int sub_index)
{
	if (!c || !c->media_player || !libvlc_video_set_spu_)
		return;

	if (!enable) {
		libvlc_video_set_spu_(c->media_player, -1);
		blog(LOG_INFO, "[vlc-video] Subtitles disabled");
		return;
	}

	if (!libvlc_video_get_spu_description_ || !libvlc_track_description_list_release_) {
		libvlc_video_set_spu_(c->media_player, sub_index);
		return;
	}

	libvlc_track_description_t *desc = libvlc_video_get_spu_description_(c->media_player);
	if (!desc) {
		libvlc_video_set_spu_(c->media_player, sub_index);
		return;
	}

	libvlc_track_description_t *cur = desc;
	int count = 0;
	int target_id = -1;

	while (cur) {
		if (cur->i_id >= 0) {
			count++;
			if (count == sub_index) {
				target_id = cur->i_id;
				break;
			}
		}
		cur = cur->p_next;
	}

	if (target_id >= 0) {
		libvlc_video_set_spu_(c->media_player, target_id);
		blog(LOG_INFO, "[vlc-video] Switched subtitle track to index %d (VLC ID %d)", sub_index, target_id);
	}

	libvlc_track_description_list_release_(desc);
}

static inline uint32_t obs_to_vlc_color(uint32_t c)
{
	uint32_t r = c & 0xFF;
	uint32_t g = (c >> 8) & 0xFF;
	uint32_t b = (c >> 16) & 0xFF;
	return (r << 16) | (g << 8) | b;
}

static void apply_custom_subtitle_settings(struct vlc_source *c)
{
	if (!config_PutPsz_ || !config_PutInt_ || !config_PutFloat_)
		return;

	if (c->sub_settings.custom_enable) {
		if (c->sub_settings.font_face && *c->sub_settings.font_face) {
			config_PutPsz_(NULL, "freetype-font", c->sub_settings.font_face);
		}
		if (c->sub_settings.font_size > 0) {
			config_PutInt_(NULL, "freetype-fontsize", c->sub_settings.font_size);
		}
		config_PutInt_(NULL, "freetype-bold", c->sub_settings.font_bold ? 1 : 0);

		config_PutInt_(NULL, "freetype-color", (int64_t)obs_to_vlc_color(c->sub_settings.text_color));

		/* Outline */
		if (c->sub_settings.outline_enable && c->sub_settings.outline_thickness > 0) {
			config_PutInt_(NULL, "freetype-outline-thickness", c->sub_settings.outline_thickness);
			config_PutInt_(NULL, "freetype-outline-color", (int64_t)obs_to_vlc_color(c->sub_settings.outline_color));
		} else {
			config_PutInt_(NULL, "freetype-outline-thickness", 0);
		}

		/* Shadow */
		if (c->sub_settings.shadow_enable) {
			float shadow_dist = 0.60f;
			if (c->sub_settings.shadow_distance <= 4 && c->sub_settings.shadow_distance >= 1) {
				if (c->sub_settings.shadow_distance == 1) shadow_dist = 0.30f;
				else if (c->sub_settings.shadow_distance == 2) shadow_dist = 0.50f;
				else if (c->sub_settings.shadow_distance == 3) shadow_dist = 0.70f;
				else if (c->sub_settings.shadow_distance == 4) shadow_dist = 0.90f;
			} else if (c->sub_settings.shadow_distance > 4) {
				shadow_dist = (float)c->sub_settings.shadow_distance / 100.0f;
				if (shadow_dist > 1.0f) shadow_dist = 1.0f;
				if (shadow_dist < 0.05f) shadow_dist = 0.05f;
			}

			int shadow_op = (c->sub_settings.shadow_opacity * 255) / 100;
			if (shadow_op > 255) shadow_op = 255;
			if (shadow_op < 0) shadow_op = 0;

			config_PutFloat_(NULL, "freetype-shadow-distance", shadow_dist);
			config_PutInt_(NULL, "freetype-shadow-color", (int64_t)obs_to_vlc_color(c->sub_settings.shadow_color));
			config_PutInt_(NULL, "freetype-shadow-opacity", shadow_op);
		} else {
			config_PutFloat_(NULL, "freetype-shadow-distance", 0.0f);
			config_PutInt_(NULL, "freetype-shadow-opacity", 0);
		}

		/* Background box & opacity (0..255) */
		if (c->sub_settings.bg_enable && c->sub_settings.bg_opacity > 0) {
			int opacity_255 = (c->sub_settings.bg_opacity * 255) / 100;
			if (opacity_255 > 255) opacity_255 = 255;
			if (opacity_255 < 0) opacity_255 = 0;
			config_PutInt_(NULL, "freetype-background-color", (int64_t)obs_to_vlc_color(c->sub_settings.bg_color));
			config_PutInt_(NULL, "freetype-background-opacity", opacity_255);
		} else {
			config_PutInt_(NULL, "freetype-background-opacity", 0);
		}

		config_PutInt_(NULL, "sub-margin", c->sub_settings.margin);
	} else {
		/* Reset to standard VLC defaults */
		config_PutPsz_(NULL, "freetype-font", "Arial");
		config_PutInt_(NULL, "freetype-fontsize", 0);
		config_PutInt_(NULL, "freetype-bold", 0);
		config_PutInt_(NULL, "freetype-color", 0x00FFFFFF);
		config_PutInt_(NULL, "freetype-outline-thickness", 1);
		config_PutInt_(NULL, "freetype-outline-color", 0x00000000);
		config_PutFloat_(NULL, "freetype-shadow-distance", 0.06f);
		config_PutInt_(NULL, "freetype-shadow-color", 0x00000000);
		config_PutInt_(NULL, "freetype-shadow-opacity", 128);
		config_PutInt_(NULL, "freetype-background-opacity", 0);
		config_PutInt_(NULL, "sub-margin", 0);
	}
}

static inline libvlc_media_t *create_media_from_file(const char *file)
{
	return (file && strstr(file, "://") != NULL) ? libvlc_media_new_location_(libvlc, file)
						     : libvlc_media_new_path_(libvlc, file);
}

static void free_files(media_file_array_t *files)
{
	for (size_t i = 0; i < files->num; i++) {
		bfree(files->array[i].path);
		libvlc_media_release_(files->array[i].media);
	}

	da_free(*files);
}

#define MAKEFORMAT(ch0, ch1, ch2, ch3)                                                                   \
	((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | ((uint32_t)(uint8_t)(ch2) << 16) | \
	 ((uint32_t)(uint8_t)(ch3) << 24))

static inline bool chroma_is(const char *chroma, const char *val)
{
	return *(uint32_t *)chroma == *(uint32_t *)val;
}

static enum video_format convert_vlc_video_format(char *chroma, bool *full)
{
	*full = false;

#define CHROMA_TEST(val, ret)       \
	if (chroma_is(chroma, val)) \
	return ret
#define CHROMA_CONV(val, new_val, ret)                           \
	do {                                                     \
		if (chroma_is(chroma, val)) {                    \
			*(uint32_t *)chroma = (uint32_t)new_val; \
			return ret;                              \
		}                                                \
	} while (false)
#define CHROMA_CONV_FULL(val, new_val, ret)     \
	do {                                    \
		*full = true;                   \
		CHROMA_CONV(val, new_val, ret); \
	} while (false)

	/* Hardware Decoded Surface formats -> System Memory NV12 via Direct3D11/9 staging download */
	CHROMA_CONV("DX11", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);
	CHROMA_CONV("DXA9", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);
	CHROMA_CONV("d3d1", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);
	CHROMA_CONV("va08", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);

	CHROMA_TEST("RGBA", VIDEO_FORMAT_RGBA);
	CHROMA_TEST("BGRA", VIDEO_FORMAT_BGRA);

	/* 4:2:0 formats */
	CHROMA_TEST("NV12", VIDEO_FORMAT_NV12);
	CHROMA_TEST("I420", VIDEO_FORMAT_I420);
	CHROMA_TEST("IYUV", VIDEO_FORMAT_I420);
	CHROMA_CONV("NV21", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);
	CHROMA_CONV("Y42B", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);
	CHROMA_CONV("YV12", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);
	CHROMA_CONV("yv12", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_NV12);

	CHROMA_CONV_FULL("J420", MAKEFORMAT('J', '4', '2', '0'), VIDEO_FORMAT_I420);

	/* 4:2:2 formats */
	CHROMA_TEST("I422", VIDEO_FORMAT_I422);
	CHROMA_TEST("Y42B", VIDEO_FORMAT_I422);
	CHROMA_TEST("UYVY", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("UYNV", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("UYNY", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("Y422", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("HDYC", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("AVUI", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("uyv1", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("2vuy", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("2Vuy", VIDEO_FORMAT_UYVY);
	CHROMA_TEST("2Vu1", VIDEO_FORMAT_UYVY);

	CHROMA_TEST("YUY2", VIDEO_FORMAT_YUY2);
	CHROMA_TEST("YUYV", VIDEO_FORMAT_YUY2);
	CHROMA_TEST("YUNV", VIDEO_FORMAT_YUY2);
	CHROMA_TEST("V422", VIDEO_FORMAT_YUY2);

	CHROMA_TEST("YVYU", VIDEO_FORMAT_YVYU);

	CHROMA_CONV("v210", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("cyuv", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("CYUV", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("VYUY", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("NV16", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("NV61", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("I410", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("Y42B", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("J422", MAKEFORMAT('U', 'Y', 'V', 'Y'), VIDEO_FORMAT_UYVY);

	/* 4:4:4 formats */
	CHROMA_TEST("I444", VIDEO_FORMAT_I444);
	CHROMA_CONV_FULL("J444", MAKEFORMAT('R', 'G', 'B', 'A'), VIDEO_FORMAT_RGBA);
	CHROMA_CONV("YUVA", MAKEFORMAT('R', 'G', 'B', 'A'), VIDEO_FORMAT_RGBA);

	/* 4:4:0 formats */
	CHROMA_CONV("I440", MAKEFORMAT('I', '4', '4', '4'), VIDEO_FORMAT_I444);
	CHROMA_CONV("J440", MAKEFORMAT('I', '4', '4', '4'), VIDEO_FORMAT_I444);

	/* 4:1:0 formats */
	CHROMA_CONV("YVU9", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("I410", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_UYVY);

	/* 4:1:1 formats */
	CHROMA_CONV("I411", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_UYVY);
	CHROMA_CONV("Y41B", MAKEFORMAT('N', 'V', '1', '2'), VIDEO_FORMAT_UYVY);

	/* greyscale formats */
	CHROMA_TEST("GREY", VIDEO_FORMAT_Y800);
	CHROMA_TEST("Y800", VIDEO_FORMAT_Y800);
	CHROMA_TEST("Y8  ", VIDEO_FORMAT_Y800);
#undef CHROMA_CONV_FULL
#undef CHROMA_CONV
#undef CHROMA_TEST

	*(uint32_t *)chroma = (uint32_t)MAKEFORMAT('B', 'G', 'R', 'A');
	return VIDEO_FORMAT_BGRA;
}

static inline unsigned get_format_lines(enum video_format format, unsigned height, size_t plane)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
		return (plane == 0) ? height : height / 2;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_Y800:
		return height;
	case VIDEO_FORMAT_NONE:
	default:
		break;
	}

	return 0;
}

static enum audio_format convert_vlc_audio_format(char *format)
{
#define AUDIO_TEST(val, ret)        \
	if (chroma_is(format, val)) \
	return ret
#define AUDIO_CONV(val, new_val, ret)                            \
	do {                                                     \
		if (chroma_is(format, val)) {                    \
			*(uint32_t *)format = (uint32_t)new_val; \
			return ret;                              \
		}                                                \
	} while (false)

	AUDIO_TEST("S16N", AUDIO_FORMAT_16BIT);
	AUDIO_TEST("S32N", AUDIO_FORMAT_32BIT);
	AUDIO_TEST("FL32", AUDIO_FORMAT_FLOAT);

	AUDIO_CONV("U16N", MAKEFORMAT('S', '1', '6', 'N'), AUDIO_FORMAT_16BIT);
	AUDIO_CONV("U32N", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
	AUDIO_CONV("S24N", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
	AUDIO_CONV("U24N", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
	AUDIO_CONV("FL64", MAKEFORMAT('F', 'L', '3', '2'), AUDIO_FORMAT_FLOAT);

	AUDIO_CONV("S16I", MAKEFORMAT('S', '1', '6', 'N'), AUDIO_FORMAT_16BIT);
	AUDIO_CONV("U16I", MAKEFORMAT('S', '1', '6', 'N'), AUDIO_FORMAT_16BIT);
	AUDIO_CONV("S24I", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
	AUDIO_CONV("U24I", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
	AUDIO_CONV("S32I", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
	AUDIO_CONV("U32I", MAKEFORMAT('S', '3', '2', 'N'), AUDIO_FORMAT_32BIT);
#undef AUDIO_CONV
#undef AUDIO_TEST

	*(uint32_t *)format = (uint32_t)MAKEFORMAT('F', 'L', '3', '2');
	return AUDIO_FORMAT_FLOAT;
}

/* ------------------------------------------------------------------------- */

static void vlcs_get_metadata(void *data, calldata_t *cd)
{
	struct vlc_source *vlcs = data;
	const char *data_id = calldata_string(cd, "tag_id");

	if (!vlcs || !data_id)
		return;
	libvlc_media_t *media = libvlc_media_player_get_media_(vlcs->media_player);

	if (!media)
		return;

#define VLC_META(media, cd, did, tid, tag)                                               \
	else if (strcmp(did, tid) == 0)                                                  \
	{                                                                                \
		calldata_set_string(cd, "tag_data", libvlc_media_get_meta_(media, tag)); \
	}

	if (strcmp(data_id, "title") == 0)
		calldata_set_string(cd, "tag_data", libvlc_media_get_meta_(media, libvlc_meta_Title));

	VLC_META(media, cd, data_id, "artist", libvlc_meta_Artist)
	VLC_META(media, cd, data_id, "genre", libvlc_meta_Genre)
	VLC_META(media, cd, data_id, "copyright", libvlc_meta_Copyright)
	VLC_META(media, cd, data_id, "album", libvlc_meta_Album)
	VLC_META(media, cd, data_id, "track_number", libvlc_meta_TrackNumber)
	VLC_META(media, cd, data_id, "description", libvlc_meta_Description)
	VLC_META(media, cd, data_id, "rating", libvlc_meta_Rating)
	VLC_META(media, cd, data_id, "date", libvlc_meta_Date)
	VLC_META(media, cd, data_id, "setting", libvlc_meta_Setting)
	VLC_META(media, cd, data_id, "url", libvlc_meta_URL)
	VLC_META(media, cd, data_id, "language", libvlc_meta_Language)
	VLC_META(media, cd, data_id, "now_playing", libvlc_meta_NowPlaying)
	VLC_META(media, cd, data_id, "publisher", libvlc_meta_Publisher)
	VLC_META(media, cd, data_id, "encoded_by", libvlc_meta_EncodedBy)
	VLC_META(media, cd, data_id, "artwork_url", libvlc_meta_ArtworkURL)
	VLC_META(media, cd, data_id, "track_id", libvlc_meta_TrackID)
	VLC_META(media, cd, data_id, "track_total", libvlc_meta_TrackTotal)
	VLC_META(media, cd, data_id, "director", libvlc_meta_Director)
	VLC_META(media, cd, data_id, "season", libvlc_meta_Season)
	VLC_META(media, cd, data_id, "episode", libvlc_meta_Episode)
	VLC_META(media, cd, data_id, "show_name", libvlc_meta_ShowName)
	VLC_META(media, cd, data_id, "actors", libvlc_meta_Actors)
	VLC_META(media, cd, data_id, "album_artist", libvlc_meta_AlbumArtist)
	VLC_META(media, cd, data_id, "disc_number", libvlc_meta_DiscNumber)
	VLC_META(media, cd, data_id, "disc_total", libvlc_meta_DiscTotal)

	libvlc_media_release_(media);
#undef VLC_META
}

/* ------------------------------------------------------------------------- */

static const char *vlcs_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VLCSource");
}

static void vlcs_destroy(void *data)
{
	struct vlc_source *c = data;

	if (c->media_list_player) {
		libvlc_media_list_player_stop_(c->media_list_player);
		libvlc_media_list_player_release_(c->media_list_player);
		c->media_list_player = NULL;
	}
	if (c->media_player) {
		libvlc_media_player_release_(c->media_player);
		c->media_player = NULL;
	}
	if (c->media_list) {
		libvlc_media_list_release_(c->media_list);
		c->media_list = NULL;
	}

	/* Stop dedicated audio consumer worker before destroying ring buffers */
	if (atomic_load(&c->pairing_session.worker_created)) {
		atomic_store(&c->pairing_session.worker_running, false);
		if (c->pairing_session.worker_event)
			os_event_signal(c->pairing_session.worker_event);
		pthread_join(c->pairing_session.worker_thread, NULL);
		atomic_store(&c->pairing_session.worker_created, false);
	}
	if (c->pairing_session.worker_event) {
		os_event_destroy(c->pairing_session.worker_event);
		c->pairing_session.worker_event = NULL;
	}

	if (c->pairing_session.initialized) {
		mxf_timeline_aligner_destroy(&c->pairing_session.aligner);
		for (int i = 0; i < MAX_STREAM_OUTPUTS; i++) {
			spsc_ring_buffer_destroy(&c->pairing_session.stream_queues[i]);
		}
		c->pairing_session.initialized = false;
	}

	bfree((void *)c->audio.data[0]);
	obs_source_frame_free(&c->frame);

	free_files(&c->files);
	bfree(c->sub_settings.font_face);
	pthread_mutex_destroy(&c->mutex);
	bfree(c);
}

static void *vlcs_video_lock(void *data, void **planes)
{
	struct vlc_source *c = data;
	for (size_t i = 0; i < MAX_AV_PLANES && c->frame.data[i] != NULL; i++)
		planes[i] = c->frame.data[i];
	return NULL;
}

static void vlcs_video_display(void *data, void *picture)
{
	struct vlc_source *c = data;
	c->frame.timestamp = (uint64_t)libvlc_clock_() * 1000ULL - time_start;
	obs_source_output_video(c->source, &c->frame);

	if (c->video_frames_received < 5 || (c->video_frames_received % 100) == 0) {
		blog(LOG_INFO, "[vlc-video: '%s'] Video frame #%u delivered: TS=%llu ms (%ux%u, format=%d)",
		     obs_source_get_name(c->source), c->video_frames_received,
		     (unsigned long long)(c->frame.timestamp / 1000000ULL), c->frame.width, c->frame.height,
		     c->frame.format);
	}
	c->video_frames_received++;

	UNUSED_PARAMETER(picture);
}

static void calculate_display_size(struct vlc_source *c, unsigned *width, unsigned *height)
{
	libvlc_media_t *media = libvlc_media_player_get_media_(c->media_player);

	if (!media)
		return;

	libvlc_media_track_t **tracks;

	unsigned count = libvlc_media_tracks_get_(media, &tracks);

	if (count > 0) {
		for (unsigned i = 0; i < count; i++) {
			libvlc_media_track_t *track = tracks[i];

			if (track->i_type != libvlc_track_video)
				continue;

			unsigned display_width = track->video->i_width;
			unsigned display_height = track->video->i_height;

			if (display_width == 0 || display_height == 0)
				continue;

			/* Keep native decoded dimensions to preserve Direct3D11 hardware decoding
			 * textures and prevent CPU swscale filter insertion. OBS compositor handles
			 * aspect ratio scaling natively on GPU. */

			switch (track->video->i_orientation) {
			case libvlc_video_orient_left_top:
			case libvlc_video_orient_left_bottom:
			case libvlc_video_orient_right_top:
			case libvlc_video_orient_right_bottom:
				/* orientation swaps height and width */
				*width = display_height;
				*height = display_width;
				break;
			default:
				/* height and width not swapped */
				*width = display_width;
				*height = display_height;
				break;
			}
		}

		libvlc_media_tracks_release_(tracks, count);
	}

	libvlc_media_release_(media);
}

static unsigned vlcs_video_format(void **p_data, char *chroma, unsigned *width, unsigned *height, unsigned *pitches,
				  unsigned *lines)
{
	struct vlc_source *c = *p_data;
	enum video_format new_format;
	enum video_range_type range;
	bool new_range;
	size_t i = 0;
	char orig_chroma[8] = {0};
	memcpy(orig_chroma, chroma, 4);

	new_format = convert_vlc_video_format(chroma, &new_range);

	blog(LOG_INFO,
	     "[vlc-video: '%s'] Video format callback: LibVLC offered chroma '%.4s' (%ux%u) -> Negotiated format %d ('%.4s', %ux%u)",
	     obs_source_get_name(c->source), orig_chroma, *width, *height, (int)new_format, chroma, *width, *height);

	if (new_format == VIDEO_FORMAT_NONE) {
		blog(LOG_ERROR, "[vlc-video: '%s'] Format negotiation failed for chroma '%.4s'",
		     obs_source_get_name(c->source), orig_chroma);
		return 0;
	}

	/* The width and height passed from VLC are the buffer size rather than
	 * the correct video display size, and may be the next multiple of 32
	 * up from the original dimension, e.g. 1080 would become 1088. VLC 4.0
	 * will pass the correct display size in *(width+1) and *(height+1) but
	 * for now we need to calculate it ourselves. */
	calculate_display_size(c, width, height);

	/* don't allocate a new frame if format/width/height hasn't changed */
	if (c->frame.format != new_format || c->frame.width != *width || c->frame.height != *height) {
		obs_source_frame_free(&c->frame);
		obs_source_frame_init(&c->frame, new_format, *width, *height);

		c->frame.format = new_format;
		c->frame.full_range = new_range;
		range = c->frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, range, new_format, c->frame.color_matrix,
						       c->frame.color_range_min, c->frame.color_range_max);
	}

	while (c->frame.data[i]) {
		pitches[i] = (unsigned)c->frame.linesize[i];
		lines[i] = get_format_lines(c->frame.format, *height, i);
		i++;
	}

	return 1;
}

static inline struct mxf_stream_ctx *get_mxf_stream_ctx(void *data)
{
	if (!data)
		return NULL;
	struct mxf_stream_ctx *ctx = (struct mxf_stream_ctx *)data;
	if (ctx->magic == MXF_STREAM_MAGIC && ctx->source != NULL &&
	    ctx->slot_id >= 0 && ctx->slot_id < MAX_STREAM_OUTPUTS &&
	    &ctx->source->pairing_session.streams[ctx->slot_id] == ctx) {
		return ctx;
	}
	return NULL;
}

static void vlcs_audio_cleanup(void *data)
{
	struct mxf_stream_ctx *ctx = get_mxf_stream_ctx(data);
	if (ctx && ctx->source && ctx->source->pairing_mode != PAIR_MODE_PASSTHROUGH) {
		atomic_store(&ctx->is_active, false);
		atomic_fetch_sub(&ctx->source->pairing_session.active_streams, 1);
		/* Memory remains permanently pinned in struct vlc_source. Zero free() called! */
	}
}

static void *pairing_audio_worker_thread(void *param)
{
	struct vlc_source *c = (struct vlc_source *)param;
	os_set_thread_name("mxf-audio-worker");

	int16_t diagnostic_buf[3840]; /* 1920 stereo frames = 3840 int16 samples = 40ms @ 48kHz */

	while (atomic_load_explicit(&c->pairing_session.worker_running, memory_order_acquire)) {
		/* Timed wait: wakes on signal from producer/flush or every 5ms */
		os_event_timedwait(c->pairing_session.worker_event, 5);

		if (!atomic_load_explicit(&c->pairing_session.worker_running, memory_order_acquire))
			break;

		if (c->pairing_mode == PAIR_MODE_PASSTHROUGH || c->pairing_mode == PAIR_MODE_AUTO || atomic_load(&c->pairing_session.direct_stereo))
			continue;

		int slot_l = 0;
		int slot_r = 1;

		if (c->pairing_mode == PAIR_MODE_AUTO) {
			if (!atomic_load(&c->pairing_session.routing_locked)) {
				int active_slots[MAX_STREAM_OUTPUTS];
				int num_active = 0;
				int total_active_streams = atomic_load(&c->pairing_session.active_streams);
				uint64_t max_samples = 0;

				for (int i = 0; i < total_active_streams && i < MAX_STREAM_OUTPUTS; i++) {
					struct mxf_stream_ctx *ctx = &c->pairing_session.streams[i];
					uint64_t samples = atomic_load_explicit(&ctx->total_samples_received, memory_order_relaxed);
					uint64_t energy = atomic_load_explicit(&ctx->total_energy_accum, memory_order_relaxed);

					if (samples > max_samples)
						max_samples = samples;

					if (samples >= 960) {
						uint64_t mean_energy = energy / samples;
						/* Threshold: mean_energy > 400 (RMS > 20, ~ -64 dBFS) indicates active audio */
						if (mean_energy > 400) {
							active_slots[num_active++] = i;
						}
					}
				}

				if (num_active >= 2) {
					/* Two active streams found! Lower track = L, Higher track = R */
					slot_l = active_slots[0];
					slot_r = active_slots[1];
					atomic_store(&c->pairing_session.left_slot, slot_l);
					atomic_store(&c->pairing_session.right_slot, slot_r);
					atomic_store(&c->pairing_session.routing_locked, true);
					blog(LOG_INFO, "[vlc-video] Playout Auto-Detect locked: Stereo Track %d (L) + Track %d (R)",
					     slot_l + 1, slot_r + 1);
				} else if (num_active == 1) {
					/* If only 1 total stream opened, or grace period elapsed without partner: lock Dual-Mono */
					if (total_active_streams <= 1 || max_samples >= 7200) {
						slot_l = active_slots[0];
						slot_r = active_slots[0];
						atomic_store(&c->pairing_session.left_slot, slot_l);
						atomic_store(&c->pairing_session.right_slot, slot_r);
						atomic_store(&c->pairing_session.routing_locked, true);
						blog(LOG_INFO, "[vlc-video] Playout Auto-Detect locked: Dual-Mono Track %d (Center Mix)",
						     slot_l + 1);
					} else {
						slot_l = active_slots[0];
						slot_r = active_slots[0];
					}
				} else {
					/* No signal detected yet (initial silence): fallback default */
					slot_l = 0;
					slot_r = (total_active_streams > 1) ? 1 : 0;
					atomic_store(&c->pairing_session.left_slot, slot_l);
					atomic_store(&c->pairing_session.right_slot, slot_r);
				}
			} else {
				slot_l = atomic_load(&c->pairing_session.left_slot);
				slot_r = atomic_load(&c->pairing_session.right_slot);
			}
		} else if (c->pairing_mode == PAIR_MODE_MANUAL_1_2) {
			slot_l = 0; slot_r = 1;
		} else if (c->pairing_mode == PAIR_MODE_MANUAL_1_3) {
			slot_l = 0; slot_r = 2;
		} else if (c->pairing_mode == PAIR_MODE_MANUAL_3_4) {
			slot_l = 2; slot_r = 3;
		}

		/* DUAL-MONO CENTER MIX (Single active track) */
		if (slot_l == slot_r) {
			uint32_t avail = spsc_ring_buffer_available_read(&c->pairing_session.stream_queues[slot_l]);
			if (avail > 0) {
				uint32_t to_read = (avail > 1920) ? 1920 : avail;
				int16_t mono_buf[1920];
				int64_t pts = 0;
				uint32_t read_count = spsc_ring_buffer_read(&c->pairing_session.stream_queues[slot_l],
									     mono_buf, to_read, &pts);
				if (read_count > 0) {
					for (uint32_t i = 0; i < read_count; i++) {
						diagnostic_buf[i * 2] = mono_buf[i];
						diagnostic_buf[i * 2 + 1] = mono_buf[i];
					}
					uint64_t ts = ((uint64_t)pts >= time_start) ? ((uint64_t)pts - time_start) : 0;
					struct obs_source_audio paired_audio = {
						.data = {(const uint8_t *)diagnostic_buf},
						.frames = read_count,
						.speakers = SPEAKERS_STEREO,
						.format = AUDIO_FORMAT_16BIT,
						.samples_per_sec = 48000,
						.timestamp = ts,
					};
					if (c->source)
						obs_source_output_audio(c->source, &paired_audio);
				}
			}
			continue;
		}

		/* STEREO PAIRING VIA TIMELINE ALIGNER */
		mxf_timeline_aligner_set_queues(&c->pairing_session.aligner,
						&c->pairing_session.stream_queues[slot_l],
						&c->pairing_session.stream_queues[slot_r]);

		uint32_t frames_aligned = 0;
		int64_t aligned_pts = 0;
		do {
			frames_aligned = mxf_timeline_aligner_process(
				&c->pairing_session.aligner, diagnostic_buf, 1920, &aligned_pts);
			if (frames_aligned > 0) {
				c->pairing_session.diagnostic_frames_processed += frames_aligned;
				uint64_t ts = ((uint64_t)aligned_pts >= time_start) ? ((uint64_t)aligned_pts - time_start) : 0;
				struct obs_source_audio paired_audio = {
					.data = {(const uint8_t *)diagnostic_buf},
					.frames = frames_aligned,
					.speakers = SPEAKERS_STEREO,
					.format = AUDIO_FORMAT_16BIT,
					.samples_per_sec = 48000,
					.timestamp = ts,
				};
				if (c->source)
					obs_source_output_audio(c->source, &paired_audio);
			}
		} while (frames_aligned > 0 &&
			 atomic_load_explicit(&c->pairing_session.worker_running, memory_order_acquire));

		/* Discard data from unused queues so they don't overflow */
		int total_streams = atomic_load(&c->pairing_session.active_streams);
		for (int i = 0; i < total_streams && i < MAX_STREAM_OUTPUTS; i++) {
			if (i != slot_l && i != slot_r) {
				uint32_t unused_avail = spsc_ring_buffer_available_read(&c->pairing_session.stream_queues[i]);
				if (unused_avail > 4800) {
					spsc_ring_buffer_discard(&c->pairing_session.stream_queues[i], unused_avail);
				}
			}
		}
	}

	return NULL;
}

static void vlcs_audio_flush(void *data, int64_t pts)
{
	struct mxf_stream_ctx *ctx = get_mxf_stream_ctx(data);
	if (ctx && ctx->source && ctx->source->pairing_mode != PAIR_MODE_PASSTHROUGH) {
		/* Option B: Autonomously increment this stream's independent seek sequence counter.
		 * 100% lock-free, zero cross-stream reading, zero inter-thread contention! */
		atomic_fetch_add_explicit(&ctx->stream_epoch, 1, memory_order_release);

		if (ctx->source->pairing_session.worker_event)
			os_event_signal(ctx->source->pairing_session.worker_event);
		(void)pts;
	}
}

static void vlcs_audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
	struct vlc_source *c = NULL;
	struct mxf_stream_ctx *ctx = get_mxf_stream_ctx(data);

	if (ctx && ctx->source && ctx->source->pairing_mode != PAIR_MODE_PASSTHROUGH) {
		c = ctx->source;
	} else {
		c = (struct vlc_source *)data;
	}

	/* 100% UNTOUCHED PASS-THROUGH LOGIC (WITH REALLOC SIZE BUG FIX) */
	if (!c || c->pairing_mode == PAIR_MODE_PASSTHROUGH || c->pairing_mode == PAIR_MODE_AUTO) {
		if (!c || !c->source)
			return;
		size_t size = get_audio_size(c->audio.format, c->audio.speakers, count);

		if (c->audio_capacity < size) {
			c->audio.data[0] = brealloc((void *)c->audio.data[0], size);
			c->audio_capacity = size;
		}

		memcpy((void *)c->audio.data[0], samples, size);
		c->audio.timestamp = (uint64_t)pts * 1000ULL - time_start;
		c->audio.frames = count;

		obs_source_output_audio(c->source, &c->audio);
		return;
	}

	/* MULTI-TRACK / AUTO-DETECTION MODE (PHASE E4B) */
	if (!ctx || !atomic_load(&ctx->is_active))
		return;

	/* If native stereo was detected on Stream 0: direct stereo output */
	if (atomic_load(&c->pairing_session.direct_stereo)) {
		if (ctx->slot_id == 0 && c->source) {
			uint64_t ts = ((uint64_t)pts * 1000ULL >= time_start) ? ((uint64_t)pts * 1000ULL - time_start) : 0;
			struct obs_source_audio direct_audio = {
				.data = {(const uint8_t *)samples},
				.frames = count,
				.speakers = SPEAKERS_STEREO,
				.format = AUDIO_FORMAT_16BIT,
				.samples_per_sec = ctx->sample_rate ? ctx->sample_rate : 48000,
				.timestamp = ts,
			};
			obs_source_output_audio(c->source, &direct_audio);
		}
		return;
	}

	int slot = ctx->slot_id;
	if (slot < 0 || slot >= MAX_STREAM_OUTPUTS)
		return;

	int64_t pts_ns = pts * 1000LL;
	uint32_t stream_epoch = atomic_load_explicit(&ctx->stream_epoch, memory_order_acquire);

	/* Energy Accumulation for Auto-Detection */
	const int16_t *s16 = (const int16_t *)samples;
	uint64_t block_energy = 0;
	for (unsigned i = 0; i < count; i++) {
		int32_t v = s16[i];
		block_energy += (uint64_t)(v * v);
	}
	atomic_fetch_add_explicit(&ctx->total_energy_accum, block_energy, memory_order_relaxed);
	atomic_fetch_add_explicit(&ctx->total_samples_received, count, memory_order_relaxed);

	/* Write block tagged with stream_epoch into this stream's own lock-free SPSC queue */
	spsc_ring_buffer_write_epoch(&c->pairing_session.stream_queues[slot], samples, count, pts_ns, stream_epoch);

	/* Wake up dedicated audio worker (Single Consumer) */
	if (c->pairing_session.worker_event)
		os_event_signal(c->pairing_session.worker_event);
}

static int vlcs_audio_setup(void **p_data, char *format, unsigned *rate, unsigned *channels)
{
	struct vlc_source *c = *p_data;
	if (!c)
		return -1;

	/* In Pass-through or Auto mode: direct native OBS audio output */
	if (c->pairing_mode == PAIR_MODE_PASSTHROUGH || c->pairing_mode == PAIR_MODE_AUTO) {
		enum audio_format new_audio_format;
		struct obs_audio_info aoi;
		obs_get_audio_info(&aoi);
		uint32_t out_channels = get_audio_channels(aoi.speakers);

		new_audio_format = convert_vlc_audio_format(format);
		if (*channels > out_channels)
			*channels = out_channels;

		if (c->audio.format == new_audio_format && c->audio.samples_per_sec == *rate &&
		    c->audio.speakers == (enum speaker_layout)*channels)
			return 0;

		c->audio_capacity = 0;
		bfree((void *)c->audio.data[0]);
		c->audio.data[0] = NULL;

		memset(&c->audio, 0, sizeof(c->audio));
		c->audio.speakers = (enum speaker_layout)*channels;
		c->audio.samples_per_sec = *rate;
		c->audio.format = new_audio_format;
		return 0;
	}

	/* In Multi-track / Auto Mode: */
	int slot = atomic_fetch_add(&c->pairing_session.active_streams, 1);
	if (slot >= MAX_STREAM_OUTPUTS)
		return -1;

	struct mxf_stream_ctx *ctx = &c->pairing_session.streams[slot];
	ctx->magic = MXF_STREAM_MAGIC;
	ctx->slot_id = slot;
	ctx->channels = *channels;
	ctx->sample_rate = *rate;
	atomic_store(&ctx->is_active, true);
	atomic_store(&ctx->stream_epoch, 0);
	atomic_store(&ctx->total_energy_accum, 0);
	atomic_store(&ctx->total_samples_received, 0);
	ctx->role = STREAM_ROLE_UNKNOWN;
	ctx->first_pts = -1;
	ctx->generation = c->pairing_session.generation;
	ctx->source = c;

	/* If first stream already has 2 or more channels (native stereo/multichannel): */
	if (slot == 0 && *channels >= 2) {
		atomic_store(&c->pairing_session.direct_stereo, true);
		*channels = 2;
		*(uint32_t *)format = (uint32_t)MAKEFORMAT('S', '1', '6', 'N');
		blog(LOG_INFO, "[vlc-video] Stream 0 is native stereo (%u channels, %u Hz), direct pass-through active",
		     *channels, *rate);
		*p_data = ctx;
		return 0;
	}

	blog(LOG_INFO, "[vlc-video] Multi-track stream %d initialized: rate=%u, channels=%u (mode %d)",
	     slot, *rate, *channels, (int)c->pairing_mode);

	*channels = 1;
	*(uint32_t *)format = (uint32_t)MAKEFORMAT('S', '1', '6', 'N');

	*p_data = ctx;
	return 0;
}

static void add_file(struct vlc_source *c, media_file_array_t *new_files, const char *path, int network_caching,
		     int track_index, int subtitle_index, bool subtitle_enable, uint64_t item_id)
{
	struct media_file_data data;
	struct dstr new_path = {0};
	libvlc_media_t *new_media;
	bool is_url = path && strstr(path, "://") != NULL;

	dstr_copy(&new_path, path);
#ifdef _WIN32
	if (!is_url)
		dstr_replace(&new_path, "/", "\\");
#endif
	path = new_path.array;

	new_media = get_media(&c->files, path);

	if (!new_media)
		new_media = get_media(new_files, path);
	if (!new_media)
		new_media = create_media_from_file(path);

	if (new_media) {
		if (c->hw_decode) {
			libvlc_media_add_option_(new_media, ":avcodec-hw=any");
			libvlc_media_add_option_(new_media, ":deinterlace=0");
		} else {
			libvlc_media_add_option_(new_media, ":avcodec-hw=none");
		}

		bool is_remote = is_url;
#ifdef _WIN32
		if (!is_remote && path) {
			if (path[0] == '\\' && path[1] == '\\') {
				is_remote = true;
			} else if (path[0] != '\0' && path[1] == ':') {
				char drive_root[4] = {path[0], ':', '\\', '\0'};
				if (GetDriveTypeA(drive_root) == DRIVE_REMOTE) {
					is_remote = true;
				}
			}
		}
#endif
		int cache_ms = (network_caching > 1000) ? network_caching : (is_remote ? 2000 : 1000);
		struct dstr file_caching_opt = {0};
		dstr_catf(&file_caching_opt, ":file-caching=%d", cache_ms);
		libvlc_media_add_option_(new_media, file_caching_opt.array);
		dstr_free(&file_caching_opt);

		if (is_remote || is_url) {
			struct dstr network_caching_option = {0};
			dstr_catf(&network_caching_option, ":network-caching=%d", cache_ms);
			libvlc_media_add_option_(new_media, network_caching_option.array);
			dstr_free(&network_caching_option);
		}

		if (!subtitle_enable) {
			libvlc_media_add_option_(new_media, ":no-sub-autodetect-file");
		}

		if (c->pairing_mode == PAIR_MODE_PASSTHROUGH || c->pairing_mode == PAIR_MODE_AUTO) {
			struct dstr track_option = {0};
			dstr_catf(&track_option, ":audio-track=%d", track_index - 1);
			libvlc_media_add_option_(new_media, track_option.array);
			dstr_free(&track_option);
		} else {
			libvlc_media_add_option_(new_media, ":sout-all");
			libvlc_media_add_option_(new_media, ":sout=#duplicate{dst=display}");
			libvlc_media_add_option_(new_media, ":sout-mux-caching=500");
		}

		struct dstr sub_option = {0};
		if (subtitle_enable) {
			dstr_catf(&sub_option, ":sub-track=%d", subtitle_index - 1);
		}
		libvlc_media_add_option_(new_media, sub_option.array);
		dstr_free(&sub_option);

		if (subtitle_enable && c->sub_settings.custom_enable) {
			apply_custom_subtitle_settings(c);

			if (c->sub_settings.font_face && *c->sub_settings.font_face) {
				struct dstr font_opt = {0};
				dstr_catf(&font_opt, ":freetype-font=%s", c->sub_settings.font_face);
				libvlc_media_add_option_(new_media, font_opt.array);
				dstr_free(&font_opt);
			}
			if (c->sub_settings.font_size > 0) {
				struct dstr size_opt = {0};
				dstr_catf(&size_opt, ":freetype-fontsize=%d", c->sub_settings.font_size);
				libvlc_media_add_option_(new_media, size_opt.array);
				dstr_free(&size_opt);
			}
			if (c->sub_settings.font_bold) {
				libvlc_media_add_option_(new_media, ":freetype-bold");
			}
			struct dstr color_opt = {0};
			dstr_catf(&color_opt, ":freetype-color=%u",
				  (unsigned)obs_to_vlc_color(c->sub_settings.text_color));
			libvlc_media_add_option_(new_media, color_opt.array);
			dstr_free(&color_opt);

			struct dstr outline_opt = {0};
			int outline_th = c->sub_settings.outline_enable ? c->sub_settings.outline_thickness : 0;
			dstr_catf(&outline_opt, ":freetype-outline-thickness=%d", outline_th);
			libvlc_media_add_option_(new_media, outline_opt.array);
			dstr_free(&outline_opt);

			if (outline_th > 0) {
				struct dstr outline_color_opt = {0};
				dstr_catf(&outline_color_opt, ":freetype-outline-color=%u",
					  (unsigned)obs_to_vlc_color(c->sub_settings.outline_color));
				libvlc_media_add_option_(new_media, outline_color_opt.array);
				dstr_free(&outline_color_opt);
			}

			if (c->sub_settings.shadow_enable) {
				float dist = 0.60f;
				if (c->sub_settings.shadow_distance <= 4 && c->sub_settings.shadow_distance >= 1) {
					if (c->sub_settings.shadow_distance == 1) dist = 0.30f;
					else if (c->sub_settings.shadow_distance == 2) dist = 0.50f;
					else if (c->sub_settings.shadow_distance == 3) dist = 0.70f;
					else if (c->sub_settings.shadow_distance == 4) dist = 0.90f;
				} else if (c->sub_settings.shadow_distance > 4) {
					dist = (float)c->sub_settings.shadow_distance / 100.0f;
					if (dist > 1.0f) dist = 1.0f;
					if (dist < 0.05f) dist = 0.05f;
				}
				struct dstr shadow_dist_opt = {0};
				dstr_catf(&shadow_dist_opt, ":freetype-shadow-distance=%.2f", dist);
				libvlc_media_add_option_(new_media, shadow_dist_opt.array);
				dstr_free(&shadow_dist_opt);

				struct dstr shadow_color_opt = {0};
				dstr_catf(&shadow_color_opt, ":freetype-shadow-color=%u",
					  (unsigned)obs_to_vlc_color(c->sub_settings.shadow_color));
				libvlc_media_add_option_(new_media, shadow_color_opt.array);
				dstr_free(&shadow_color_opt);

				int shadow_op = (c->sub_settings.shadow_opacity * 255) / 100;
				if (shadow_op > 255) shadow_op = 255;
				if (shadow_op < 0) shadow_op = 0;
				struct dstr shadow_op_opt = {0};
				dstr_catf(&shadow_op_opt, ":freetype-shadow-opacity=%d", shadow_op);
				libvlc_media_add_option_(new_media, shadow_op_opt.array);
				dstr_free(&shadow_op_opt);
			} else {
				libvlc_media_add_option_(new_media, ":freetype-shadow-distance=0.0");
				libvlc_media_add_option_(new_media, ":freetype-shadow-opacity=0");
			}

			if (c->sub_settings.bg_enable && c->sub_settings.bg_opacity > 0) {
				int op = (c->sub_settings.bg_opacity * 255) / 100;
				if (op > 255) op = 255;
				if (op < 0) op = 0;
				struct dstr bg_col_opt = {0};
				dstr_catf(&bg_col_opt, ":freetype-background-color=%u",
					  (unsigned)obs_to_vlc_color(c->sub_settings.bg_color));
				libvlc_media_add_option_(new_media, bg_col_opt.array);
				dstr_free(&bg_col_opt);

				struct dstr bg_op_opt = {0};
				dstr_catf(&bg_op_opt, ":freetype-background-opacity=%d", op);
				libvlc_media_add_option_(new_media, bg_op_opt.array);
				dstr_free(&bg_op_opt);
			} else {
				libvlc_media_add_option_(new_media, ":freetype-background-opacity=0");
			}

			if (c->sub_settings.margin >= 0) {
				struct dstr margin_opt = {0};
				dstr_catf(&margin_opt, ":sub-margin=%d", c->sub_settings.margin);
				libvlc_media_add_option_(new_media, margin_opt.array);
				dstr_free(&margin_opt);
			}

			if (c->sub_settings.override_ass) {
				libvlc_media_add_option_(new_media, ":sub-text-scale=100");
				if (c->sub_settings.font_face && *c->sub_settings.font_face) {
					struct dstr libass_font = {0};
					dstr_catf(&libass_font, ":libass-font=%s", c->sub_settings.font_face);
					libvlc_media_add_option_(new_media, libass_font.array);
					dstr_free(&libass_font);
				}
			}
		}

		data.id = item_id;
		data.path = new_path.array;
		data.media = new_media;
		da_push_back(*new_files, &data);
	} else {
		dstr_free(&new_path);
	}
}

static bool valid_extension(const char *ext)
{
	struct dstr test = {0};
	bool valid = false;
	const char *b;
	const char *e;

	if (!ext || !*ext)
		return false;

	b = &EXTENSIONS_MEDIA[1];
	e = strchr(b, ';');

	for (;;) {
		if (e)
			dstr_ncopy(&test, b, e - b);
		else
			dstr_copy(&test, b);

		if (dstr_cmpi(&test, ext) == 0) {
			valid = true;
			break;
		}

		if (!e)
			break;

		b = e + 2;
		e = strchr(b, ';');
	}

	dstr_free(&test);
	return valid;
}

static void vlcs_update(void *data, obs_data_t *settings)
{
	media_file_array_t new_files;
	media_file_array_t old_files;
	struct vlc_source *c = data;
	obs_data_array_t *array;
	const char *behavior;
	size_t count;
	int network_caching;
	int track_index;
	int subtitle_index;
	bool subtitle_enable;
	bool hw_decode_val;
	bool loop_val;
	bool shuffle_val;

	da_init(new_files);
	da_init(old_files);

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);

	loop_val = obs_data_get_bool(settings, S_LOOP);
	hw_decode_val = obs_data_get_bool(settings, S_HW_DECODE);
	behavior = obs_data_get_string(settings, S_BEHAVIOR);
	network_caching = (int)obs_data_get_int(settings, S_NETWORK_CACHING);
	track_index = (int)obs_data_get_int(settings, S_TRACK);
	c->pairing_mode = (enum mxf_pairing_mode)obs_data_get_int(settings, S_STEREO_PAIRING_MODE);
	subtitle_index = (int)obs_data_get_int(settings, S_SUBTITLE_TRACK);
	subtitle_enable = obs_data_get_bool(settings, S_SUBTITLE_ENABLE);
	shuffle_val = obs_data_get_bool(settings, S_SHUFFLE);

	c->sub_settings.custom_enable = obs_data_get_bool(settings, S_SUBTITLE_CUSTOM_ENABLE);
	obs_data_t *font_obj = obs_data_get_obj(settings, S_SUBTITLE_FONT);
	if (font_obj) {
		bfree(c->sub_settings.font_face);
		const char *face = obs_data_get_string(font_obj, "face");
		c->sub_settings.font_face = (face && *face) ? bstrdup(face) : bstrdup("Segoe UI");
		c->sub_settings.font_size = (int)obs_data_get_int(font_obj, "size");
		long flags = obs_data_get_int(font_obj, "flags");
		c->sub_settings.font_bold = (flags & OBS_FONT_BOLD) != 0;
		c->sub_settings.font_italic = (flags & OBS_FONT_ITALIC) != 0;
		obs_data_release(font_obj);
	}
	c->sub_settings.text_color = (uint32_t)obs_data_get_int(settings, S_SUBTITLE_COLOR);
	c->sub_settings.outline_enable = obs_data_get_bool(settings, S_SUBTITLE_OUTLINE_ENABLE);
	c->sub_settings.outline_thickness = (int)obs_data_get_int(settings, S_SUBTITLE_OUTLINE);
	c->sub_settings.outline_color = (uint32_t)obs_data_get_int(settings, S_SUBTITLE_OUTLINE_COLOR);
	c->sub_settings.shadow_enable = obs_data_get_bool(settings, S_SUBTITLE_SHADOW_ENABLE);
	c->sub_settings.shadow_distance = (int)obs_data_get_int(settings, S_SUBTITLE_SHADOW);
	c->sub_settings.shadow_color = (uint32_t)obs_data_get_int(settings, S_SUBTITLE_SHADOW_COLOR);
	int s_op = (int)obs_data_get_int(settings, S_SUBTITLE_SHADOW_OPACITY);
	c->sub_settings.shadow_opacity = (s_op > 0) ? s_op : 100;
	c->sub_settings.bg_enable = obs_data_get_bool(settings, S_SUBTITLE_BG_ENABLE);
	c->sub_settings.bg_color = (uint32_t)obs_data_get_int(settings, S_SUBTITLE_BG_COLOR);
	c->sub_settings.bg_opacity = (int)obs_data_get_int(settings, S_SUBTITLE_BG_OPACITY);
	c->sub_settings.margin = (int)obs_data_get_int(settings, S_SUBTITLE_MARGIN);
	c->sub_settings.override_ass = obs_data_get_bool(settings, S_SUBTITLE_OVERRIDE_ASS);

	apply_custom_subtitle_settings(c);

	c->behavior = BEHAVIOR_STOP_RESTART;
	if (astrcmpi(behavior, S_BEHAVIOR_PAUSE_UNPAUSE) == 0) {
		c->behavior = BEHAVIOR_PAUSE_UNPAUSE;
	} else if (astrcmpi(behavior, S_BEHAVIOR_ALWAYS_PLAY) == 0) {
		c->behavior = BEHAVIOR_ALWAYS_PLAY;
	}

	bool loop_changed = (c->loop != loop_val);
	c->loop = loop_val;
	if (c->media_list_player && loop_changed) {
		libvlc_media_list_player_set_playback_mode_(
			c->media_list_player, c->loop ? libvlc_playback_mode_loop : libvlc_playback_mode_default);
	}

	bool hw_decode_changed = (c->hw_decode != hw_decode_val);
	c->hw_decode = hw_decode_val;

	/* ------------------------------------- */
	/* Parse new list of sources from settings */

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		if (!path || !*path) {
			obs_data_release(item);
			continue;
		}
		uint64_t item_id = get_or_create_item_id(item);
		os_dir_t *dir = os_opendir(path);

		if (dir) {
			struct dstr dir_path = {0};
			struct os_dirent *ent;

			for (;;) {
				const char *ext;

				ent = os_readdir(dir);
				if (!ent)
					break;
				if (ent->directory)
					continue;

				ext = os_get_path_extension(ent->d_name);
				if (!valid_extension(ext))
					continue;

				dstr_copy(&dir_path, path);
				dstr_cat_ch(&dir_path, '/');
				dstr_cat(&dir_path, ent->d_name);
				add_file(c, &new_files, dir_path.array, network_caching, track_index, subtitle_index,
					 subtitle_enable, generate_stable_item_id());
			}

			dstr_free(&dir_path);
			os_closedir(dir);
		} else {
			add_file(c, &new_files, path, network_caching, track_index, subtitle_index, subtitle_enable,
				 item_id);
		}

		obs_data_release(item);
	}

	/* ------------------------------------- */
	/* 1. Track Selection without restart */
	bool track_changed = (c->current_audio_track != track_index);
	bool sub_changed = (c->current_subtitle_enable != subtitle_enable ||
			    c->current_subtitle_track != subtitle_index);

	c->current_audio_track = track_index;
	c->current_subtitle_track = subtitle_index;
	c->current_subtitle_enable = subtitle_enable;
	c->network_caching = network_caching;

	/* ------------------------------------- */
	/* Check currently playing item */
	struct media_file_data *cur_playing = get_current_playing_file(c);
	bool is_playing = (cur_playing != NULL);

	/* ------------------------------------- */
	/* 4. Clear Playlist */
	if (c->files.num > 0 && new_files.num == 0) {
		blog(LOG_INFO, "[vlc-video] Clearing playlist (was %u items)", (unsigned)c->files.num);
		libvlc_media_list_player_stop_(c->media_list_player);
		libvlc_media_player_stop_(c->media_player);
		if (c->source)
			obs_source_output_video(c->source, NULL);

		if (c->media_list) {
			libvlc_media_list_lock_(c->media_list);
			while (libvlc_media_list_count_(c->media_list) > 0)
				libvlc_media_list_remove_index_(c->media_list, 0);
			libvlc_media_list_unlock_(c->media_list);
		}

		pthread_mutex_lock(&c->mutex);
		free_files(&c->files);
		da_init(c->files);
		pthread_mutex_unlock(&c->mutex);

		atomic_store(&c->pairing_session.routing_locked, false);
		atomic_store(&c->pairing_session.direct_stereo, false);
		atomic_store(&c->pairing_session.active_streams, 0);
		for (int i = 0; i < MAX_STREAM_OUTPUTS; i++) {
			spsc_ring_buffer_reset(&c->pairing_session.stream_queues[i]);
		}
		mxf_timeline_aligner_reset(&c->pairing_session.aligner);

		obs_data_array_release(array);
		return;
	}

	/* ------------------------------------- */
	/* 2. Empty playlist -> first files added */
	if (c->files.num == 0 && new_files.num > 0) {
		blog(LOG_INFO, "[vlc-video] Initializing playlist with %u items", (unsigned)new_files.num);
		pthread_mutex_lock(&c->mutex);
		c->files = new_files;
		pthread_mutex_unlock(&c->mutex);

		if (!c->media_list)
			c->media_list = libvlc_media_list_new_(libvlc);

		libvlc_media_list_lock_(c->media_list);
		while (libvlc_media_list_count_(c->media_list) > 0)
			libvlc_media_list_remove_index_(c->media_list, 0);
		for (size_t i = 0; i < c->files.num; i++)
			libvlc_media_list_add_media_(c->media_list, c->files.array[i].media);
		libvlc_media_list_unlock_(c->media_list);

		libvlc_media_list_player_set_media_list_(c->media_list_player, c->media_list);
		libvlc_media_list_player_set_playback_mode_(
			c->media_list_player, c->loop ? libvlc_playback_mode_loop : libvlc_playback_mode_default);

		if (c->behavior == BEHAVIOR_ALWAYS_PLAY || (c->source && obs_source_active(c->source))) {
			libvlc_media_list_player_play_(c->media_list_player);
			apply_audio_track_runtime(c, track_index);
			apply_subtitle_track_runtime(c, subtitle_enable, subtitle_index);
		} else if (c->source) {
			obs_source_output_video(c->source, NULL);
		}

		obs_data_array_release(array);
		return;
	}

	/* ------------------------------------- */
	/* 3. Remove Files from Playlist: check if currently playing file was removed */
	bool cur_playing_in_new = false;
	if (cur_playing) {
		for (size_t i = 0; i < new_files.num; i++) {
			if (new_files.array[i].id == cur_playing->id ||
			    (cur_playing->path && new_files.array[i].path &&
			     strcmp(new_files.array[i].path, cur_playing->path) == 0)) {
				cur_playing_in_new = true;
				break;
			}
		}
	}

	if (is_playing && cur_playing && !cur_playing_in_new) {
#ifdef _WIN32
		const char *fname = cur_playing->path;
		const char *slash = strrchr(fname, '\\');
		if (!slash)
			slash = strrchr(fname, '/');
		if (slash)
			fname = slash + 1;

		const char *msg_format = obs_module_text("RemovePlayingItem.Message");
		const char *dialog_title = obs_module_text("RemovePlayingItem.Title");

		struct dstr formatted_msg = {0};
		dstr_catf(&formatted_msg, msg_format, fname);

		wchar_t wmsg[1024] = {0};
		wchar_t wtitle[256] = {0};
		MultiByteToWideChar(CP_UTF8, 0, formatted_msg.array, -1, wmsg, 1024);
		MultiByteToWideChar(CP_UTF8, 0, dialog_title, -1, wtitle, 256);
		dstr_free(&formatted_msg);

		HWND parent = GetActiveWindow();
		if (!parent)
			parent = GetForegroundWindow();

		int mb_res = MessageBoxW(parent, wmsg, wtitle,
					 MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
		if (mb_res != IDYES) {
			/* CANCELLED: leave playlist and playback completely unchanged */
			blog(LOG_INFO, "[vlc-video] Removal of currently playing file cancelled by user");
			free_files(&new_files);

			/* Restore settings array to original c->files so OBS UI remains unchanged */
			obs_data_array_t *orig_array = obs_data_array_create();
			for (size_t i = 0; i < c->files.num; i++) {
				obs_data_t *orig_item = obs_data_create();
				obs_data_set_string(orig_item, "value", c->files.array[i].path);
				obs_data_set_int(orig_item, "id", (long long)c->files.array[i].id);
				obs_data_array_push_back(orig_array, orig_item);
				obs_data_release(orig_item);
			}
			obs_data_set_array(settings, S_PLAYLIST, orig_array);
			obs_data_array_release(orig_array);
			obs_data_array_release(array);
			return;
		}
#endif
		/* CONFIRMED: Stop playback and release current media before removing item.
		 * Do not automatically advance to the next playlist item. */
		blog(LOG_INFO, "[vlc-video] Removal of currently playing file confirmed, stopping playback");
		libvlc_media_list_player_stop_(c->media_list_player);
		libvlc_media_player_stop_(c->media_player);
		if (c->source)
			obs_source_output_video(c->source, NULL);

		pthread_mutex_lock(&c->mutex);
		free_files(&c->files);
		c->files = new_files;
		pthread_mutex_unlock(&c->mutex);

		if (!c->media_list)
			c->media_list = libvlc_media_list_new_(libvlc);

		libvlc_media_list_lock_(c->media_list);
		while (libvlc_media_list_count_(c->media_list) > 0)
			libvlc_media_list_remove_index_(c->media_list, 0);
		for (size_t i = 0; i < c->files.num; i++)
			libvlc_media_list_add_media_(c->media_list, c->files.array[i].media);
		libvlc_media_list_unlock_(c->media_list);

		libvlc_media_list_player_set_media_list_(c->media_list_player, c->media_list);
		libvlc_media_list_player_set_playback_mode_(
			c->media_list_player, c->loop ? libvlc_playback_mode_loop : libvlc_playback_mode_default);

		/* Do not automatically advance to next item (do not call play) */
		obs_data_array_release(array);
		return;
	}

	/* ------------------------------------- */
	/* Apply runtime tracks immediately if video is active */
	if (is_playing) {
		if (track_changed)
			apply_audio_track_runtime(c, track_index);
		if (sub_changed)
			apply_subtitle_track_runtime(c, subtitle_enable, subtitle_index);
	}

	/* ------------------------------------- */
	/* Check if playlist files are identical */
	bool playlist_identical = (c->files.num == new_files.num);
	if (playlist_identical) {
		for (size_t i = 0; i < c->files.num; i++) {
			if (c->files.array[i].id != new_files.array[i].id ||
			    strcmp(c->files.array[i].path, new_files.array[i].path) != 0) {
				playlist_identical = false;
				break;
			}
		}
	}

	if (playlist_identical && !hw_decode_changed) {
		/* Only non-playlist properties changed. Playback continues uninterrupted! */
		free_files(&new_files);
		obs_data_array_release(array);
		return;
	}

	/* ------------------------------------- */
	/* If currently playing and hw_decode hasn't changed: seamlessly update playlist */
	if (is_playing && !hw_decode_changed) {
		/* 1. Append newly added files */
		libvlc_media_list_lock_(c->media_list);
		for (size_t i = 0; i < new_files.num; i++) {
			bool found_in_old = false;
			for (size_t j = 0; j < c->files.num; j++) {
				if (c->files.array[j].id == new_files.array[i].id ||
				    (c->files.array[j].path && new_files.array[i].path &&
				     strcmp(c->files.array[j].path, new_files.array[i].path) == 0)) {
					found_in_old = true;
					break;
				}
			}
			if (!found_in_old) {
				libvlc_media_list_add_media_(c->media_list, new_files.array[i].media);
				blog(LOG_INFO, "[vlc-video] Appended file to playing playlist: %s",
				     new_files.array[i].path);
			}
		}

		/* 2. Remove files that were deleted (cur_playing is guaranteed not in this set) */
		for (size_t j = c->files.num; j > 0; j--) {
			size_t idx = j - 1;
			bool found_in_new = false;
			for (size_t i = 0; i < new_files.num; i++) {
				if (new_files.array[i].id == c->files.array[idx].id ||
				    (new_files.array[i].path && c->files.array[idx].path &&
				     strcmp(new_files.array[i].path, c->files.array[idx].path) == 0)) {
					found_in_new = true;
					break;
				}
			}
			if (!found_in_new && &c->files.array[idx] != cur_playing) {
				int m_idx = libvlc_media_list_index_of_item_(c->media_list,
									      c->files.array[idx].media);
				if (m_idx >= 0) {
					libvlc_media_list_remove_index_(c->media_list, m_idx);
					blog(LOG_INFO, "[vlc-video] Removed non-playing file from media list: %s",
					     c->files.array[idx].path);
				}
			}
		}
		libvlc_media_list_unlock_(c->media_list);

		pthread_mutex_lock(&c->mutex);
		old_files = c->files;
		c->files = new_files;
		pthread_mutex_unlock(&c->mutex);
		free_files(&old_files);

		obs_data_array_release(array);
		return;
	}

	/* ------------------------------------- */
	/* Otherwise (stopped, or hw_decode changed): standard clean restart */
	libvlc_media_list_player_stop_(c->media_list_player);

	pthread_mutex_lock(&c->mutex);
	old_files = c->files;
	c->files = new_files;
	pthread_mutex_unlock(&c->mutex);

	c->shuffle = shuffle_val;
	if (c->files.num > 1 && c->shuffle) {
		media_file_array_t shuffled;
		DARRAY(size_t) idxs;

		da_init(shuffled);
		da_init(idxs);
		da_resize(idxs, c->files.num);
		da_reserve(shuffled, c->files.num);

		for (size_t i = 0; i < c->files.num; i++)
			idxs.array[i] = i;
		for (size_t i = idxs.num; i > 0; i--) {
			size_t val = rand() % i;
			size_t idx = idxs.array[val];
			da_push_back(shuffled, &c->files.array[idx]);
			da_erase(idxs, val);
		}

		da_free(c->files);
		da_free(idxs);
		c->files = shuffled;
	}

	free_files(&old_files);

	if (!c->media_list)
		c->media_list = libvlc_media_list_new_(libvlc);

	libvlc_media_list_lock_(c->media_list);
	while (libvlc_media_list_count_(c->media_list) > 0)
		libvlc_media_list_remove_index_(c->media_list, 0);
	for (size_t i = 0; i < c->files.num; i++)
		libvlc_media_list_add_media_(c->media_list, c->files.array[i].media);
	libvlc_media_list_unlock_(c->media_list);

	libvlc_media_list_player_set_media_list_(c->media_list_player, c->media_list);
	libvlc_media_list_player_set_playback_mode_(
		c->media_list_player, c->loop ? libvlc_playback_mode_loop : libvlc_playback_mode_default);

	if (c->files.num && (c->behavior == BEHAVIOR_ALWAYS_PLAY || (c->source && obs_source_active(c->source)))) {
		libvlc_media_list_player_play_(c->media_list_player);
		apply_audio_track_runtime(c, track_index);
		apply_subtitle_track_runtime(c, subtitle_enable, subtitle_index);
	} else if (c->source) {
		obs_source_output_video(c->source, NULL);
	}

	obs_data_array_release(array);
}

static void vlcs_started(const struct libvlc_event_t *event, void *data)
{
	struct vlc_source *c = data;
	obs_source_media_started(c->source);

	atomic_store(&c->pairing_session.routing_locked, false);
	atomic_store(&c->pairing_session.direct_stereo, false);
	atomic_store(&c->pairing_session.active_streams, 0);
	for (int i = 0; i < MAX_STREAM_OUTPUTS; i++) {
		spsc_ring_buffer_reset(&c->pairing_session.stream_queues[i]);
		atomic_store(&c->pairing_session.streams[i].total_energy_accum, 0);
		atomic_store(&c->pairing_session.streams[i].total_samples_received, 0);
	}
	mxf_timeline_aligner_reset(&c->pairing_session.aligner);

	if (c->current_audio_track > 0)
		apply_audio_track_runtime(c, c->current_audio_track);
	apply_subtitle_track_runtime(c, c->current_subtitle_enable, c->current_subtitle_track);

	UNUSED_PARAMETER(event);
}

static void vlcs_stopped(const struct libvlc_event_t *event, void *data)
{
	struct vlc_source *c = data;
	if (!c->loop) {
		obs_source_output_video(c->source, NULL);
	}

	obs_source_media_ended(c->source);

	atomic_store(&c->pairing_session.routing_locked, false);
	atomic_store(&c->pairing_session.direct_stereo, false);
	for (int i = 0; i < MAX_STREAM_OUTPUTS; i++) {
		spsc_ring_buffer_reset(&c->pairing_session.stream_queues[i]);
	}
	mxf_timeline_aligner_reset(&c->pairing_session.aligner);

	UNUSED_PARAMETER(event);
}

static enum obs_media_state vlcs_get_state(void *data)
{
	struct vlc_source *c = data;

	libvlc_state_t state = libvlc_media_player_get_state_(c->media_player);

	switch (state) {
	case libvlc_NothingSpecial:
		return OBS_MEDIA_STATE_NONE;
	case libvlc_Opening:
		return OBS_MEDIA_STATE_OPENING;
	case libvlc_Buffering:
		return OBS_MEDIA_STATE_BUFFERING;
	case libvlc_Playing:
		return OBS_MEDIA_STATE_PLAYING;
	case libvlc_Paused:
		return OBS_MEDIA_STATE_PAUSED;
	case libvlc_Stopped:
		return OBS_MEDIA_STATE_STOPPED;
	case libvlc_Ended:
		return OBS_MEDIA_STATE_ENDED;
	case libvlc_Error:
		return OBS_MEDIA_STATE_ERROR;
	}

	return 0;
}

static void vlcs_play_pause(void *data, bool pause)
{
	struct vlc_source *c = data;

	libvlc_state_t state = libvlc_media_player_get_state_(c->media_player);

	if (pause && state == libvlc_Playing)
		libvlc_media_list_player_pause_(c->media_list_player);
	else if (!pause && state == libvlc_Paused)
		libvlc_media_list_player_play_(c->media_list_player);
}

static void vlcs_restart(void *data)
{
	struct vlc_source *c = data;

	libvlc_media_list_player_stop_(c->media_list_player);
	libvlc_media_list_player_play_(c->media_list_player);
}

static void vlcs_stop(void *data)
{
	struct vlc_source *c = data;

	libvlc_media_list_player_stop_(c->media_list_player);
	obs_source_output_video(c->source, NULL);
}

static void vlcs_playlist_next(void *data)
{
	struct vlc_source *c = data;

	libvlc_media_list_player_next_(c->media_list_player);
}

static void vlcs_playlist_prev(void *data)
{
	struct vlc_source *c = data;

	libvlc_media_list_player_previous_(c->media_list_player);
}

static int64_t vlcs_get_duration(void *data)
{
	struct vlc_source *c = data;

	return (int64_t)libvlc_media_player_get_length_(c->media_player);
}

static int64_t vlcs_get_time(void *data)
{
	struct vlc_source *c = data;

	return (int64_t)libvlc_media_player_get_time_(c->media_player);
}

static void vlcs_set_time(void *data, int64_t ms)
{
	struct vlc_source *c = data;

	libvlc_media_player_set_time_(c->media_player, (libvlc_time_t)ms);
}

static void vlcs_play_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct vlc_source *c = data;

	enum obs_media_state state = obs_source_media_get_state(c->source);

	if (pressed && obs_source_showing(c->source)) {
		if (state == OBS_MEDIA_STATE_PLAYING)
			obs_source_media_play_pause(c->source, true);
		else if (state == OBS_MEDIA_STATE_PAUSED)
			obs_source_media_play_pause(c->source, false);
	}
}

static void vlcs_restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct vlc_source *c = data;

	if (pressed && obs_source_showing(c->source))
		obs_source_media_restart(c->source);
}

static void vlcs_stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct vlc_source *c = data;

	if (pressed && obs_source_showing(c->source))
		obs_source_media_stop(c->source);
}

static void vlcs_playlist_next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct vlc_source *c = data;

	if (pressed && obs_source_showing(c->source))
		obs_source_media_next(c->source);
}

static void vlcs_playlist_prev_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct vlc_source *c = data;

	if (pressed && obs_source_showing(c->source))
		obs_source_media_previous(c->source);
}

static void *vlcs_create(obs_data_t *settings, obs_source_t *source)
{
	struct vlc_source *c = bzalloc(sizeof(*c));
	c->source = source;

	c->play_pause_hotkey = obs_hotkey_register_source(source, "VLCSource.PlayPause", obs_module_text("PlayPause"),
							  vlcs_play_pause_hotkey, c);

	c->restart_hotkey = obs_hotkey_register_source(source, "VLCSource.Restart", obs_module_text("Restart"),
						       vlcs_restart_hotkey, c);

	c->stop_hotkey =
		obs_hotkey_register_source(source, "VLCSource.Stop", obs_module_text("Stop"), vlcs_stop_hotkey, c);

	c->playlist_next_hotkey = obs_hotkey_register_source(
		source, "VLCSource.PlaylistNext", obs_module_text("PlaylistNext"), vlcs_playlist_next_hotkey, c);

	c->playlist_prev_hotkey = obs_hotkey_register_source(
		source, "VLCSource.PlaylistPrev", obs_module_text("PlaylistPrev"), vlcs_playlist_prev_hotkey, c);

	pthread_mutex_init_value(&c->mutex);
	if (pthread_mutex_init(&c->mutex, NULL) != 0)
		goto error;

	if (!load_libvlc())
		goto error;

	c->media_list_player = libvlc_media_list_player_new_(libvlc);
	if (!c->media_list_player)
		goto error;

	c->media_player = libvlc_media_player_new_(libvlc);
	if (!c->media_player)
		goto error;

	c->media_list = libvlc_media_list_new_(libvlc);
	if (!c->media_list)
		goto error;

	libvlc_media_list_player_set_media_player_(c->media_list_player, c->media_player);
	libvlc_media_list_player_set_media_list_(c->media_list_player, c->media_list);

	c->current_audio_track = 1;
	c->current_subtitle_track = 1;
	c->current_subtitle_enable = false;

	libvlc_video_set_callbacks_(c->media_player, vlcs_video_lock, NULL, vlcs_video_display, c);
	libvlc_video_set_format_callbacks_(c->media_player, vlcs_video_format, NULL);

	for (int i = 0; i < MAX_STREAM_OUTPUTS; i++) {
		spsc_ring_buffer_init(&c->pairing_session.stream_queues[i], MXF_AUDIO_FMT_S16, 96000);
		c->pairing_session.streams[i].magic = MXF_STREAM_MAGIC;
		c->pairing_session.streams[i].slot_id = i;
		c->pairing_session.streams[i].source = c;
		atomic_store(&c->pairing_session.streams[i].stream_epoch, 0);
		atomic_store(&c->pairing_session.streams[i].is_active, false);
		atomic_store(&c->pairing_session.streams[i].total_energy_accum, 0);
		atomic_store(&c->pairing_session.streams[i].total_samples_received, 0);
	}
	mxf_timeline_aligner_init(&c->pairing_session.aligner, NULL, NULL);
	mxf_timeline_aligner_set_queues(&c->pairing_session.aligner,
					&c->pairing_session.stream_queues[0],
					&c->pairing_session.stream_queues[1]);
	atomic_store(&c->pairing_session.left_slot, 0);
	atomic_store(&c->pairing_session.right_slot, 1);
	atomic_store(&c->pairing_session.routing_locked, false);
	atomic_store(&c->pairing_session.direct_stereo, false);
	c->pairing_session.initialized = true;

	if (os_event_init(&c->pairing_session.worker_event, OS_EVENT_TYPE_AUTO) == 0) {
		atomic_store(&c->pairing_session.worker_running, true);
		if (pthread_create(&c->pairing_session.worker_thread, NULL, pairing_audio_worker_thread, c) == 0) {
			atomic_store(&c->pairing_session.worker_created, true);
		}
	}

	libvlc_audio_set_callbacks_(c->media_player, vlcs_audio_play, NULL, NULL, vlcs_audio_flush, NULL, c);
	libvlc_audio_set_format_callbacks_(c->media_player, vlcs_audio_setup, vlcs_audio_cleanup);

	libvlc_event_manager_t *event_manager;
	event_manager = libvlc_media_player_event_manager_(c->media_player);
	libvlc_event_attach_(event_manager, libvlc_MediaPlayerEndReached, vlcs_stopped, c);
	libvlc_event_attach_(event_manager, libvlc_MediaPlayerOpening, vlcs_started, c);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void get_metadata(in string tag_id out string tag_data)", vlcs_get_metadata, c);

	vlcs_update(c, settings);

	return c;

error:
	vlcs_destroy(c);
	return NULL;
}

static void vlcs_activate(void *data)
{
	struct vlc_source *c = data;

	if (c->behavior == BEHAVIOR_STOP_RESTART) {
		libvlc_media_list_player_play_(c->media_list_player);

	} else if (c->behavior == BEHAVIOR_PAUSE_UNPAUSE) {
		libvlc_media_list_player_play_(c->media_list_player);
	}
}

static void vlcs_deactivate(void *data)
{
	struct vlc_source *c = data;

	if (c->behavior == BEHAVIOR_STOP_RESTART) {
		libvlc_media_list_player_stop_(c->media_list_player);
		obs_source_output_video(c->source, NULL);

	} else if (c->behavior == BEHAVIOR_PAUSE_UNPAUSE) {
		libvlc_media_list_player_pause_(c->media_list_player);
	}
}

static void vlcs_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_SHUFFLE, false);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_string(settings, S_BEHAVIOR, S_BEHAVIOR_STOP_RESTART);
	obs_data_set_default_int(settings, S_NETWORK_CACHING, 400);
	obs_data_set_default_int(settings, S_TRACK, 1);
	obs_data_set_default_int(settings, S_STEREO_PAIRING_MODE, PAIR_MODE_AUTO);
	obs_data_set_default_bool(settings, S_SUBTITLE_ENABLE, false);
	obs_data_set_default_int(settings, S_SUBTITLE_TRACK, 1);

	/* Custom Subtitles Defaults */
	obs_data_set_default_bool(settings, S_SUBTITLE_CUSTOM_ENABLE, false);

	obs_data_t *default_font = obs_data_create();
	obs_data_set_string(default_font, "face", "Segoe UI");
	obs_data_set_int(default_font, "size", 28);
	obs_data_set_int(default_font, "flags", OBS_FONT_BOLD);
	obs_data_set_default_obj(settings, S_SUBTITLE_FONT, default_font);
	obs_data_release(default_font);

	obs_data_set_default_int(settings, S_SUBTITLE_COLOR, 0xFFFFFF);         /* White */
	obs_data_set_default_bool(settings, S_SUBTITLE_OUTLINE_ENABLE, true);
	obs_data_set_default_int(settings, S_SUBTITLE_OUTLINE, 2);              /* Normal (2px) */
	obs_data_set_default_int(settings, S_SUBTITLE_OUTLINE_COLOR, 0x000000); /* Black */
	obs_data_set_default_bool(settings, S_SUBTITLE_SHADOW_ENABLE, false);
	obs_data_set_default_int(settings, S_SUBTITLE_SHADOW, 60);              /* 60% */
	obs_data_set_default_int(settings, S_SUBTITLE_SHADOW_COLOR, 0x000000);  /* Black */
	obs_data_set_default_int(settings, S_SUBTITLE_SHADOW_OPACITY, 100);     /* 100% */
	obs_data_set_default_bool(settings, S_SUBTITLE_BG_ENABLE, false);
	obs_data_set_default_int(settings, S_SUBTITLE_BG_COLOR, 0x000000);      /* Black */
	obs_data_set_default_int(settings, S_SUBTITLE_BG_OPACITY, 60);          /* 60% */
	obs_data_set_default_int(settings, S_SUBTITLE_MARGIN, 40);              /* 40 px */
	obs_data_set_default_bool(settings, S_SUBTITLE_OVERRIDE_ASS, true);
}

static bool sub_custom_enable_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	bool sub_enable = obs_data_get_bool(settings, S_SUBTITLE_ENABLE);
	bool custom_enable = obs_data_get_bool(settings, S_SUBTITLE_CUSTOM_ENABLE) && sub_enable;
	bool outline_enable = obs_data_get_bool(settings, S_SUBTITLE_OUTLINE_ENABLE) && custom_enable;
	bool shadow_enable = obs_data_get_bool(settings, S_SUBTITLE_SHADOW_ENABLE) && custom_enable;
	bool bg_enable = obs_data_get_bool(settings, S_SUBTITLE_BG_ENABLE) && custom_enable;

	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_TRACK), sub_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_CUSTOM_ENABLE), sub_enable);

	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_FONT), custom_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_COLOR), custom_enable);

	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_OUTLINE_ENABLE), custom_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_OUTLINE), outline_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_OUTLINE_COLOR), outline_enable);

	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_SHADOW_ENABLE), custom_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_SHADOW), shadow_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_SHADOW_COLOR), shadow_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_SHADOW_OPACITY), shadow_enable);

	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_BG_ENABLE), custom_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_BG_COLOR), bg_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_BG_OPACITY), bg_enable);

	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_MARGIN), custom_enable);
	obs_property_set_visible(obs_properties_get(props, S_SUBTITLE_OVERRIDE_ASS), custom_enable);

	UNUSED_PARAMETER(p);
	return true;
}

static obs_properties_t *vlcs_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();
	struct vlc_source *c = data;
	struct dstr filter = {0};
	struct dstr exts = {0};
	struct dstr path = {0};
	obs_property_t *p;

	obs_properties_set_flags(ppts, OBS_PROPERTIES_DEFER_UPDATE);
	obs_properties_add_bool(ppts, S_LOOP, T_LOOP);
	obs_properties_add_bool(ppts, S_SHUFFLE, T_SHUFFLE);

	if (c) {
		pthread_mutex_lock(&c->mutex);
		if (c->files.num) {
			struct media_file_data *last = da_end(c->files);
			const char *slash;

			dstr_copy(&path, last->path);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&c->mutex);
	}

	p = obs_properties_add_list(ppts, S_BEHAVIOR, T_BEHAVIOR, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_BEHAVIOR_STOP_RESTART, S_BEHAVIOR_STOP_RESTART);
	obs_property_list_add_string(p, T_BEHAVIOR_PAUSE_UNPAUSE, S_BEHAVIOR_PAUSE_UNPAUSE);
	obs_property_list_add_string(p, T_BEHAVIOR_ALWAYS_PLAY, S_BEHAVIOR_ALWAYS_PLAY);

	dstr_cat(&filter, "Media Files (");
	dstr_copy(&exts, EXTENSIONS_MEDIA);
	dstr_replace(&exts, ";", " ");
	dstr_cat_dstr(&filter, &exts);

	dstr_cat(&filter, ");;Video Files (");
	dstr_copy(&exts, EXTENSIONS_VIDEO);
	dstr_replace(&exts, ";", " ");
	dstr_cat_dstr(&filter, &exts);

	dstr_cat(&filter, ");;Audio Files (");
	dstr_copy(&exts, EXTENSIONS_AUDIO);
	dstr_replace(&exts, ";", " ");
	dstr_cat_dstr(&filter, &exts);

	dstr_cat(&filter, ");;Playlist Files (");
	dstr_copy(&exts, EXTENSIONS_PLAYLIST);
	dstr_replace(&exts, ";", " ");
	dstr_cat_dstr(&filter, &exts);
	dstr_cat(&filter, ")");

	obs_properties_add_editable_list(ppts, S_PLAYLIST, T_PLAYLIST, OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS,
					 filter.array, path.array);
	dstr_free(&path);
	dstr_free(&filter);
	dstr_free(&exts);

	p = obs_properties_add_int(ppts, S_NETWORK_CACHING, T_NETWORK_CACHING, 100, 60000, 10);
	obs_property_int_set_suffix(p, " ms");

	obs_properties_add_int(ppts, S_TRACK, T_TRACK, 1, 10, 1);

	obs_property_t *p_pair = obs_properties_add_list(ppts, S_STEREO_PAIRING_MODE,
							 T_STEREO_PAIRING_MODE,
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_pair, T_STEREO_PAIRING_AUTO, PAIR_MODE_AUTO);
	obs_property_list_add_int(p_pair, T_STEREO_PAIRING_PASSTHROUGH, PAIR_MODE_PASSTHROUGH);
	obs_property_list_add_int(p_pair, T_STEREO_PAIRING_1_2, PAIR_MODE_MANUAL_1_2);
	obs_property_list_add_int(p_pair, T_STEREO_PAIRING_1_3, PAIR_MODE_MANUAL_1_3);
	obs_property_list_add_int(p_pair, T_STEREO_PAIRING_3_4, PAIR_MODE_MANUAL_3_4);

	p = obs_properties_add_bool(ppts, S_SUBTITLE_ENABLE, T_SUBTITLE_ENABLE);
	obs_property_set_modified_callback(p, sub_custom_enable_modified);

	obs_properties_add_int(ppts, S_SUBTITLE_TRACK, T_SUBTITLE_TRACK, 1, 1000, 1);

	p = obs_properties_add_bool(ppts, S_SUBTITLE_CUSTOM_ENABLE, T_SUBTITLE_CUSTOM_ENABLE);
	obs_property_set_modified_callback(p, sub_custom_enable_modified);

	obs_properties_add_font(ppts, S_SUBTITLE_FONT, T_SUBTITLE_FONT);
	obs_properties_add_color(ppts, S_SUBTITLE_COLOR, T_SUBTITLE_COLOR);

	p = obs_properties_add_bool(ppts, S_SUBTITLE_OUTLINE_ENABLE, T_SUBTITLE_OUTLINE_ENABLE);
	obs_property_set_modified_callback(p, sub_custom_enable_modified);

	obs_property_t *p_outline = obs_properties_add_list(ppts, S_SUBTITLE_OUTLINE, T_SUBTITLE_OUTLINE,
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_outline, T_SUBTITLE_OUTLINE_THIN, 1);
	obs_property_list_add_int(p_outline, T_SUBTITLE_OUTLINE_NORMAL, 2);
	obs_property_list_add_int(p_outline, T_SUBTITLE_OUTLINE_THICK, 3);

	obs_properties_add_color(ppts, S_SUBTITLE_OUTLINE_COLOR, T_SUBTITLE_OUTLINE_COLOR);

	p = obs_properties_add_bool(ppts, S_SUBTITLE_SHADOW_ENABLE, T_SUBTITLE_SHADOW_ENABLE);
	obs_property_set_modified_callback(p, sub_custom_enable_modified);

	obs_property_t *p_shadow = obs_properties_add_int_slider(ppts, S_SUBTITLE_SHADOW, T_SUBTITLE_SHADOW, 10, 100, 5);
	obs_property_int_set_suffix(p_shadow, " %");

	obs_properties_add_color(ppts, S_SUBTITLE_SHADOW_COLOR, T_SUBTITLE_SHADOW_COLOR);
	obs_property_t *p_shadow_op = obs_properties_add_int_slider(ppts, S_SUBTITLE_SHADOW_OPACITY, T_SUBTITLE_SHADOW_OPACITY, 0, 100, 5);
	obs_property_int_set_suffix(p_shadow_op, " %");

	p = obs_properties_add_bool(ppts, S_SUBTITLE_BG_ENABLE, T_SUBTITLE_BG_ENABLE);
	obs_property_set_modified_callback(p, sub_custom_enable_modified);

	obs_properties_add_color(ppts, S_SUBTITLE_BG_COLOR, T_SUBTITLE_BG_COLOR);
	obs_property_t *p_opacity = obs_properties_add_int_slider(ppts, S_SUBTITLE_BG_OPACITY, T_SUBTITLE_BG_OPACITY, 0, 100, 5);
	obs_property_int_set_suffix(p_opacity, " %");

	obs_property_t *p_margin = obs_properties_add_int_slider(ppts, S_SUBTITLE_MARGIN, T_SUBTITLE_MARGIN, 0, 300, 5);
	obs_property_int_set_suffix(p_margin, " px");

	obs_properties_add_bool(ppts, S_SUBTITLE_OVERRIDE_ASS, T_SUBTITLE_OVERRIDE_ASS);

	obs_properties_add_bool(ppts, S_HW_DECODE, T_HW_DECODE);

	return ppts;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct vlc_source *s = src;
	const char *orig_path = data;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *file = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(file, "value");

		if (strcmp(path, orig_path) == 0) {
			if (new_path && *new_path)
				obs_data_set_string(file, "value", new_path);
			else
				obs_data_array_erase(files, i);

			obs_data_release(file);
			break;
		}

		obs_data_release(file);
	}

	obs_source_update(source, settings);

	obs_data_array_release(files);
	obs_data_release(settings);
}

static obs_missing_files_t *vlcs_missingfiles(void *data)
{
	struct vlc_source *s = data;
	obs_missing_files_t *missing_files = obs_missing_files_create();

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_array_t *files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t *item = obs_data_array_item(files, i);
		const char *path = obs_data_get_string(item, "value");

		if (strcmp(path, "") != 0) {
			if (!os_file_exists(path) && strstr(path, "://") == NULL) {
				obs_missing_file_t *file = obs_missing_file_create(
					path, missing_file_callback, OBS_MISSING_FILE_SOURCE, source, (void *)path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}

struct obs_source_info vlc_source_info = {
	.id = "vlc_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = vlcs_get_name,
	.create = vlcs_create,
	.destroy = vlcs_destroy,
	.update = vlcs_update,
	.get_defaults = vlcs_defaults,
	.get_properties = vlcs_properties,
	.activate = vlcs_activate,
	.deactivate = vlcs_deactivate,
	.missing_files = vlcs_missingfiles,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = vlcs_play_pause,
	.media_restart = vlcs_restart,
	.media_stop = vlcs_stop,
	.media_next = vlcs_playlist_next,
	.media_previous = vlcs_playlist_prev,
	.media_get_duration = vlcs_get_duration,
	.media_get_time = vlcs_get_time,
	.media_set_time = vlcs_set_time,
	.media_get_state = vlcs_get_state,
};
