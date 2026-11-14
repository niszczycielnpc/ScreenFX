/*
 * ScreenFX renderer - runs inside SceShellUI.
 *
 * Loaded by the ScreenFX plugin with the same remote ELF loader OnionHEN uses
 * for its Toolbox. Draws the look published in state.bin as PUI panels on the
 * "Game" scene (the layer OnionHEN's monitor bar lives on), watches the pad
 * for the button combo and shows the in-game preset menu.
 *
 * All Mono/PUI work happens on the ShellUI UI thread. The only per-frame entry
 * point known on every firmware is PUI Application.Update, which OnionHEN
 * already detours; this module chains onto that detour (it swaps the jump
 * target and calls the previous one first) instead of patching the code again.
 */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <onion/proc.h>
#include <onion/ready.h>
#include <onion/ucred.h>
#include <ps5/kernel.h>
}

#include "fx_state.h"

namespace {

/* ShellUI is sandboxed: the plugin's /data/OnionHEN is /user/data/OnionHEN here. */
const char *const kDirCandidates[] = {"/user/data/OnionHEN/screenfx", "/data/OnionHEN/screenfx"};
char g_log_path[96], g_state_path[96], g_request_path[96], g_request_tmp_path[96];

bool pick_directory() {
  for (const char *dir : kDirCandidates) {
    char probe[96];
    snprintf(probe, sizeof(probe), "%s/state.bin", dir);
    const int fd = open(probe, O_RDONLY);
    if (fd < 0) continue;
    close(fd);
    snprintf(g_log_path, sizeof(g_log_path), "%s/renderer.log", dir);
    snprintf(g_state_path, sizeof(g_state_path), "%s/state.bin", dir);
    snprintf(g_request_path, sizeof(g_request_path), "%s/request", dir);
    snprintf(g_request_tmp_path, sizeof(g_request_tmp_path), "%s/request.tmp", dir);
    return true;
  }
  return false;
}


/* ---- Mono, resolved from the libmonosgen already loaded in ShellUI ---- */
using MonoObject = void;
using MonoClass = void;
using MonoImage = void;
using MonoMethod = void;
using MonoDomain = void;

MonoDomain *(*mono_get_root_domain)();
void *(*mono_thread_attach)(MonoDomain *);
void *(*mono_domain_assembly_open)(MonoDomain *, const char *);
MonoImage *(*mono_assembly_get_image)(void *);
MonoClass *(*mono_class_from_name)(MonoImage *, const char *, const char *);
MonoMethod *(*mono_class_get_method_from_name)(MonoClass *, const char *, int);
void *(*mono_class_get_property_from_name)(MonoClass *, const char *);
MonoMethod *(*mono_property_get_get_method)(void *);
MonoMethod *(*mono_property_get_set_method)(void *);
MonoObject *(*mono_runtime_invoke)(MonoMethod *, void *, void **, MonoObject **);
MonoObject *(*mono_object_new)(MonoDomain *, MonoClass *);
void (*mono_runtime_object_init)(MonoObject *);
void *(*mono_object_unbox)(MonoObject *);
MonoObject *(*mono_string_new)(MonoDomain *, const char *);
uint64_t (*mono_compile_method)(MonoMethod *);
uint32_t (*mono_gchandle_new)(MonoObject *, int);
MonoObject *(*mono_gchandle_get_target)(uint32_t);

int (*sceUserServiceGetForegroundUser)(int *);
int (*sceUserServiceGetInitialUser)(int *);
int (*scePadGetHandle)(int, int, int);
int (*scePadOpen)(int, int, int, void *);
int (*scePadReadState)(int, void *);

MonoDomain *g_domain;
MonoClass *g_widget_class, *g_panel_class, *g_label_class, *g_color_class, *g_font_class, *g_scene_class;
MonoMethod *g_append_child, *g_find_by_name, *g_remove_from_parent, *g_color_ctor, *g_font_ctor;
uint32_t g_game_handle;

void (*g_prev_update)(MonoObject *);
volatile bool g_armed;
bool g_quiet; /* mutes setup retries in the log */

void logf(const char *format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(line, sizeof(line) - 1, format, args);
  va_end(args);
  if (n < 0 || !g_log_path[0] || g_quiet) return;
  if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n';
  const int fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) return;
  (void)!write(fd, line, n);
  close(fd);
}

