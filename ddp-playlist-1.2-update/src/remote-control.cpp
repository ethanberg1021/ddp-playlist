/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "remote-control.hpp"
#include "playlist-controller.hpp"

#include <obs-module.h>
#include <obs-websocket-api.h>
#include <plugin-support.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <functional>
#include <vector>

namespace {

/* ---------------- helpers ---------------- */

void runOnUiThread(const std::function<void()> &fn)
{
	PlaylistController *ctrl = PlaylistController::instance();
	if (!ctrl)
		return;
	if (QThread::currentThread() == ctrl->thread())
		fn();
	else
		QMetaObject::invokeMethod(ctrl, fn, Qt::BlockingQueuedConnection);
}

void postToUiThread(const std::function<void()> &fn)
{
	PlaylistController *ctrl = PlaylistController::instance();
	if (ctrl)
		QMetaObject::invokeMethod(ctrl, fn, Qt::QueuedConnection);
}

QJsonObject dataToJson(obs_data_t *data)
{
	const char *json = data ? obs_data_get_json(data) : nullptr;
	if (!json)
		return {};
	return QJsonDocument::fromJson(QByteArray(json)).object();
}

void jsonToData(const QJsonObject &obj, obs_data_t *out)
{
	QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
	OBSDataAutoRelease tmp = obs_data_create_from_json(bytes.constData());
	if (tmp)
		obs_data_apply(out, tmp);
}

int toInt(const QJsonValue &v, int fallback)
{
	if (v.isDouble())
		return v.toInt();
	bool ok = false;
	int n = v.toString().trimmed().toInt(&ok);
	return ok ? n : fallback;
}

bool toBool(const QJsonValue &v, bool fallback)
{
	if (v.isBool())
		return v.toBool();
	QString s = v.toString().trimmed().toLower();
	if (s == "true" || s == "1" || s == "yes" || s == "on")
		return true;
	if (s == "false" || s == "0" || s == "no" || s == "off")
		return false;
	return fallback;
}

/* ---------------- hotkeys ---------------- */

struct HotkeyDef {
	const char *name;
	const char *description;
	void (PlaylistController::*action)();
};

const HotkeyDef kHotkeys[] = {
	{"ddp_playlist.play_pause", "DDP Playlist: Play / Pause", &PlaylistController::playPause},
	{"ddp_playlist.stop", "DDP Playlist: Stop", &PlaylistController::stop},
	{"ddp_playlist.next", "DDP Playlist: Next clip", &PlaylistController::next},
	{"ddp_playlist.previous", "DDP Playlist: Previous clip", &PlaylistController::previous},
	{"ddp_playlist.restart", "DDP Playlist: Restart clip", &PlaylistController::restart},
	{"ddp_playlist.take_break", "DDP Playlist: Take break (cut to commercials)", &PlaylistController::takeBreak},
	{"ddp_playlist.end_break", "DDP Playlist: End break (back to show)", &PlaylistController::endBreak},
	{"ddp_playlist.toggle_break", "DDP Playlist: Toggle break", &PlaylistController::toggleBreak},
};

std::vector<obs_hotkey_id> g_hotkeyIds;

void hotkeyPressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *def = static_cast<const HotkeyDef *>(data);
	postToUiThread([def]() {
		if (auto *c = PlaylistController::instance())
			(c->*(def->action))();
	});
}

/* ---------------- websocket vendor ---------------- */

obs_websocket_vendor g_vendor = nullptr;

using Handler = std::function<QJsonObject(PlaylistController *, const QJsonObject &)>;

QJsonObject ok(PlaylistController *c)
{
	QJsonObject o = c->stateJson();
	o["ok"] = true;
	return o;
}

QJsonObject fail(const QString &why)
{
	QJsonObject o;
	o["ok"] = false;
	o["error"] = why;
	return o;
}

QJsonObject simple(PlaylistController *c, void (PlaylistController::*fn)())
{
	(c->*fn)();
	return ok(c);
}

bool selectPlaylistFromRequest(PlaylistController *c, const QJsonObject &req, QString *err)
{
	if (req.contains("project")) {
		QJsonValue p = req["project"];
		bool found = p.isDouble() ? c->selectProject(p.toInt() - 1) : c->selectProjectByName(p.toString());
		if (!found) {
			*err = QStringLiteral("Project not found");
			return false;
		}
	}
	if (req.contains("playlist")) {
		QJsonValue p = req["playlist"];
		bool found = p.isDouble() ? c->selectPlaylist(p.toInt() - 1) : c->selectPlaylistByName(p.toString());
		if (!found) {
			*err = QStringLiteral("Playlist not found");
			return false;
		}
	}
	return true;
}

