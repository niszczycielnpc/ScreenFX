#include "fx_inject.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include <onion/proc_query.h>
#include <onion/ready.h>

#include "fx_state.h"
#include "plugin_config.h"

#define CRASH_WINDOW_SECONDS 90

/* OnionHEN's remote ELF loader (libNineS): ptrace-attaches, maps the ELF and
 * starts its main on a new thread inside the target. */
bool Inject_Toolbox(int pid, uint8_t *elf);

extern uint8_t fx_renderer_elf_start[];
extern uint8_t fx_renderer_elf_end[];

__asm__(
    ".section .rodata\n"
    ".global fx_renderer_elf_start\n"
    ".align 16\n"
    "fx_renderer_elf_start:\n"
    ".incbin \"" FX_RENDERER_ELF "\"\n"
    ".global fx_renderer_elf_end\n"
    "fx_renderer_elf_end:\n"
    ".text\n");

static pid_t g_handled_pid;   /* ShellUI instance we already dealt with */
static time_t g_loaded_at;    /* when we last loaded the renderer */
static pid_t g_waiting_pid;   /* instance we already reported as waiting */
static int g_verify_polls;    /* polls left before checking the renderer came up */

fx_inject_result fx_inject_poll(char *detail, size_t detail_size) {
    const pid_t shellui = onion_find_pid("SceShellUI");
    if (shellui > 0 && shellui == g_handled_pid && g_verify_polls > 0 && --g_verify_polls == 0) {
        const bool ready = onion_ready_matches_pid(FX_READY_NAME, shellui);
        const bool nodir = onion_ready_matches_pid(FX_READY_NAME "_nodir", shellui);
        snprintf(detail, detail_size, "%s", ready ? "renderer is up and hooked"
                 : nodir ? "renderer cannot see the screenfx folder from inside ShellUI"
                         : "renderer did not come up; check renderer.log");
        return ready ? FX_INJECT_LOADED : FX_INJECT_FAILED;
    }
    if (shellui <= 0 || shellui == g_handled_pid) return FX_INJECT_NOTHING;

    /* A renderer from an earlier run of this plugin is still in there. */
    if (onion_ready_matches_pid(FX_READY_NAME, shellui)) {
        g_handled_pid = shellui;
        return FX_INJECT_NOTHING;
    }

    /* New ShellUI process. If the previous one died soon after we loaded
     * into it, assume we caused that and stop. */
    if (g_loaded_at != 0 && time(NULL) - g_loaded_at < CRASH_WINDOW_SECONDS) {
        g_handled_pid = shellui;
        g_loaded_at = 0;
        snprintf(detail, detail_size, "ShellUI restarted within %d s of loading; giving up",
                 CRASH_WINDOW_SECONDS);
        return FX_INJECT_GUARD_TRIPPED;
    }

    /* The renderer chains onto OnionHEN's frame hook, so that must exist first. */
    if (!onion_ready_matches_pid(ONION_READY_TOOLBOX, shellui)) {
        if (g_waiting_pid == shellui) return FX_INJECT_NOTHING;
        g_waiting_pid = shellui;
        snprintf(detail, detail_size, "waiting for the OnionHEN Toolbox in ShellUI pid %d",
                 (int)shellui);
        return FX_INJECT_WAITING;
    }

    g_handled_pid = shellui;
    const size_t size = (size_t)(fx_renderer_elf_end - fx_renderer_elf_start);
    uint8_t *copy = malloc(size);
    if (!copy) {
        snprintf(detail, detail_size, "out of memory (%zu bytes)", size);
        return FX_INJECT_FAILED;
    }
    memcpy(copy, fx_renderer_elf_start, size);
    const bool ok = Inject_Toolbox((int)shellui, copy);
    free(copy);

    if (!ok) {
        snprintf(detail, detail_size, "load into ShellUI pid %d failed", (int)shellui);
        return FX_INJECT_FAILED;
    }
    g_loaded_at = time(NULL);
    g_verify_polls = 5; /* ~10 s */
    snprintf(detail, detail_size, "loaded into ShellUI pid %d (%zu bytes); see %s", (int)shellui,
             size, FX_RENDERER_LOG_PATH);
    return FX_INJECT_LOADED;
}
