/*
OBS-Trim
Copyright (C) 2026 BigRyPulls

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAction>
#include <QApplication>
#include <QMainWindow>
#include <QMetaObject>
#include <QTimer>

#include "trim-dialog.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static TrimDialog *g_dialog = nullptr;

static TrimDialog *ensureDialog()
{
	if (!g_dialog) {
		auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		g_dialog = new TrimDialog(mainWindow);
	}
	return g_dialog;
}

static void openLastRecording()
{
	char *recPath = obs_frontend_get_last_recording();
	QString path;
	if (recPath) {
		path = QString::fromUtf8(recPath);
		bfree(recPath);
	}
	if (path.isEmpty()) {
		obs_log(LOG_WARNING, "Open Last Recording: no recording path reported");
		return;
	}
	QMetaObject::invokeMethod(
		qApp,
		[path]() {
			TrimDialog *d = ensureDialog();
			d->openFile(path);
		},
		Qt::QueuedConnection);
}

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// Tools -> OBS-Trim -> Open Last Recording
		QAction *action = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(obs_module_text("OBS-Trim.OpenLast")));
		QObject::connect(action, &QAction::triggered, []() { openLastRecording(); });
		return;
	}

	if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		// Only after the recording is closed/finalized. The frontend API
		// reports the completed file; do not poll the recording directory.
		TrimDialog *d = ensureDialog();
		bool autoOpen = d ? d->autoOpenEnabled() : true;
		if (!autoOpen)
			return;

		// Small delay lets the muxer finish closing the file on slow disks.
		QTimer::singleShot(800, []() { openLastRecording(); });
		return;
	}

	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		if (g_dialog) {
			g_dialog->shutdown();
			delete g_dialog;
			g_dialog = nullptr;
		}
	}
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
