/*
 * ScreenFX renderer - runs inside SceShellUI.
 *
 * Loaded by the ScreenFX plugin with the same remote ELF loader OnionHEN uses
 * for its Toolbox. Draws the look published in state.bin as PUI panels on the
 * "Game" scene (the layer OnionHEN's monitor bar lives on) and watches the pad
 * for the button combo.
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
MonoClass *g_widget_class, *g_panel_class, *g_color_class, *g_scene_class;
MonoMethod *g_append_child, *g_find_by_name, *g_remove_from_parent, *g_color_ctor;
uint32_t g_game_handle;

void (*g_prev_update)(MonoObject *);
volatile bool g_armed;

void logf(const char *format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(line, sizeof(line) - 1, format, args);
  va_end(args);
  if (n < 0 || !g_log_path[0]) return;
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

void set_float(MonoObject *obj, const char *name, float value) {
  void *args[1] = {&value};
  invoke(setter(g_panel_class, name), obj, args);
}

void set_int(MonoObject *obj, const char *name, int32_t value) {
  void *args[1] = {&value};
  invoke(setter(g_panel_class, name), obj, args);
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

int g_panel_count; /* panels currently attached, named id_scfx_0 .. n-1 */

void add_panel(MonoObject *root, float x, float y, float w, float h, const uint8_t rgba[4]) {
  MonoObject *panel = mono_object_new(g_domain, g_panel_class);
  if (!panel) return;
  mono_runtime_object_init(panel);

  char name[32];
  snprintf(name, sizeof(name), "id_scfx_%d", g_panel_count);
  void *name_args[1] = {mono_string_new(g_domain, name)};
  invoke(setter(g_panel_class, "Name"), panel, name_args);
  set_float(panel, "X", x);
  set_float(panel, "Y", y);
  set_float(panel, "Width", w);
  set_float(panel, "Height", h);

  /* UIColor is a struct: construct it in a box and pass the unboxed data. */
  MonoObject *boxed = mono_object_new(g_domain, g_color_class);
  if (boxed) {
    float r = rgba[0] / 255.0f, g = rgba[1] / 255.0f, b = rgba[2] / 255.0f, a = rgba[3] / 255.0f;
    void *color = mono_object_unbox(boxed);
    void *ctor_args[4] = {&r, &g, &b, &a};
    invoke(g_color_ctor, color, ctor_args);
    void *color_args[1] = {color};
    invoke(setter(g_panel_class, "BackgroundColor"), panel, color_args);
  }
  set_int(panel, "BackgroundVisibility", 1);
  set_float(panel, "BackgroundOpacity", 1.0f);
  set_int(panel, "BackgroundStyle", 1);

  void *append_args[1] = {panel};
  invoke(g_append_child, root, append_args);
  ++g_panel_count;
}