/* ---- small Mono helpers; every invoke swallows managed exceptions ---- */

MonoObject *invoke(MonoMethod *method, void *self, void **args) {
  if (!method) return nullptr;
  MonoObject *exception = nullptr;
  MonoObject *result = mono_runtime_invoke(method, self, args, &exception);
  return exception ? nullptr : result;
}

MonoMethod *setter(MonoClass *klass, const char *name) {
  void *prop = mono_class_get_property_from_name(klass, name);
  return prop ? mono_property_get_set_method(prop) : nullptr;
}

MonoMethod *getter(MonoClass *klass, const char *name) {
  void *prop = mono_class_get_property_from_name(klass, name);
  return prop ? mono_property_get_get_method(prop) : nullptr;
}

void set_float(MonoClass *klass, MonoObject *obj, const char *name, float value) {
  void *args[1] = {&value};
  invoke(setter(klass, name), obj, args);
}

void set_int(MonoClass *klass, MonoObject *obj, const char *name, int32_t value) {
  void *args[1] = {&value};
  invoke(setter(klass, name), obj, args);
}

void set_string(MonoClass *klass, MonoObject *obj, const char *name, const char *value) {
  void *args[1] = {mono_string_new(g_domain, value)};
  invoke(setter(klass, name), obj, args);
}

/* UIColor is a struct: construct it in a box and pass the unboxed data. */
void set_color(MonoClass *klass, MonoObject *obj, const char *name, const uint8_t rgba[4]) {
  MonoObject *boxed = mono_object_new(g_domain, g_color_class);
  if (!boxed) return;
  float r = rgba[0] / 255.0f, g = rgba[1] / 255.0f, b = rgba[2] / 255.0f, a = rgba[3] / 255.0f;
  void *color = mono_object_unbox(boxed);
  void *ctor_args[4] = {&r, &g, &b, &a};
  invoke(g_color_ctor, color, ctor_args);
  void *args[1] = {color};
  invoke(setter(klass, name), obj, args);
}

MonoObject *find_widget(MonoObject *root, const char *name) {
  void *args[1] = {mono_string_new(g_domain, name)};
  return invoke(g_find_by_name, root, args);
}

void remove_widget(MonoObject *root, const char *name) {
  MonoObject *widget = find_widget(root, name);
  if (widget) invoke(g_remove_from_parent, widget, nullptr);
}

float get_float(MonoObject *obj, const char *name) {
  MonoObject *boxed = invoke(getter(g_widget_class, name), obj, nullptr);
  return boxed ? *static_cast<float *>(mono_object_unbox(boxed)) : 0.0f;
}

MonoObject *root_widget() {
  MonoObject *game = mono_gchandle_get_target(g_game_handle);
  return game ? invoke(getter(g_scene_class, "RootWidget"), game, nullptr) : nullptr;
}

/* ---- panels ---- */

/* Creates a named Panel on `root`. rgba == nullptr leaves it transparent. */
MonoObject *make_panel(MonoObject *root, const char *name, float x, float y, float w, float h,
                       const uint8_t *rgba) {
  MonoObject *panel = mono_object_new(g_domain, g_panel_class);
  if (!panel) return nullptr;
  mono_runtime_object_init(panel);
  set_string(g_panel_class, panel, "Name", name);
  set_float(g_panel_class, panel, "X", x);
  set_float(g_panel_class, panel, "Y", y);
  set_float(g_panel_class, panel, "Width", w);
  set_float(g_panel_class, panel, "Height", h);
  if (rgba) {
    set_color(g_panel_class, panel, "BackgroundColor", rgba);
    set_int(g_panel_class, panel, "BackgroundVisibility", 1);
    set_float(g_panel_class, panel, "BackgroundOpacity", 1.0f);
    set_int(g_panel_class, panel, "BackgroundStyle", 1);
  } else {
    set_int(g_panel_class, panel, "BackgroundVisibility", 0);
  }
  void *append_args[1] = {panel};
  invoke(g_append_child, root, append_args);
  return panel;
}

int g_panel_count; /* effect panels currently attached, named id_scfx_0 .. n-1 */

void add_panel(MonoObject *root, float x, float y, float w, float h, const uint8_t rgba[4]) {
  char name[32];
  snprintf(name, sizeof(name), "id_scfx_%d", g_panel_count);
  if (make_panel(root, name, x, y, w, h, rgba)) ++g_panel_count;
}

