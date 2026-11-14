#pragma once

#include <stddef.h>

typedef enum fx_inject_result {
    FX_INJECT_NOTHING = 0,   /* nothing to report */
    FX_INJECT_WAITING,       /* not loaded yet: Toolbox not ready, or a load will be retried */
    FX_INJECT_LOADED,        /* progress: loaded, then again when the renderer reports ready */
    FX_INJECT_FAILED,        /* gave up on this ShellUI instance */
    FX_INJECT_GUARD_TRIPPED  /* ShellUI restarted right after a load */
} fx_inject_result;

/* Call every couple of seconds while drawing is enabled. Loads the renderer
 * into SceShellUI once per ShellUI instance, after OnionHEN's Toolbox is up. */
fx_inject_result fx_inject_poll(char *detail, size_t detail_size);
