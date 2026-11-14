#pragma once

#include <stdint.h>

#include "fx_settings.h"

#define FX_STATE_MAGIC 0x58464353u /* "SCFX" */
#define FX_STATE_VERSION 1u

/* Fixed little-endian record read by the renderer. Only ever grows at the tail. */
typedef struct fx_state_v1 {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t sequence;       /* bumped on every publish */
    uint8_t enabled;
    uint8_t scanlines;
    uint8_t scan_alpha;      /* 0..255 */
    uint8_t scan_pitch;      /* px at 1080p */
    uint8_t vignette;
    uint8_t vig_alpha;       /* 0..255 */
    uint8_t bars;            /* FX_BARS_* */
    uint8_t dim_alpha;       /* 0..255 */
    uint8_t tint_rgba[4];    /* a == 0 means no tint */
    uint8_t combo;           /* FX_COMBO_* */
    uint8_t preset;          /* FX_PRESET_*, for the in-game menu highlight */
    uint8_t has_custom;      /* show "Custom" in the in-game menu */
    uint8_t reserved;
} fx_state_v1;

/* onion_ready marker holding the pid of the ShellUI instance that has a renderer. */
#define FX_READY_NAME "screenfx_renderer_v2"

typedef enum fx_request {
    FX_REQUEST_NONE = 0,
    FX_REQUEST_TOGGLE,
    FX_REQUEST_NEXT_PRESET,
    FX_REQUEST_SET          /* "set N": N = FX_PRESET_*, or -1 for effects off */
} fx_request;

int fx_state_prepare(void);
int fx_state_publish(const fx_settings *s);
/* Consumes the request file written by the renderer (combo or in-game menu).
 * *value is the argument of FX_REQUEST_SET. */
fx_request fx_state_take_request(int *value);
