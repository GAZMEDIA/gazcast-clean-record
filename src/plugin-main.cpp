/*
GazCast Clean Record for OBS Studio
Copyright (C) 2026 GazCast

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <plugin-support.h>

#include "clean-record.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "GazCast Clean Record";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Records a clean copy of the program output without overlays, while the stream keeps them.";
}

namespace {

bool g_shuttingDown = false;

void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (g_shuttingDown) {
		return;
	}
	CleanRecordFrontendEvent(event);
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		g_shuttingDown = true;
		CleanRecordShutdown();
	}
}

} // namespace

bool obs_module_load(void)
{
	CleanRecordRegisterTypes();
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

/* Docks are added here, before OBS restores the saved window layout, so the
 * dock's position and visibility persist. */
void obs_module_post_load(void)
{
	CleanRecordCreateDock();
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
}

void obs_module_unload(void)
{
	/* After OBS_FRONTEND_EVENT_EXIT the frontend API is already torn down. */
	if (!g_shuttingDown) {
		obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	}
	obs_log(LOG_INFO, "plugin unloaded");
}