QJsonObject listProjects(PlaylistController *c)
{
	QJsonArray projects;
	for (const auto &p : c->library().projects) {
		QJsonArray pls;
		for (const auto &pl : p.playlists) {
			QJsonObject po;
			po["name"] = pl.name;
			po["count"] = pl.items.size();
			pls.append(po);
		}
		QJsonObject o;
		o["name"] = p.name;
		o["playlists"] = pls;
		projects.append(o);
	}
	QJsonObject out = ok(c);
	out["projects"] = projects;
	return out;
}

QJsonObject listItems(PlaylistController *c)
{
	QJsonArray items;
	if (const Playlist *pl = c->activePlaylist()) {
		int n = 1;
		for (const auto &it : pl->items) {
			QJsonObject o;
			o["index"] = n++;
			o["name"] = it.name;
			o["path"] = it.path;
			items.append(o);
		}
	}
	QJsonObject out = ok(c);
	out["items"] = items;
	return out;
}

QJsonObject playItem(PlaylistController *c, const QJsonObject &req)
{
	const Playlist *pl = c->activePlaylist();
	if (!pl)
		return fail("No playlist selected");
	if (req.contains("name")) {
		QString name = req["name"].toString().trimmed();
		for (const auto &it : pl->items)
			if (it.name.compare(name, Qt::CaseInsensitive) == 0) {
				c->playItem(it.id);
				return ok(c);
			}
		return fail("Clip not found");
	}
	int idx = toInt(req["index"], 0);
	if (idx < 1 || idx > pl->items.size())
		return fail("index must be 1 to " + QString::number(pl->items.size()));
	c->playIndex(idx - 1);
	return ok(c);
}

QJsonObject loadPlaylist(PlaylistController *c, const QJsonObject &req)
{
	QString err;
	if (!selectPlaylistFromRequest(c, req, &err))
		return fail(err);
	if (toBool(req["break"], false))
		c->takeBreak();
	else if (toBool(req["play"], false))
		c->playIndex(0);
	return ok(c);
}

const std::vector<std::pair<const char *, Handler>> &handlers()
{
	static const std::vector<std::pair<const char *, Handler>> list = {
		{"GetState", [](PlaylistController *c, const QJsonObject &) { return ok(c); }},
		{"Play", [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::play); }},
		{"Pause", [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::pause); }},
		{"PlayPause",
		 [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::playPause); }},
		{"Stop", [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::stop); }},
		{"Next", [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::next); }},
		{"Previous",
		 [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::previous); }},
		{"Restart",
		 [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::restart); }},
		{"TakeBreak",
		 [](PlaylistController *c, const QJsonObject &req) {
			 QString err;
			 if (!selectPlaylistFromRequest(c, req, &err))
				 return fail(err);
			 c->takeBreak();
			 return ok(c);
		 }},
		{"EndBreak",
		 [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::endBreak); }},
		{"ToggleBreak",
		 [](PlaylistController *c, const QJsonObject &) { return simple(c, &PlaylistController::toggleBreak); }},
		{"PlayItem", playItem},
		{"LoadPlaylist", loadPlaylist},
		{"SelectProject",
		 [](PlaylistController *c, const QJsonObject &req) {
			 QJsonObject r;
			 if (req.contains("project"))
				 r["project"] = req["project"];
			 else
				 r["project"] = req["name"];
			 QString err;
			 return selectPlaylistFromRequest(c, r, &err) ? ok(c) : fail(err);
		 }},
		{"SelectPlaylist",
		 [](PlaylistController *c, const QJsonObject &req) {
			 QJsonObject r = req;
			 if (!r.contains("playlist"))
				 r["playlist"] = req["name"];
			 QString err;
			 return selectPlaylistFromRequest(c, r, &err) ? ok(c) : fail(err);
		 }},
		{"AddClips",
		 [](PlaylistController *c, const QJsonObject &req) {
			 QString err;
			 if (!selectPlaylistFromRequest(c, req, &err))
				 return fail(err);
			 /* obs-websocket drops arrays of plain strings, so accept:
			  *   {"path": "C:/ads/spot.mp4"}
			  *   {"paths": "C:/ads/a.mp4|C:/ads/b.mp4"}
			  *   {"paths": [{"path": "C:/ads/a.mp4"}, ...]} */
			 QStringList paths;
			 QJsonValue pv = req["paths"];
			 if (pv.isArray()) {
				 for (const auto &v : pv.toArray())
					 paths << (v.isObject() ? v.toObject()["path"].toString() : v.toString());
			 } else if (pv.isString()) {
				 paths << pv.toString().split('|', Qt::SkipEmptyParts);
			 }
			 if (req.contains("path"))
				 paths << req["path"].toString();
			 for (QString &p : paths)
				 p = p.trimmed();
			 int added = c->addFiles(paths);
			 QJsonObject out = ok(c);
			 out["added"] = added;
			 return out;
		 }},
		{"ListProjects", [](PlaylistController *c, const QJsonObject &) { return listProjects(c); }},
		{"ListItems", [](PlaylistController *c, const QJsonObject &) { return listItems(c); }},
		{"SetLoop",
		 [](PlaylistController *c, const QJsonObject &req) {
			 c->setLoop(toBool(req["enabled"], true));
			 return ok(c);
		 }},
		{"AddPlayerToScene",
		 [](PlaylistController *c, const QJsonObject &) {
			 return simple(c, &PlaylistController::addPlayerToCurrentScene);
		 }},
	};
	return list;
}

