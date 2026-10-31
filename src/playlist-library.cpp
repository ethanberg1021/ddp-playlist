/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "playlist-library.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

static const int kFormatVersion = 1;

int Playlist::indexOf(const QString &itemId) const
{
	for (int i = 0; i < items.size(); i++)
		if (items[i].id == itemId)
			return i;
	return -1;
}

int Project::indexOf(const QString &playlistId) const
{
	for (int i = 0; i < playlists.size(); i++)
		if (playlists[i].id == playlistId)
			return i;
	return -1;
}

int Library::indexOf(const QString &projectId) const
{
	for (int i = 0; i < projects.size(); i++)
		if (projects[i].id == projectId)
			return i;
	return -1;
}

QString Library::newId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString Library::displayNameFromPath(const QString &path)
{
	return QFileInfo(path).completeBaseName();
}

QStringList Library::mediaExtensions()
{
	return {"mp4", "mov", "m4v", "mkv", "webm", "avi", "flv", "ts",  "mts",  "m2ts", "mxf", "wmv",
		"mpg", "mpeg", "gif", "mp3", "wav", "aac", "m4a", "flac", "ogg", "opus", "aiff", "aif"};
}

/* ---------- JSON helpers ---------- */

static QJsonObject itemToJson(const PlaylistItem &it)
{
	QJsonObject o;
	o["id"] = it.id;
	o["name"] = it.name;
	o["path"] = it.path;
	return o;
}

static PlaylistItem itemFromJson(const QJsonObject &o)
{
	PlaylistItem it;
	it.id = o["id"].toString();
	it.name = o["name"].toString();
	it.path = o["path"].toString();
	if (it.id.isEmpty())
		it.id = Library::newId();
	if (it.name.isEmpty())
		it.name = Library::displayNameFromPath(it.path);
	return it;
}

static QJsonObject playlistToJson(const Playlist &pl)
{
	QJsonObject o;
	o["id"] = pl.id;
	o["name"] = pl.name;
	o["loop"] = pl.loop;
	o["autoAdvance"] = pl.autoAdvance;
	QJsonArray items;
	for (const auto &it : pl.items)
		items.append(itemToJson(it));
	o["items"] = items;
	return o;
}

static Playlist playlistFromJson(const QJsonObject &o)
{
	Playlist pl;
	pl.id = o["id"].toString();
	pl.name = o["name"].toString();
	pl.loop = o["loop"].toBool(false);
	pl.autoAdvance = o["autoAdvance"].toBool(true);
	for (const auto &v : o["items"].toArray())
		pl.items.append(itemFromJson(v.toObject()));
	if (pl.id.isEmpty())
		pl.id = Library::newId();
	if (pl.name.isEmpty())
		pl.name = QStringLiteral("Playlist");
	return pl;
}

static QJsonObject projectToJson(const Project &p)
{
	QJsonObject o;
	o["id"] = p.id;
	o["name"] = p.name;
	o["activePlaylistId"] = p.activePlaylistId;
	QJsonArray pls;
	for (const auto &pl : p.playlists)
		pls.append(playlistToJson(pl));
	o["playlists"] = pls;
	return o;
}

static Project projectFromJson(const QJsonObject &o)
{
	Project p;
	p.id = o["id"].toString();
	p.name = o["name"].toString();
	p.activePlaylistId = o["activePlaylistId"].toString();
	for (const auto &v : o["playlists"].toArray())
		p.playlists.append(playlistFromJson(v.toObject()));
	if (p.id.isEmpty())
		p.id = Library::newId();
	if (p.name.isEmpty())
		p.name = QStringLiteral("Project");
	return p;
}

static bool writeJson(const QString &path, const QJsonObject &root)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return f.commit();
}

static bool readJson(const QString &path, QJsonObject *out)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return false;
	QJsonParseError err;
	QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return false;
	*out = doc.object();
	return true;
}

/* ---------- Library ---------- */

bool Library::load(const QString &path, QString *hotkeysJson)
{
	QJsonObject root;
	if (!readJson(path, &root)) {
		// Try the backup left by the last good save
		if (!readJson(path + ".bak", &root))
			return false;
	}

	projects.clear();
	for (const auto &v : root["projects"].toArray())
		projects.append(projectFromJson(v.toObject()));

	activeProjectId = root["activeProjectId"].toString();
	returnAfterBreak = root["returnAfterBreak"].toBool(true);
	monitorAudio = root["monitorAudio"].toBool(true);
	dockIntroduced = root["dockIntroduced"].toBool(false);
	QString scene = root["breakSceneName"].toString();
	if (!scene.isEmpty())
		breakSceneName = scene;

	if (hotkeysJson) {
		QJsonObject hk = root["hotkeys"].toObject();
		*hotkeysJson = QString::fromUtf8(QJsonDocument(hk).toJson(QJsonDocument::Compact));
	}
	return true;
}

bool Library::save(const QString &path, const QString &hotkeysJson) const
{
	QJsonObject root;
	root["formatVersion"] = kFormatVersion;
	root["activeProjectId"] = activeProjectId;
	root["returnAfterBreak"] = returnAfterBreak;
	root["monitorAudio"] = monitorAudio;
	root["dockIntroduced"] = dockIntroduced;
	root["breakSceneName"] = breakSceneName;

	QJsonArray arr;
	for (const auto &p : projects)
		arr.append(projectToJson(p));
	root["projects"] = arr;

	QJsonDocument hk = QJsonDocument::fromJson(hotkeysJson.toUtf8());
	if (hk.isObject())
		root["hotkeys"] = hk.object();

	// keep one backup of the previous good file
	if (QFile::exists(path)) {
		QFile::remove(path + ".bak");
		QFile::copy(path, path + ".bak");
	}
	return writeJson(path, root);
}

bool Library::exportProject(const Project &project, const QString &path)
{
	QJsonObject root;
	root["formatVersion"] = kFormatVersion;
	root["type"] = QStringLiteral("ddp-playlist-project");
	root["project"] = projectToJson(project);
	return writeJson(path, root);
}

bool Library::importProject(const QString &path, Project *out)
{
	QJsonObject root;
	if (!readJson(path, &root))
		return false;
	if (!root.contains("project"))
		return false;
	*out = projectFromJson(root["project"].toObject());

	// Fresh ids so an import never collides with an existing project
	out->id = newId();
	QString oldActive = out->activePlaylistId;
	for (auto &pl : out->playlists) {
		QString newPl = newId();
		if (pl.id == oldActive)
			out->activePlaylistId = newPl;
		pl.id = newPl;
		for (auto &it : pl.items)
			it.id = newId();
	}
	return true;
}
