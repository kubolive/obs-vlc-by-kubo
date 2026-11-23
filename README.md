# obs-vlc-by-kubo

An enhanced mod of OBS Studio's built-in `vlc-video` (VLC Video Source) plugin, tailored for broadcast and playout workflows.

> **⚠️ Warning:** This plugin overwrites OBS Studio's default `vlc-video` plugin. Back up your original plugin file before replacing it.

## Key Changes

* **Optimized MXF Playback & Audio Pairing:**
  * Treats MXF files as streams rather than scanning the entire file before playback, eliminating startup delays on large or growing broadcast files.
  * Automatically pairs discrete mono audio tracks into stereo channels (Ch 1-2, 3-4, etc.).
* **Hardware Decoding:** Added GPU hardware-accelerated video decoding support.
* **Custom Subtitles:** Customize font, text color, outline, drop shadow, background box, bottom margin, and override embedded ASS/SSA styles.
* **Non-Disruptive Controls & Playlist:**
  * Switch Audio or Subtitle tracks on the fly without reloading playback.
  * Add or remove playlist items without interrupting the active stream.
  * Confirmation prompt when attempting to remove a currently playing file.
