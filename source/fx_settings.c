#include "fx_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void fx_settings_defaults(fx_settings *s) {
    memset(s, 0, sizeof(*s));
    s->enabled = 1;
    s->combo = FX_COMBO_L3_R3;
    s->notify = 1;
    fx_settings_apply_preset(s, FX_PRESET_CRT);
}

void fx_settings_clamp(fx_settings *s) {
    s->enabled = !!s->enabled;
    s->preset = clampi(s->preset, 0, FX_PRESET_COUNT - 1);
    s->scanlines = !!s->scanlines;
    s->scan_strength = clampi(s->scan_strength, 0, 100);
    s->scan_pitch = clampi(s->scan_pitch, 2, 8);
    s->vignette = !!s->vignette;
    s->vig_strength = clampi(s->vig_strength, 0, 100);
    s->tint = clampi(s->tint, 0, FX_TINT_COUNT - 1);
    s->tint_strength = clampi(s->tint_strength, 0, 100);
    s->bars = clampi(s->bars, 0, FX_BARS_COUNT - 1);
    s->dim = clampi(s->dim, 0, 80);
    s->combo = clampi(s->combo, 0, FX_COMBO_COUNT - 1);
    s->notify = !!s->notify;
    s->renderer = !!s->renderer;
    s->has_custom = !!s->has_custom;
}

const char *fx_preset_name(int preset) {
    static const char *const names[FX_PRESET_COUNT] = FX_PRESET_NAMES;
    return names[clampi(preset, 0, FX_PRESET_COUNT - 1)];
}

void fx_settings_apply_preset(fx_settings *s, int preset) {
    s->preset = clampi(preset, 0, FX_PRESET_COUNT - 1);
    if (s->preset == FX_PRESET_CUSTOM) {
        if (s->has_custom) memcpy(&s->scanlines, s->custom, sizeof(s->custom));
        return;
    }

    s->scanlines = 0; s->scan_strength = 45; s->scan_pitch = 4;
    s->vignette = 0;  s->vig_strength = 30;
    s->tint = FX_TINT_NONE; s->tint_strength = 30;
    s->bars = FX_BARS_OFF; s->dim = 0;

    switch (s->preset) {
    case FX_PRESET_CRT:
        s->scanlines = 1; s->vignette = 1;
        break;
    case FX_PRESET_ARCADE:
        s->scanlines = 1; s->scan_strength = 60; s->scan_pitch = 3;
        s->vignette = 1; s->vig_strength = 20;
        break;
    case FX_PRESET_NIGHT:
        s->tint = FX_TINT_NIGHT; s->tint_strength = 35; s->dim = 20;
        break;
    case FX_PRESET_CINEMA:
        s->bars = FX_BARS_21_9; s->vignette = 1; s->vig_strength = 25;
        break;
    case FX_PRESET_PHOSPHOR:
        s->scanlines = 1; s->scan_strength = 30;
        s->tint = FX_TINT_GREEN; s->tint_strength = 40;
        break;
    default:
        break;
    }
}

void fx_settings_remember_custom(fx_settings *s) {
    memcpy(s->custom, &s->scanlines, sizeof(s->custom));
    s->has_custom = 1;
}

_Static_assert(offsetof(fx_settings, dim) - offsetof(fx_settings, scanlines) ==
                   (FX_LOOK_FIELDS - 1) * sizeof(int),
               "look fields must stay contiguous");

static const struct { const char *key; size_t offset; } k_fields[] = {
#define F(name) {#name, offsetof(fx_settings, name)}
    F(enabled), F(preset), F(scanlines), F(scan_strength), F(scan_pitch), F(vignette),
    F(vig_strength), F(tint), F(tint_strength), F(bars), F(dim), F(combo), F(notify), F(renderer), F(has_custom),
#undef F
};

int fx_settings_load(const char *path, fx_settings *s) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        *eq = '\0';
        int custom_index = -1;
        if (sscanf(line, "custom%d", &custom_index) == 1 && custom_index >= 0 &&
            custom_index < FX_LOOK_FIELDS) {
            s->custom[custom_index] = atoi(eq + 1);
            continue;
        }
        for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); ++i) {
            if (strcmp(line, k_fields[i].key) == 0) {
                *(int *)((char *)s + k_fields[i].offset) = atoi(eq + 1);
                break;
            }
        }
    }
    fclose(f);
    fx_settings_clamp(s);
    /* Settings saved before 0.30 have a Custom look but no snapshot of it. */
    if (s->preset == FX_PRESET_CUSTOM && !s->has_custom) fx_settings_remember_custom(s);
    return 0;
}

int fx_settings_save(const char *path, const fx_settings *s) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); ++i) {
        fprintf(f, "%s=%d\n", k_fields[i].key,
                *(const int *)((const char *)s + k_fields[i].offset));
    }
    for (int i = 0; i < FX_LOOK_FIELDS; ++i) fprintf(f, "custom%d=%d\n", i, s->custom[i]);
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
