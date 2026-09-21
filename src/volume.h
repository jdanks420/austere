#ifndef AUSTERE_VOLUME_H
#define AUSTERE_VOLUME_H

/* Volume keybind actions. Shell out to pactl, falling back to amixer
 * (§6.2 policy shared with bar_modules/volume.c) — no uniform mixer
 * ABI exists without libasound. */
void volume_shift(int delta_percent);
void volume_toggle_mute(void);

#endif