/* Removes up to `budget` panels; returns true when none are left. */
bool remove_panels(MonoObject *root, int budget) {
  while (g_panel_count > 0 && budget-- > 0) {
    char name[32];
    snprintf(name, sizeof(name), "id_scfx_%d", g_panel_count - 1);
    void *args[1] = {mono_string_new(g_domain, name)};
    MonoObject *panel = invoke(g_find_by_name, root, args);
    if (panel) invoke(g_remove_from_parent, panel, nullptr);
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

/* ---- pad combo ---- */

int g_pad = -1;
int g_held_frames;
bool g_fired_hold, g_pad_logged;

void send_request(const char *word) {
  const int fd = open(g_request_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return;
  (void)!write(fd, word, strlen(word));
  close(fd);
  rename(g_request_tmp_path, g_request_path);
}

void poll_pad(uint8_t combo) {
  uint32_t mask = 0;
  if (combo == FX_COMBO_L3_R3) mask = 0x2 | 0x4;
  else if (combo == FX_COMBO_L1_R1_OPTIONS) mask = 0x400 | 0x800 | 0x8;
  else if (combo == FX_COMBO_L2_R2_TOUCHPAD) mask = 0x100 | 0x200 | 0x100000;
  if (!mask || !scePadReadState) return;

  if (g_pad < 0) {
    int user = -1;
    if (!sceUserServiceGetForegroundUser || sceUserServiceGetForegroundUser(&user) != 0 || user <= 0) {
      if (!sceUserServiceGetInitialUser || sceUserServiceGetInitialUser(&user) != 0) user = -1;
    }
    int handle = (user > 0 && scePadGetHandle) ? scePadGetHandle(user, 0, 0) : -1;
    if (handle < 0 && user > 0 && scePadOpen) handle = scePadOpen(user, 0, 0, nullptr);
    if (!g_pad_logged) {
      g_pad_logged = true;
      logf("pad: user=0x%x handle=0x%x", user, handle);
    }
    if (handle < 0) return;
    g_pad = handle;
  }

  uint8_t data[512];
  memset(data, 0, sizeof(data));
  if (scePadReadState(g_pad, data) != 0) { g_held_frames = 0; return; }
  uint32_t buttons;
  memcpy(&buttons, data, sizeof(buttons));

  if ((buttons & mask) == mask) {
    if (++g_held_frames >= 60 && !g_fired_hold) { /* ~1 s */
      g_fired_hold = true;
      send_request("next_preset");
    }
    return;
  }
  if (g_held_frames >= 3 && !g_fired_hold) send_request("toggle");
  g_held_frames = 0;
  g_fired_hold = false;
}

/* ---- per-frame work on the UI thread ---- */

unsigned g_frame;

void tick() {
  ++g_frame;

  if (g_frame % 30 == 0) {
    fx_state_v1 st;
    if (read_state(&st) && memcmp(&st, &g_state, sizeof(st)) != 0) {
      g_state = st;
      g_rebuild = true;
    }
    /* A user may log in after boot; retry the pad a few times per minute. */
  }
  if (g_pad >= 0 || g_frame % 30 == 0) poll_pad(g_state.combo);

  if (!g_rebuild && g_plan_done >= g_plan_size) return;

  MonoObject *root = root_widget();
  if (!root) return;

  if (g_rebuild) {
    if (!remove_panels(root, kPerFrame * 4)) return; /* keep clearing next frame */
    const float W = get_float(root, "Width"), H = get_float(root, "Height");
    if (W <= 1.0f || H <= 1.0f) return;
    make_plan(W, H);
    g_rebuild = false;
    logf("plan: %d panels on %.0fx%.0f (enabled=%d)", g_plan_size, W, H, g_state.enabled);
  }

  for (int i = 0; i < kPerFrame && g_plan_done < g_plan_size; ++i) {
    const Rect &r = g_plan[g_plan_done++];
    add_panel(root, r.x, r.y, r.w, r.h, r.rgba);
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
  if (!g_widget_class || !g_panel_class || !g_scene_class || !g_color_class) {
    logf("init: classes widget=%p panel=%p scene=%p color=%p", g_widget_class, g_panel_class,
         g_scene_class, g_color_class);
    return false;
  }
  g_append_child = mono_class_get_method_from_name(g_widget_class, "AppendChild", 1);
  g_find_by_name = mono_class_get_method_from_name(g_widget_class, "FindWidgetByName", 1);
  g_remove_from_parent = mono_class_get_method_from_name(g_widget_class, "RemoveFromParent", 0);
  g_color_ctor = mono_class_get_method_from_name(g_color_class, ".ctor", 4);
  if (!g_append_child || !g_find_by_name || !g_remove_from_parent || !g_color_ctor) {
    logf("init: methods append=%p find=%p remove=%p ctor=%p", g_append_child, g_find_by_name,
         g_remove_from_parent, g_color_ctor);
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
  logf("---- renderer start pid=%d dir=%s ----", pid, g_state_path);

  /* Same credential window OnionHEN's own ShellUI module uses during setup. */
  const uintptr_t old_authid = set_ucred_to_ptrace();
  bool ok = resolve_symbols(pid);
  if (ok) {
    g_domain = mono_get_root_domain();
    ok = g_domain != nullptr;
    if (ok) mono_thread_attach(g_domain);
  }
  ok = ok && resolve_ui();
  if (ok) {
    g_rebuild = true; /* first tick draws whatever state.bin holds */
    g_armed = true;
    ok = chain_update_hook(open_image("Sce.PlayStation.PUI.dll"));
    if (!ok) g_armed = false;
  }
  if (old_authid) set_proc_authid(pid, old_authid);

  /* Tells the plugin this ShellUI instance already has a renderer. The marker
   * lives on tmpfs next to OnionHEN's own, so it cannot survive a reboot. */
  if (ok) onion_ready_signal_pid(FX_READY_NAME, pid);
  logf("init: %s", ok ? "ready" : "FAILED - nothing was hooked");

  /* Returning would run the payload's exit path inside ShellUI. */
  for (;;) sleep(0x100000);
}
