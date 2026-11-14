#pragma once

#include <onion/services.h>
#include <onion/status.h>
#include <onion/ui.h>

#include "fx_settings.h"

onion_status fx_ui_create(const fx_settings *s, onion_ui_document **out_document);
/* Applies one Toolbox event to *s. ONION_E_NOT_FOUND means "not ours". */
onion_status fx_ui_apply_event(onion_ui_handle handle, const onion_ui_event_v1 *event,
                               fx_settings *s);
/* Pushes every value back to the Toolbox page (after a preset or combo). */
void fx_ui_sync(const onion_host_services_v1 *services, onion_ui_handle handle,
                const fx_settings *s);