/* Removes up to `budget` panels; returns true when none are left. */
bool remove_panels(MonoObject *root, int budget) {
  while (g_panel_count > 0 && budget-- > 0) {
    char name[32];
    snprintf(name, sizeof(name), "id_scfx_%d", g_panel_count - 1);
    remove_widget(root, name);
    --g_panel_count;
  }
  return g_panel_count == 0;
}

/* ---- build plan: a flat list of rectangles, emitted a few per frame ---- */

struct Rect { float x, y, w, h; uint8_t rgba[4]; };
constexpr int kMaxRects = 512;
constexpr int kMaxScanlines = 400; /* widget count ShellUI has to draw every frame */
constexpr int kPerFrame = 24;      /* panels created/removed per frame, to avoid hitches */

Rect g_plan[kMaxRects];
int g_plan_size, g_plan_done;
bool g_rebuild;
fx_state_v1 g_state;

void plan_add(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (g_plan_size >= kMaxRects || a == 0 || w <= 0 || h <= 0) return;
  g_plan[g_plan_size++] = {x, y, w, h, {r, g, b, a}};
}

void make_plan(float W, float H) {
  g_plan_size = g_plan_done = 0;
  const fx_state_v1 &s = g_state;
  if (!s.enabled) return;

  if (s.tint_rgba[3]) plan_add(0, 0, W, H, s.tint_rgba[0], s.tint_rgba[1], s.tint_rgba[2], s.tint_rgba[3]);
  if (s.dim_alpha) plan_add(0, 0, W, H, 0, 0, 0, s.dim_alpha);

  if (s.vignette && s.vig_alpha) {
    /* Nested frames; overlaps make corners darkest. */
    constexpr int kSteps = 6;
    const float band = H * 0.03f;
    for (int i = 0; i < kSteps; ++i) {
      const float falloff = static_cast<float>(kSteps - i) / kSteps;
      const uint8_t a = static_cast<uint8_t>(s.vig_alpha * falloff * falloff * 0.5f);
      const float inset = band * i;
      plan_add(0, inset, W, band, 0, 0, 0, a);
      plan_add(0, H - inset - band, W, band, 0, 0, 0, a);
      plan_add(inset, 0, band, H, 0, 0, 0, a);
      plan_add(W - inset - band, 0, band, H, 0, 0, 0, a);
    }
  }

  if (s.bars == FX_BARS_21_9) {
    const float bar = (H - W * 9.0f / 21.0f) / 2.0f;
    plan_add(0, 0, W, bar, 0, 0, 0, 255);
    plan_add(0, H - bar, W, bar, 0, 0, 0, 255);
  } else if (s.bars == FX_BARS_4_3) {
    const float bar = (W - H * 4.0f / 3.0f) / 2.0f;
    plan_add(0, 0, bar, H, 0, 0, 0, 255);
    plan_add(W - bar, 0, bar, H, 0, 0, 0, 255);
  }

  if (s.scanlines && s.scan_alpha) {
    /* scan_pitch is in 1080p pixels; scale to the scene and cap the line count. */
    float pitch = s.scan_pitch * H / 1080.0f;
    if (H / pitch > kMaxScanlines) pitch = H / kMaxScanlines;
    const float thickness = pitch / 2.0f;
    for (float y = 0; y < H; y += pitch) plan_add(0, y, W, thickness, 0, 0, 0, s.scan_alpha);
  }
}

bool read_state(fx_state_v1 *out) {
  const int fd = open(g_state_path, O_RDONLY);
  if (fd < 0) return false;
  fx_state_v1 st;
  const ssize_t n = read(fd, &st, sizeof(st));
  close(fd);
  if (n != (ssize_t)sizeof(st) || st.magic != FX_STATE_MAGIC || st.version != FX_STATE_VERSION) return false;
  *out = st;
  return true;
}

/* ---- pad ---- */

constexpr uint32_t kUp = 0x10, kDown = 0x40, kCircle = 0x2000, kCross = 0x4000;

int g_pad = -1;
bool g_pad_logged;