void vendorRequest(obs_data_t *request, obs_data_t *response, void *priv)
{
	auto *handler = static_cast<const Handler *>(priv);
	QJsonObject req = dataToJson(request);
	QJsonObject result = fail("Plugin is shutting down");
	runOnUiThread([&]() {
		if (auto *c = PlaylistController::instance())
			result = (*handler)(c, req);
	});
	jsonToData(result, response);
}

void emitState()
{
	PlaylistController *c = PlaylistController::instance();
	if (!g_vendor || !c)
		return;
	QJsonObject st = c->stateJson();
	QByteArray bytes = QJsonDocument(st).toJson(QJsonDocument::Compact);
	OBSDataAutoRelease data = obs_data_create_from_json(bytes.constData());
	obs_websocket_vendor_emit_event(g_vendor, "StateChanged", data);
}

} // namespace

namespace RemoteControl {

void registerHotkeys()
{
	PlaylistController *c = PlaylistController::instance();
	OBSDataAutoRelease saved = c ? obs_data_create_from_json(c->hotkeyData().toUtf8().constData()) : nullptr;

	for (const auto &def : kHotkeys) {
		obs_hotkey_id id = obs_hotkey_register_frontend(def.name, def.description, hotkeyPressed,
								(void *)&def);
		g_hotkeyIds.push_back(id);
		if (saved) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(saved, def.name);
			if (arr)
				obs_hotkey_load(id, arr);
		}
	}
}

void saveHotkeys()
{
	PlaylistController *c = PlaylistController::instance();
	if (!c)
		return;
	OBSDataAutoRelease out = obs_data_create();
	for (size_t i = 0; i < g_hotkeyIds.size(); i++) {
		OBSDataArrayAutoRelease arr = obs_hotkey_save(g_hotkeyIds[i]);
		if (arr)
			obs_data_set_array(out, kHotkeys[i].name, arr);
	}
	c->setHotkeyData(QString::fromUtf8(obs_data_get_json(out)));
	c->saveLibrary();
}

void unregisterHotkeys()
{
	for (obs_hotkey_id id : g_hotkeyIds)
		obs_hotkey_unregister(id);
	g_hotkeyIds.clear();
}

void registerWebsocketVendor()
{
	g_vendor = obs_websocket_register_vendor("DDPPlaylist");
	if (!g_vendor) {
		obs_log(LOG_WARNING, "obs-websocket not available; Companion vendor requests disabled "
				     "(hotkeys still work)");
		return;
	}
	for (const auto &h : handlers())
		obs_websocket_vendor_register_request(g_vendor, h.first, vendorRequest, (void *)&h.second);

	if (auto *c = PlaylistController::instance()) {
		QObject::connect(c, &PlaylistController::playbackChanged, c, emitState);
		QObject::connect(c, &PlaylistController::libraryChanged, c, emitState);
	}
	obs_log(LOG_INFO, "Registered obs-websocket vendor 'DDPPlaylist' (%d requests)", (int)handlers().size());
}

void shutdown()
{
	g_vendor = nullptr;
}

} // namespace RemoteControl
