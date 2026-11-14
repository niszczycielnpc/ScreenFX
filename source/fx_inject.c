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

#define LOAD_ATTEMPTS 5
#define RETRY_SECONDS 10
/* The renderer keeps retrying its own setup for two minutes (console start). */
#define READY_TIMEOUT_SECONDS 150

static pid_t g_done_pid;      /* ShellUI instance we are finished with */
static pid_t g_target_pid;    /* ShellUI instance we are working on */
static pid_t g_waiting_pid;   /* instance already reported as waiting */
static int g_attempts;        /* loads tried into g_target_pid */
static time_t g_next_attempt; /* earliest time for the next load */
static time_t g_ready_since;  /* loaded; waiting for the renderer's marker */
static time_t g_loaded_at;    /* last successful load, for the crash guard */

fx_inject_result fx_inject_poll(char *detail, size_t detail_size) {
    const pid_t shellui = onion_find_pid("SceShellUI");
    if (shellui <= 0 || shellui == g_done_pid) return FX_INJECT_NOTHING;
    const time_t now = time(NULL);

    /* Up and hooked: either ours just now, or left by an earlier plugin run. */
    if (onion_ready_matches_pid(FX_READY_NAME, shellui)) {
        const bool ours = g_ready_since != 0 && g_target_pid == shellui;
        g_done_pid = shellui;
        g_ready_since = 0;
        if (!ours) return FX_INJECT_NOTHING;
        snprintf(detail, detail_size, "renderer is up and hooked");
        return FX_INJECT_LOADED;
    }

    if (shellui != g_target_pid) {
        /* New ShellUI process. If the previous one died soon after we loaded
         * into it, assume we caused that and stop. */
        if (g_loaded_at != 0 && now - g_loaded_at < CRASH_WINDOW_SECONDS) {
            g_done_pid = shellui;
            g_loaded_at = 0;
            snprintf(detail, detail_size, "ShellUI restarted within %d s of loading; giving up",
                     CRASH_WINDOW_SECONDS);
            return FX_INJECT_GUARD_TRIPPED;
        }
        g_target_pid = shellui;
        g_attempts = 0;
        g_next_attempt = 0;
        g_ready_since = 0;
    }

    if (g_ready_since != 0) {
        const bool nodir = onion_ready_matches_pid(FX_READY_NAME "_nodir", shellui);
        if (!nodir && now - g_ready_since < READY_TIMEOUT_SECONDS) return FX_INJECT_NOTHING;
        g_done_pid = shellui;
        g_ready_since = 0;
        snprintf(detail, detail_size, "%s",
                 nodir ? "renderer cannot see the screenfx folder from inside ShellUI"
                       : "renderer did not come up; check renderer.log");
        return FX_INJECT_FAILED;
    }

    /* The renderer chains onto OnionHEN's frame hook, so that must exist first. */
    if (!onion_ready_matches_pid(ONION_READY_TOOLBOX, shellui)) {
        if (g_waiting_pid == shellui) return FX_INJECT_NOTHING;
        g_waiting_pid = shellui;
        snprintf(detail, detail_size, "waiting for the OnionHEN Toolbox in ShellUI pid %d",
                 (int)shellui);
        return FX_INJECT_WAITING;
    }
    if (now < g_next_attempt) return FX_INJECT_NOTHING;

    const size_t size = (size_t)(fx_renderer_elf_end - fx_renderer_elf_start);
    uint8_t *copy = malloc(size);
    if (!copy) {
        g_done_pid = shellui;
        snprintf(detail, detail_size, "out of memory (%zu bytes)", size);
        return FX_INJECT_FAILED;
    }
    memcpy(copy, fx_renderer_elf_start, size);
    ++g_attempts;
    const bool ok = Inject_Toolbox((int)shellui, copy);
    free(copy);

    if (!ok) {
        /* ShellUI can be busy (another debugger attached) right after start. */
        if (g_attempts >= LOAD_ATTEMPTS) {
            g_done_pid = shellui;
            snprintf(detail, detail_size, "load into ShellUI pid %d failed %d times; giving up",
                     (int)shellui, g_attempts);
            return FX_INJECT_FAILED;
        }
        g_next_attempt = now + RETRY_SECONDS;
        snprintf(detail, detail_size, "load into ShellUI pid %d failed (try %d of %d); retrying",
                 (int)shellui, g_attempts, LOAD_ATTEMPTS);
        return FX_INJECT_WAITING;
    }
    g_loaded_at = now;
    g_ready_since = now;
    snprintf(detail, detail_size, "loaded into ShellUI pid %d (try %d, %zu bytes); see %s",
             (int)shellui, g_attempts, size, FX_RENDERER_LOG_PATH);
    return FX_INJECT_LOADED;
}