void send_request(const char *text) {
  const int fd = open(g_request_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return;
  (void)!write(fd, text, strlen(text));
  close(fd);
  rename(g_request_tmp_path, g_request_path);
}

uint32_t combo_mask(uint8_t combo) {
  if (combo == FX_COMBO_L3_R3) return 0x2 | 0x4;
  if (combo == FX_COMBO_L1_R1_OPTIONS) return 0x400 | 0x800 | 0x8;
  if (combo == FX_COMBO_L2_R2_TOUCHPAD) return 0x100 | 0x200 | 0x100000;
  return 0;
}

/* Returns false when no pad state is available this frame. */
bool read_buttons(uint32_t *buttons) {
  if (!scePadReadState) return false;
  if (g_pad < 0) {
    int user = -1;
    if (!sceUserServiceGetForegroundUser || sceUserServiceGetForegroundUser(&user) != 0 || user <= 0) {
      if (!sceUserServiceGetInitialUser || sceUserServiceGetInitialUser(&user) != 0) user = -1;
    }
    int handle = (user > 0 && scePadGetHandle) ? scePadGetHandle(user, 0, 0) : -1;
    if (handle < 0 && user > 0 && scePadOpen) handle = scePadOpen(user, 0, 0, nullptr);
    if (!g_pad_logged || handle >= 0) {
      g_pad_logged = true;
      logf("pad: user=0x%x handle=0x%x", user, handle);
    }
    if (handle < 0) return false;
    g_pad = handle;
  }
  uint8_t data[512];
  memset(data, 0, sizeof(data));
  if (scePadReadState(g_pad, data) != 0) return false;
  memcpy(buttons, data, sizeof(*buttons));
  return true;
}

/* ---- in-game menu ---- */

const char *const kPresetNames[FX_PRESET_COUNT] = FX_PRESET_NAMES;
constexpr int kMaxItems = FX_PRESET_COUNT + 1;
constexpr int kMenuTimeoutFrames = 600; /* closes itself after ~10 s without input */

bool g_menu_open, g_menu_dirty;
int g_menu_items[kMaxItems]; /* -1 = Off, otherwise FX_PRESET_* */
int g_menu_item_count, g_menu_selected, g_menu_idle_frames;
int g_menu_widget_count; /* widgets named id_scfxm_0 .. n-1 */
float g_menu_item_y0, g_menu_item_h;

/* Text needs a Label inside a transparent Panel cell, the layout OnionHEN's bar uses. */
void menu_text(MonoObject *root, float x, float y, float w, float h, const char *text, int size,
               const uint8_t rgba[4]) {
  char name[32];
  snprintf(name, sizeof(name), "id_scfxm_%d", g_menu_widget_count);
  MonoObject *cell = make_panel(root, name, x, y, w, h, nullptr);
  if (!cell) return;
  ++g_menu_widget_count;

  MonoObject *label = mono_object_new(g_domain, g_label_class);
  if (!label) return;
  mono_runtime_object_init(label);
  snprintf(name, sizeof(name), "id_scfxm_l%d", g_menu_widget_count);
  set_string(g_label_class, label, "Name", name);
  set_int(g_label_class, label, "PositionType", 1);
  set_float(g_label_class, label, "MarginLeft", 0.0f);
  set_float(g_label_class, label, "MarginTop", 0.0f);
  set_float(g_label_class, label, "Width", w);
  set_string(g_label_class, label, "Text", text);

  /* UIFont is a struct like UIColor: (size, style 1 = bold, weight). */
  MonoObject *boxed = mono_object_new(g_domain, g_font_class);
  if (boxed) {
    int32_t font_size = size, style = 1, weight = 700;
    void *font = mono_object_unbox(boxed);
    void *ctor_args[3] = {&font_size, &style, &weight};
    invoke(g_font_ctor, font, ctor_args);
    void *font_args[1] = {font};
    invoke(setter(g_label_class, "Font"), label, font_args);
  }
  set_int(g_label_class, label, "HorizontalAlignment", 0);
  set_int(g_label_class, label, "VerticalAlignment", 0);
  set_color(g_label_class, label, "TextColor", rgba);
  set_int(g_label_class, label, "FitWidthToText", 0);
  set_int(g_label_class, label, "FitHeightToText", 1);
  set_int(g_label_class, label, "NumberOfLines", 1);

  void *append_args[1] = {label};
  invoke(g_append_child, cell, append_args);
}

void menu_box(MonoObject *root, const char *name, float x, float y, float w, float h, const uint8_t rgba[4]) {
  make_panel(root, name, x, y, w, h, rgba);
}

void menu_remove(MonoObject *root) {
  remove_widget(root, "id_scfxm_bg");
  remove_widget(root, "id_scfxm_hl");
  while (g_menu_widget_count > 0) {
    char name[32];
    snprintf(name, sizeof(name), "id_scfxm_%d", --g_menu_widget_count);
    remove_widget(root, name);
  }
}

void menu_place_highlight(MonoObject *root) {
  MonoObject *highlight = find_widget(root, "id_scfxm_hl");
  if (highlight) set_float(g_panel_class, highlight, "Y", g_menu_item_y0 + g_menu_selected * g_menu_item_h);
}

void menu_build(MonoObject *root, float H) {
  menu_remove(root);

  g_menu_item_count = 0;
  g_menu_items[g_menu_item_count++] = -1;
  for (int preset = FX_PRESET_CUSTOM + 1; preset < FX_PRESET_COUNT; ++preset) g_menu_items[g_menu_item_count++] = preset;
  if (g_state.has_custom) g_menu_items[g_menu_item_count++] = FX_PRESET_CUSTOM;
  if (g_menu_selected >= g_menu_item_count) g_menu_selected = 0;

  const float k = H / 1080.0f; /* layout is designed at 1080p */
  const float x = 90 * k, y = 110 * k, w = 520 * k, pad = 28 * k;
  const float title_h = 72 * k, hint_h = 56 * k;
  g_menu_item_h = 56 * k;
  g_menu_item_y0 = y + title_h;
  const float h = title_h + g_menu_item_count * g_menu_item_h + hint_h;

  static const uint8_t kBack[4] = {12, 14, 20, 225}, kSelect[4] = {40, 120, 255, 230};
  static const uint8_t kWhite[4] = {255, 255, 255, 255}, kAccent[4] = {120, 200, 255, 255};
  static const uint8_t kGrey[4] = {170, 176, 190, 255};

  menu_box(root, "id_scfxm_bg", x, y, w, h, kBack);
  menu_box(root, "id_scfxm_hl", x, g_menu_item_y0 + g_menu_selected * g_menu_item_h, w, g_menu_item_h, kSelect);
  menu_text(root, x + pad, y + 16 * k, w - 2 * pad, title_h, "ScreenFX", static_cast<int>(30 * k), kAccent);
  for (int i = 0; i < g_menu_item_count; ++i) {
    const char *text = g_menu_items[i] < 0 ? "Off" : kPresetNames[g_menu_items[i]];
    menu_text(root, x + pad, g_menu_item_y0 + i * g_menu_item_h + 10 * k, w - 2 * pad, g_menu_item_h, text,
              static_cast<int>(26 * k), kWhite);
  }
  menu_text(root, x + pad, g_menu_item_y0 + g_menu_item_count * g_menu_item_h + 14 * k, w - 2 * pad, hint_h,
            "Up / Down: choose      Circle: close", static_cast<int>(18 * k), kGrey);
}

void menu_open() {
  g_menu_open = true;
  g_menu_dirty = true;
  g_menu_idle_frames = 0;
  /* Start on what is active now; menu_build clamps if Custom is not listed. */
  if (!g_state.enabled) g_menu_selected = 0;
  else if (g_state.preset == FX_PRESET_CUSTOM) g_menu_selected = FX_PRESET_COUNT;
  else g_menu_selected = g_state.preset;
}

void menu_apply_selection() {
  char text[24];
  snprintf(text, sizeof(text), "set %d", g_menu_items[g_menu_selected]);
  send_request(text);
}

/* ---- input: combo opens/closes the menu, held combo is a quick on/off ---- */

int g_held_frames;
bool g_fired_hold;
uint32_t g_last_buttons;

void handle_input(MonoObject *root) {
  uint32_t buttons = 0;
  if (!read_buttons(&buttons)) { g_held_frames = 0; return; }
  const uint32_t pressed = buttons & ~g_last_buttons;
  g_last_buttons = buttons;

  const uint32_t mask = combo_mask(g_state.combo);
  if (mask && (buttons & mask) == mask) {
    if (++g_held_frames >= 60 && !g_fired_hold) { /* ~1 s */
      g_fired_hold = true;
      send_request("toggle");
    }
    return;
  }
  const bool tapped = g_held_frames >= 3 && !g_fired_hold;
  g_held_frames = 0;
  g_fired_hold = false;

  if (tapped) {
    if (g_menu_open) { g_menu_open = false; g_menu_dirty = true; }
    else menu_open();
    return;
  }
  if (!g_menu_open) return;

  if (pressed & (kCircle | kCross)) {
    g_menu_open = false;
    g_menu_dirty = true;
    return;
  }
  int step = 0;
  if (pressed & kDown) step = 1;
  else if (pressed & kUp) step = -1;
  if (step != 0 && g_menu_item_count > 0) {
    g_menu_selected = (g_menu_selected + step + g_menu_item_count) % g_menu_item_count;
    g_menu_idle_frames = 0;
    if (root) menu_place_highlight(root);
    menu_apply_selection(); /* live preview: moving the cursor applies the look */
  } else if (++g_menu_idle_frames >= kMenuTimeoutFrames) {
    g_menu_open = false;
    g_menu_dirty = true;
  }
}

/* ---- per-frame work on the UI thread ---- */

unsigned g_frame;
int g_plan_logs;

void tick() {
  ++g_frame;

  /* Follow the plugin faster while the menu is giving a live preview. */
  if (g_frame % (g_menu_open ? 6 : 30) == 0) {
    fx_state_v1 st;
    if (read_state(&st) && memcmp(&st, &g_state, sizeof(st)) != 0) {
      if (st.has_custom != g_state.has_custom && g_menu_open) g_menu_dirty = true;
      g_state = st;
      g_rebuild = true;
    }
  }

  MonoObject *root = nullptr;
  if (g_pad >= 0 || g_frame % 30 == 0) {
    if (g_menu_open) root = root_widget();
    handle_input(root);
  }

  const bool effects_busy = g_rebuild || g_plan_done < g_plan_size;
  if (!effects_busy && !g_menu_dirty) return;
  if (!root) root = root_widget();
  if (!root) return;

  if (g_rebuild) {
    if (!remove_panels(root, kPerFrame * 4)) return; /* keep clearing next frame */
    const float W = get_float(root, "Width"), H = get_float(root, "Height");
    if (W <= 1.0f || H <= 1.0f) return;
    make_plan(W, H);
    g_rebuild = false;
    if (g_plan_logs < 20) {
      ++g_plan_logs;
      logf("plan: %d panels on %.0fx%.0f (enabled=%d)", g_plan_size, W, H, g_state.enabled);
    }
  }

  for (int i = 0; i < kPerFrame && g_plan_done < g_plan_size; ++i) {
    const Rect &r = g_plan[g_plan_done++];
    add_panel(root, r.x, r.y, r.w, r.h, r.rgba);
    /* New effect panels land above the menu; redraw it once they are all in. */
    if (g_plan_done == g_plan_size && g_menu_open) g_menu_dirty = true;
  }

  if (g_menu_dirty && g_plan_done >= g_plan_size) {
    g_menu_dirty = false;
    if (g_menu_open) menu_build(root, get_float(root, "Height"));
    else menu_remove(root);
  }
}

void update_hook(MonoObject *instance) {
  g_prev_update(instance);
  if (g_armed) tick();
}

/* ---- setup ---- */

template <typename Fn>
bool sym(int handle, const char *name, Fn &out) {
  out = reinterpret_cast<Fn>(kernel_dynlib_dlsym(-1, handle, name));
  return out != nullptr;
}

bool resolve_symbols(pid_t pid) {
  const int mono = get_module_handle(pid, "libmonosgen-2.0.sprx");
  if (!mono) { logf("init: libmonosgen not loaded"); return false; }
#define MONO(name) if (!sym(mono, #name, name)) { logf("init: missing %s", #name); return false; }
  MONO(mono_get_root_domain) MONO(mono_thread_attach) MONO(mono_domain_assembly_open)
  MONO(mono_assembly_get_image) MONO(mono_class_from_name) MONO(mono_class_get_method_from_name)
  MONO(mono_class_get_property_from_name) MONO(mono_property_get_get_method)
  MONO(mono_property_get_set_method) MONO(mono_runtime_invoke) MONO(mono_object_new)
  MONO(mono_runtime_object_init) MONO(mono_object_unbox) MONO(mono_string_new)
  MONO(mono_compile_method) MONO(mono_gchandle_new) MONO(mono_gchandle_get_target)
#undef MONO

  /* Pad is optional: without it effects still work from the Toolbox. */
  const int user = get_module_handle(pid, "libSceUserService.sprx");
  const int pad = get_module_handle(pid, "libScePad.sprx");
  if (user) { sym(user, "sceUserServiceGetForegroundUser", sceUserServiceGetForegroundUser);
              sym(user, "sceUserServiceGetInitialUser", sceUserServiceGetInitialUser); }
  if (pad) { sym(pad, "scePadGetHandle", scePadGetHandle); sym(pad, "scePadOpen", scePadOpen);
             sym(pad, "scePadReadState", scePadReadState); }
  logf("init: pad module=%d readState=%p", pad, reinterpret_cast<void *>(scePadReadState));
  return true;
}

MonoImage *open_image(const char *file) {
  char path[160];
  snprintf(path, sizeof(path), "/system_ex/common_ex/lib/%s", file);
  void *assembly = mono_domain_assembly_open(g_domain, path);
  return assembly ? mono_assembly_get_image(assembly) : nullptr;
}

bool resolve_ui() {
  MonoImage *pui = open_image("Sce.PlayStation.PUI.dll");
  MonoImage *app = open_image("Sce.Vsh.ShellUI.AppSystem.dll");
  if (!pui || !app) { logf("init: assemblies pui=%p app=%p", pui, app); return false; }

  g_widget_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI2", "Widget");
  g_panel_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI2", "Panel");
  g_scene_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI2", "Scene");
  g_color_class = mono_class_from_name(pui, "Sce.PlayStation.PUI", "UIColor");
  g_label_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI2", "Label");
  g_font_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI2", "UIFont");
  if (!g_widget_class || !g_panel_class || !g_scene_class || !g_color_class || !g_label_class || !g_font_class) {
    logf("init: classes widget=%p panel=%p scene=%p color=%p label=%p font=%p", g_widget_class, g_panel_class,
         g_scene_class, g_color_class, g_label_class, g_font_class);
    return false;
  }
  g_append_child = mono_class_get_method_from_name(g_widget_class, "AppendChild", 1);
  g_find_by_name = mono_class_get_method_from_name(g_widget_class, "FindWidgetByName", 1);
  g_remove_from_parent = mono_class_get_method_from_name(g_widget_class, "RemoveFromParent", 0);
  g_color_ctor = mono_class_get_method_from_name(g_color_class, ".ctor", 4);
  g_font_ctor = mono_class_get_method_from_name(g_font_class, ".ctor", 3);
  if (!g_append_child || !g_find_by_name || !g_remove_from_parent || !g_color_ctor || !g_font_ctor) {
    logf("init: methods append=%p find=%p remove=%p color=%p font=%p", g_append_child, g_find_by_name,
         g_remove_from_parent, g_color_ctor, g_font_ctor);
    return false;
  }

  MonoClass *layers = mono_class_from_name(app, "Sce.Vsh.ShellUI.AppSystem", "LayerManager");
  MonoMethod *find = layers ? mono_class_get_method_from_name(layers, "FindContainerSceneByPath", 1) : nullptr;
  void *args[1] = {mono_string_new(g_domain, "Game")};
  MonoObject *game = invoke(find, nullptr, args);
  if (!game) { logf("init: Game scene not found (layers=%p find=%p)", layers, find); return false; }
  g_game_handle = mono_gchandle_new(game, 1);

  MonoClass *application = mono_class_from_name(pui, "Sce.PlayStation.PUI", "Application");
  MonoMethod *update = application ? mono_class_get_method_from_name(application, "Update", 0) : nullptr;
  if (!update) { logf("init: Application.Update not found"); return false; }
  return true;
}

struct alignas(16) Block16 { uint64_t low, high; };

/*
 * OnionHEN's detour is `jmp [rip+0]` followed by the 8-byte target, written at
 * a 16-byte aligned method entry. Swap only that target, atomically, and keep
 * the old one as our "original". Refuse anything that does not look like it.
 */
bool chain_update_hook(MonoImage *pui) {
  MonoClass *application = mono_class_from_name(pui, "Sce.PlayStation.PUI", "Application");
  MonoMethod *update = mono_class_get_method_from_name(application, "Update", 0);
  const uint64_t address = mono_compile_method(update);
  if (!address || (address & 15) != 0) { logf("hook: bad Update address %#lx", address); return false; }

  constexpr uint64_t kPage = 0x4000;
  if (kernel_mprotect(getpid(), address & ~(kPage - 1), kPage, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    logf("hook: mprotect failed at %#lx", address);
    return false;
  }

  auto *target = reinterpret_cast<volatile Block16 *>(address);
  uint8_t current[16];
  memcpy(current, reinterpret_cast<const void *>(address), sizeof(current));
  static const uint8_t kJump[6] = {0xff, 0x25, 0, 0, 0, 0};
  if (memcmp(current, kJump, sizeof(kJump)) != 0) {
    logf("hook: Update is not detoured by OnionHEN yet (%02x %02x %02x %02x %02x %02x)", current[0],
         current[1], current[2], current[3], current[4], current[5]);
    return false;
  }

  uint64_t previous;
  memcpy(&previous, current + 6, sizeof(previous));
  g_prev_update = reinterpret_cast<void (*)(MonoObject *)>(previous);

  uint8_t wanted[16];
  memcpy(wanted, current, sizeof(wanted));
  const uint64_t mine = reinterpret_cast<uint64_t>(&update_hook);
  memcpy(wanted + 6, &mine, sizeof(mine));

  Block16 expected, desired;
  memcpy(&expected, current, sizeof(expected));
  memcpy(&desired, wanted, sizeof(desired));
  unsigned char exchanged = 0;
  __asm__ volatile("lock; cmpxchg16b %1; sete %0"
                   : "=q"(exchanged), "+m"(*target), "+a"(expected.low), "+d"(expected.high)
                   : "b"(desired.low), "c"(desired.high)
                   : "cc", "memory");
  logf("hook: Update=%#lx previous=%#lx mine=%#lx %s", address, previous, mine,
       exchanged ? "chained" : "changed under us, not installed");
  return exchanged != 0;
}

} // namespace

int main() {
  const pid_t pid = getpid();
  /* Without the shared directory there is nothing to draw and nowhere to log;
   * leave ShellUI untouched and let the plugin report the missing marker. */
  if (!pick_directory()) {
    onion_ready_signal_pid(FX_READY_NAME "_nodir", pid);
    for (;;) sleep(0x100000);
  }
  struct stat log_stat;
  if (stat(g_log_path, &log_stat) == 0 && log_stat.st_size > 64 * 1024) unlink(g_log_path);
  logf("---- renderer start pid=%d dir=%s ----", pid, g_state_path);

  /* Same credential window OnionHEN's own ShellUI module uses during setup. */
  const uintptr_t old_authid = set_ucred_to_ptrace();
  bool ok = resolve_symbols(pid);
  if (ok) {
    g_domain = mono_get_root_domain();
    ok = g_domain != nullptr;
    if (ok) mono_thread_attach(g_domain);
  }

  /* Right after console start the Game scene or OnionHEN's frame hook may not
   * exist yet, so keep trying for two minutes. Only the first and the last
   * failure are logged. */
  constexpr int kAttempts = 120;
  bool have_ui = false, hooked = false;
  for (int attempt = 1; ok && attempt <= kAttempts && !hooked; ++attempt) {
    g_quiet = attempt != 1 && attempt != kAttempts;
    if (!have_ui) have_ui = resolve_ui();
    if (have_ui) {
      g_rebuild = true; /* first tick draws whatever state.bin holds */
      g_armed = true;
      hooked = chain_update_hook(open_image("Sce.PlayStation.PUI.dll"));
      if (!hooked) g_armed = false;
    }
    g_quiet = false;
    if (hooked) logf("init: hooked on attempt %d", attempt);
    else sleep(1);
  }
  ok = ok && hooked;
  if (old_authid) set_proc_authid(pid, old_authid);

  /* Tells the plugin this ShellUI instance already has a renderer. The marker
   * lives on tmpfs next to OnionHEN's own, so it cannot survive a reboot. */
  if (ok) onion_ready_signal_pid(FX_READY_NAME, pid);
  logf("init: %s", ok ? "ready" : "FAILED - nothing was hooked");

  /* Returning would run the payload's exit path inside ShellUI. */
  for (;;) sleep(0x100000);
}
