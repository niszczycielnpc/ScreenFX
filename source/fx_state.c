#include "fx_state.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plugin_config.h"

static uint32_t g_sequence;

static uint8_t pct_to_alpha(int pct) { return (uint8_t)((pct * 255 + 50) / 100); }

int fx_state_prepare(void) {
    (void)mkdir(FX_SHARED_DIR, 0777);
    (void)remove(FX_REQUEST_PATH);
    struct stat st;
    return (stat(FX_SHARED_DIR, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

int fx_state_publish(const fx_settings *s) {
    /* Tint colours are multiplied by tint_strength into the alpha channel. */
    static const uint8_t tints[FX_TINT_COUNT][3] = {
        {0, 0, 0}, {255, 150, 40}, {255, 90, 0}, {40, 255, 90}, {255, 176, 0}};

    fx_state_v1 st;
    memset(&st, 0, sizeof(st));
    st.magic = FX_STATE_MAGIC;
    st.version = FX_STATE_VERSION;
    st.size = sizeof(st);
    st.sequence = ++g_sequence;
    st.enabled = (uint8_t)s->enabled;
    st.scanlines = (uint8_t)s->scanlines;
    st.scan_alpha = pct_to_alpha(s->scan_strength);
    st.scan_pitch = (uint8_t)s->scan_pitch;
    st.vignette = (uint8_t)s->vignette;
    st.vig_alpha = pct_to_alpha(s->vig_strength);
    st.bars = (uint8_t)s->bars;
    st.dim_alpha = pct_to_alpha(s->dim);
    if (s->tint != FX_TINT_NONE) {
        memcpy(st.tint_rgba, tints[s->tint], 3);
        /* A full-screen colour wash above ~40 % hides the game; cap it there. */
        st.tint_rgba[3] = pct_to_alpha(s->tint_strength * 40 / 100);
    }
    st.combo = (uint8_t)s->combo;

    FILE *f = fopen(FX_STATE_PATH ".tmp", "wb");
    if (!f) return -1;
    size_t written = fwrite(&st, 1, sizeof(st), f);
    if (fclose(f) != 0 || written != sizeof(st)) { remove(FX_STATE_PATH ".tmp"); return -1; }
    if (rename(FX_STATE_PATH ".tmp", FX_STATE_PATH) != 0) {
        remove(FX_STATE_PATH ".tmp");
        return -1;
    }
    return 0;
}

fx_request fx_state_take_request(void) {
    FILE *f = fopen(FX_REQUEST_PATH, "r");
    if (!f) return FX_REQUEST_NONE;
    char word[32] = {0};
    if (!fgets(word, sizeof(word), f)) word[0] = '\0';
    fclose(f);
    (void)remove(FX_REQUEST_PATH);
    word[strcspn(word, "\r\n ")] = '\0';
    if (strcmp(word, "toggle") == 0) return FX_REQUEST_TOGGLE;
    if (strcmp(word, "next_preset") == 0) return FX_REQUEST_NEXT_PRESET;
    return FX_REQUEST_NONE;
}
