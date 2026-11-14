#include "fx_ui.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_config.h"

#define CONTRIBUTION_ID "settings"

typedef struct option { const char *title; int value; } option;

static const option k_presets[] = {
    {"Custom", FX_PRESET_CUSTOM}, {"CRT TV", FX_PRESET_CRT}, {"Arcade", FX_PRESET_ARCADE},
    {"Night", FX_PRESET_NIGHT}, {"Cinema 21:9", FX_PRESET_CINEMA},
    {"Green phosphor", FX_PRESET_PHOSPHOR}};
static const option k_strength[] = {
    {"15%", 15}, {"30%", 30}, {"45%", 45}, {"60%", 60}, {"75%", 75}, {"90%", 90}};
static const option k_pitch[] = {
    {"Fine (2 px)", 2}, {"Medium (3 px)", 3}, {"Classic (4 px)", 4}, {"Chunky (6 px)", 6},
    {"Huge (8 px)", 8}};
static const option k_tints[] = {
    {"Off", FX_TINT_NONE}, {"Warm", FX_TINT_WARM}, {"Night (blue light cut)", FX_TINT_NIGHT},
    {"Green phosphor", FX_TINT_GREEN}, {"Amber", FX_TINT_AMBER}};
static const option k_bars[] = {
    {"Off", FX_BARS_OFF}, {"Cinema 21:9", FX_BARS_21_9}, {"Retro 4:3", FX_BARS_4_3}};
static const option k_dim[] = {
    {"Off", 0}, {"10%", 10}, {"20%", 20}, {"35%", 35}, {"50%", 50}, {"65%", 65}, {"80%", 80}};
static const option k_combos[] = {
    {"Off", FX_COMBO_OFF}, {"L3 + R3", FX_COMBO_L3_R3},
    {"L1 + R1 + Options", FX_COMBO_L1_R1_OPTIONS},
    {"L2 + R2 + Touchpad", FX_COMBO_L2_R2_TOUCHPAD}};

typedef enum field_kind { FIELD_TOGGLE, FIELD_LIST } field_kind;

typedef struct field {
    const char *id;
    const char *title;
    const char *description;
    field_kind kind;
    size_t offset;
    const option *options;
    size_t option_count;
} field;

#define OPTS(a) a, sizeof(a) / sizeof(a[0])
#define OFF(name) offsetof(fx_settings, name)
static const field k_fields[] = {
    {"renderer", "Draw on screen (experimental)",
     "Loads the ScreenFX drawing module into the system UI. Turns itself off if the UI restarts right after loading.",
     FIELD_TOGGLE, OFF(renderer), NULL, 0},
    {"enabled", "Effects enabled", "Master switch. Also in the in-game menu (Off) and on a held combo.",
     FIELD_TOGGLE, OFF(enabled), NULL, 0},
    {"preset", "Preset", "Loads a ready-made look. Changing any effect below switches to Custom.",
     FIELD_LIST, OFF(preset), OPTS(k_presets)},
    {"scanlines", "Scanlines", "Dark horizontal lines like a CRT.", FIELD_TOGGLE,
     OFF(scanlines), NULL, 0},
    {"scan_strength", "Scanline strength", "How dark the lines are.", FIELD_LIST,
     OFF(scan_strength), OPTS(k_strength)},
    {"scan_pitch", "Scanline size", "Distance between lines.", FIELD_LIST, OFF(scan_pitch),
     OPTS(k_pitch)},
    {"vignette", "Vignette", "Darkens the corners of the picture.", FIELD_TOGGLE,
     OFF(vignette), NULL, 0},
    {"vig_strength", "Vignette strength", "How dark the corners get.", FIELD_LIST,
     OFF(vig_strength), OPTS(k_strength)},
    {"tint", "Colour tint", "A colour wash over the whole picture.", FIELD_LIST, OFF(tint),
     OPTS(k_tints)},
    {"tint_strength", "Tint strength", "How strong the colour wash is.", FIELD_LIST,
     OFF(tint_strength), OPTS(k_strength)},
    {"bars", "Black bars", "Covers the edges to fake another aspect ratio.", FIELD_LIST,
     OFF(bars), OPTS(k_bars)},
    {"dim", "Dim picture", "Darkens everything, for late-night play.", FIELD_LIST, OFF(dim),
     OPTS(k_dim)},
    {"combo", "Button combo",
     "Press in game to open the ScreenFX menu. Hold for one second to switch effects on or off.",
     FIELD_LIST, OFF(combo), OPTS(k_combos)},
    {"notify", "Show notifications", "Popup when the combo changes something.", FIELD_TOGGLE,
     OFF(notify), NULL, 0},
};
#define FIELD_COUNT (sizeof(k_fields) / sizeof(k_fields[0]))

static int get_field(const fx_settings *s, const field *f) {
    return *(const int *)((const char *)s + f->offset);
}

static void format_value(const field *f, int value, char *out, size_t out_size) {
    if (f->kind == FIELD_TOGGLE) snprintf(out, out_size, "%s", value ? "true" : "false");
    else snprintf(out, out_size, "%d", value);
}

/* Lists only show built-in steps; snap hand-edited INI values to the nearest one. */
static int nearest_option(const field *f, int value) {
    int best = f->options[0].value;
    for (size_t i = 1; i < f->option_count; ++i) {
        if (abs(f->options[i].value - value) < abs(best - value)) best = f->options[i].value;
    }
    return best;
}

