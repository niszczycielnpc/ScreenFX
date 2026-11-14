#pragma once

#include <stdint.h>

enum { FX_TINT_NONE, FX_TINT_WARM, FX_TINT_NIGHT, FX_TINT_GREEN, FX_TINT_AMBER, FX_TINT_COUNT };
enum { FX_BARS_OFF, FX_BARS_21_9, FX_BARS_4_3, FX_BARS_COUNT };
enum { FX_COMBO_OFF, FX_COMBO_L3_R3, FX_COMBO_L1_R1_OPTIONS, FX_COMBO_L2_R2_TOUCHPAD, FX_COMBO_COUNT };
enum { FX_PRESET_CUSTOM, FX_PRESET_CRT, FX_PRESET_ARCADE, FX_PRESET_NIGHT, FX_PRESET_CINEMA,
       FX_PRESET_PHOSPHOR, FX_PRESET_COUNT };

/* Menu/notification names, indexed by FX_PRESET_*. Shared with the renderer. */
#define FX_PRESET_NAMES \
    {"Custom", "CRT TV", "Arcade", "Night", "Cinema 21:9", "Green phosphor"}

/* The effect fields scanlines..dim are contiguous ints; presets rewrite exactly these. */
#define FX_LOOK_FIELDS 9

typedef struct fx_settings {
    int enabled;        /* master switch */
    int preset;         /* FX_PRESET_* */
    int scanlines;      /* 0/1 */
    int scan_strength;  /* 0..100 % darkness of the dark line */
    int scan_pitch;     /* 2..8 px, period of one bright+dark pair at 1080p */
    int vignette;       /* 0/1 */
    int vig_strength;   /* 0..100 */
    int tint;           /* FX_TINT_* */
    int tint_strength;  /* 0..100 */
    int bars;           /* FX_BARS_* */
    int dim;            /* 0..80 % */
    int combo;          /* FX_COMBO_* */
    int notify;         /* popup when the combo changes something */
    int renderer;       /* load the ShellUI drawing module (experimental) */
    int has_custom;     /* custom[] holds a hand-made look */
    int custom[FX_LOOK_FIELDS]; /* snapshot of scanlines..dim behind the Custom preset */
} fx_settings;

void fx_settings_defaults(fx_settings *s);
void fx_settings_clamp(fx_settings *s);
/* Overwrites the effect fields with a built-in preset (or the saved Custom
 * look); keeps combo/notify. */
void fx_settings_apply_preset(fx_settings *s, int preset);
const char *fx_preset_name(int preset);
/* Call after a hand edit: the current look becomes what Custom restores. */
void fx_settings_remember_custom(fx_settings *s);
int fx_settings_load(const char *path, fx_settings *s);
int fx_settings_save(const char *path, const fx_settings *s);
