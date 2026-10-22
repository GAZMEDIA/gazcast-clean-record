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

#pragma once

#include <obs-frontend-api.h>

/* Clean recording: a second recording of the program output with overlay
 * sources left out, while the stream and the normal recording keep them. */

/* From obs_module_load: registers the clean mix source and the
 * "Hide in GazCast clean recording" filter. */
void CleanRecordRegisterTypes();

/* From obs_module_post_load: adds the "GazCast Clean Record" dock. */
void CleanRecordCreateDock();

/* Every frontend event, forwarded by plugin-main.cpp. */
void CleanRecordFrontendEvent(enum obs_frontend_event event);

/* On OBS_FRONTEND_EVENT_EXIT, before the main window goes. */
void CleanRecordShutdown();
