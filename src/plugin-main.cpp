/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

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

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include "playlist-controller.hpp"
#include "playlist-dock.hpp"
#include "remote-control.hpp"

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char *kDockId = "ddp-playlist-dock";
static QPointer<PlaylistDock> g_dock;

/* New docks start hidden in OBS. Show ours once, the first time the plugin runs,
 * so people find it without digging through the Docks menu. */
static void introduceDock()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dw = g_dock ? qobject_cast<QDockWidget *>(g_dock->parentWidget()) : nullptr;
	if (!main || !dw)
		return;
	dw->setFloating(false);
	main->addDockWidget(Qt::RightDockWidgetArea, dw);
	dw->show();
	dw->raise();
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "DDP Playlist Player: commercial and clip playlists per project, with Stream Deck / Companion control.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "DDP Playlist Player";
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	PlaylistController *c = PlaylistController::instance();
	if (!c)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		c->onCollectionLoaded();
		if (c->takeFirstLaunch())
			introduceDock();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		c->onCollectionLoaded();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		c->onCollectionCleanup();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		RemoteControl::saveHotkeys();
		c->onCollectionCleanup();
		break;
	default:
		break;
	}
}

static void saveCallback(obs_data_t *, bool saving, void *)
{
	if (saving)
		RemoteControl::saveHotkeys();
}

bool obs_module_load(void)
{
	PlaylistController::create();
	PlaylistController::instance()->loadLibrary();

	auto *dock = new PlaylistDock(static_cast<QMainWindow *>(obs_frontend_get_main_window()));
	if (!obs_frontend_add_dock_by_id(kDockId, "DDP Playlist", dock))
		obs_log(LOG_WARNING, "Could not add the DDP Playlist dock");
	g_dock = dock;

	RemoteControl::registerHotkeys();
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_frontend_add_save_callback(saveCallback, nullptr);

	obs_log(LOG_INFO, "DDP Playlist Player loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	RemoteControl::registerWebsocketVendor();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	obs_frontend_remove_save_callback(saveCallback, nullptr);
	RemoteControl::shutdown();
	RemoteControl::unregisterHotkeys();
	if (auto *c = PlaylistController::instance())
		c->saveLibrary();
	PlaylistController::destroy();
	obs_log(LOG_INFO, "DDP Playlist Player unloaded");
}
