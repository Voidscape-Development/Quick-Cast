/*
Quick Cast - remote guests for OBS Studio
Copyright (C) 2026 Voidscape Development

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

#include "obs/guest-source.hpp"
#include "obs/return-output.hpp"
#include "obs/session-controller.hpp"
#include "ui/quick-cast-dock.hpp"
#include "util/log.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Quick Cast";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}

namespace {

qc::SessionController *g_controller = nullptr;

QPointer<qc::QuickCastDock> g_dock;

/** Plugin docks start out hidden; show ours once, docked on the right, so a new user can find it. */
void introduceDock()
{
	if (!g_dock || !g_controller->takeFirstRun())
		return;
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	for (QWidget *parent = g_dock->parentWidget(); parent; parent = parent->parentWidget()) {
		if (auto *dockWidget = qobject_cast<QDockWidget *>(parent)) {
			if (mainWindow) {
				dockWidget->setFloating(false);
				mainWindow->addDockWidget(Qt::RightDockWidgetArea, dockWidget);
			}
			dockWidget->setVisible(true);
			break;
		}
	}
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (!g_controller)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		introduceDock();
		if (g_controller->settings().startWithObs)
			g_controller->startShow();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// libobs objects (encoders, the return output) have to be gone before OBS tears the core down, which
		// happens before obs_module_unload.
		g_controller->shutdown();
		break;
	default:
		break;
	}
}

void routeCoreLog(qc::log::Level level, const std::string &message)
{
	int obsLevel = LOG_INFO;
	switch (level) {
	case qc::log::Level::Debug:
		obsLevel = LOG_DEBUG;
		break;
	case qc::log::Level::Info:
		obsLevel = LOG_INFO;
		break;
	case qc::log::Level::Warning:
		obsLevel = LOG_WARNING;
		break;
	case qc::log::Level::Error:
		obsLevel = LOG_ERROR;
		break;
	}
	obs_log(obsLevel, "%s", message.c_str());
}

} // namespace

bool obs_module_load(void)
{
	qc::log::setSink(routeCoreLog);

	qc::registerGuestSource();
	qc::registerReturnOutput();

	g_controller = new qc::SessionController();

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new qc::QuickCastDock(g_controller, mainWindow);
	g_dock = dock;
	if (!obs_frontend_add_dock_by_id("quick-cast-dock", obs_module_text("Dock.Title"), dock))
		obs_log(LOG_WARNING, "could not register the Quick Cast dock");

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	delete g_controller;
	g_controller = nullptr;
	qc::log::setSink(nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
