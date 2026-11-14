#pragma once

#include <stddef.h>

typedef enum fx_inject_result {
    FX_INJECT_NOTHING = 0,   /* nothing to report */
    FX_INJECT_WAITING,       /* OnionHEN Toolbox not ready in this ShellUI yet (reported once) */
    FX_INJECT_LOADED,        /* renderer loaded into this ShellUI instance */
    FX_INJECT_FAILED,        /* load failed; not retried for this instance */
    FX_INJECT_GUARD_TRIPPED  /* ShellUI restarted right after a load */
} fx_inject_result;

/* Call every couple of seconds while drawing is enabled. Loads the renderer
 * into SceShellUI once per ShellUI instance, after OnionHEN's Toolbox is up. */
fx_inject_result fx_inject_poll(char *detail, size_t detail_size);