static onion_ui_node_desc_v1 make_node(uint32_t kind, const char *id, const char *parent_id,
                                       const char *title) {
    onion_ui_node_desc_v1 node;
    memset(&node, 0, sizeof(node));
    node.struct_size = sizeof(node);
    node.abi_version = ONION_UI_ABI_VERSION;
    node.kind = kind;
    snprintf(node.id, sizeof(node.id), "%s", id);
    snprintf(node.parent_id, sizeof(node.parent_id), "%s", parent_id ? parent_id : "");
    snprintf(node.title, sizeof(node.title), "%s", title);
    return node;
}

static onion_status add_nodes(onion_ui_document *document, const fx_settings *s) {
    onion_ui_node_desc_v1 node = make_node(ONION_UI_NODE_PAGE, "main", NULL, PLUGIN_NAME);
    onion_status status = onion_ui_document_add_node(document, &node);

    for (size_t i = 0; status == ONION_OK && i < FIELD_COUNT; ++i) {
        const field *f = &k_fields[i];
        const int is_toggle = f->kind == FIELD_TOGGLE;
        const int value = is_toggle ? get_field(s, f) : nearest_option(f, get_field(s, f));

        node = make_node(is_toggle ? ONION_UI_NODE_TOGGLE : ONION_UI_NODE_LIST, f->id, "main",
                         f->title);
        node.value_type = is_toggle ? ONION_UI_VALUE_BOOL : ONION_UI_VALUE_STRING;
        node.binding = ONION_UI_BINDING_EVENT;
        snprintf(node.binding_key, sizeof(node.binding_key), "%s_changed", f->id);
        snprintf(node.description, sizeof(node.description), "%s", f->description);
        format_value(f, value, node.value, sizeof(node.value));
        status = onion_ui_document_add_node(document, &node);

        for (size_t o = 0; status == ONION_OK && o < f->option_count; ++o) {
            char item_id[ONION_UI_NODE_ID_MAX];
            snprintf(item_id, sizeof(item_id), "%s_%d", f->id, f->options[o].value);
            node = make_node(ONION_UI_NODE_LIST_ITEM, item_id, f->id, f->options[o].title);
            node.value_type = ONION_UI_VALUE_STRING;
            snprintf(node.value, sizeof(node.value), "%d", f->options[o].value);
            status = onion_ui_document_add_node(document, &node);
        }
    }
    return status;
}

onion_status fx_ui_create(const fx_settings *s, onion_ui_document **out_document) {
    if (!s || !out_document) return ONION_E_INVALID_ARGUMENT;

    onion_ui_document_desc_v1 desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.abi_version = ONION_UI_ABI_VERSION;
    desc.priority = 100;
    snprintf(desc.plugin_id, sizeof(desc.plugin_id), "%s", PLUGIN_ID);
    snprintf(desc.contribution_id, sizeof(desc.contribution_id), "%s", CONTRIBUTION_ID);
    snprintf(desc.title, sizeof(desc.title), "%s", PLUGIN_NAME);
    snprintf(desc.description, sizeof(desc.description),
             "Scanlines, vignette, tint and black bars over any game.");
    snprintf(desc.root_page_id, sizeof(desc.root_page_id), "main");

    onion_status status = onion_ui_document_create(&desc, out_document);
    if (status != ONION_OK) return status;
    status = add_nodes(*out_document, s);
    if (status == ONION_OK) status = onion_ui_document_validate(*out_document);
    if (status != ONION_OK) {
        onion_ui_document_destroy(*out_document);
        *out_document = NULL;
    }
    return status;
}

onion_status fx_ui_apply_event(onion_ui_handle handle, const onion_ui_event_v1 *event,
                               fx_settings *s) {
    if (handle == 0 || !event || !s) return ONION_E_INVALID_ARGUMENT;
    if (event->handle != handle || strcmp(event->contribution_id, CONTRIBUTION_ID) != 0) {
        return ONION_E_NOT_FOUND;
    }

    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        const field *f = &k_fields[i];
        if (strcmp(event->node_id, f->id) != 0) continue;

        int value;
        if (f->kind == FIELD_TOGGLE) {
            value = strcmp(event->value, "true") == 0 || strcmp(event->value, "1") == 0;
        } else {
            char *end = NULL;
            const long parsed = strtol(event->value, &end, 10);
            if (end == event->value || *end != '\0') return ONION_E_INVALID_ARGUMENT;
            value = (int)parsed;
        }

        if (f->offset == OFF(preset)) {
            fx_settings_apply_preset(s, value);
        } else {
            *(int *)((char *)s + f->offset) = value;
            /* Editing an effect by hand leaves the preset; combo/notify/master do not. */
            if (f->offset != OFF(enabled) && f->offset != OFF(combo) &&
                f->offset != OFF(notify) && f->offset != OFF(renderer)) {
                s->preset = FX_PRESET_CUSTOM;
                fx_settings_remember_custom(s);
            }
        }
        fx_settings_clamp(s);
        return ONION_OK;
    }
    return ONION_E_NOT_FOUND;
}

void fx_ui_sync(const onion_host_services_v1 *services, onion_ui_handle handle,
                const fx_settings *s) {
    if (!services || handle == 0 || !s) return;
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        const field *f = &k_fields[i];
        const int is_toggle = f->kind == FIELD_TOGGLE;
        char value[32];
        format_value(f, is_toggle ? get_field(s, f) : nearest_option(f, get_field(s, f)), value,
                     sizeof(value));
        (void)onion_ui_set_value(services, handle, f->id,
                                 is_toggle ? ONION_UI_VALUE_BOOL : ONION_UI_VALUE_STRING, value);
    }
}
