/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct PlaylistItem {
	QString id;
	QString name; // display name shown in the dock (renamable)
	QString path; // full path to the media file
};

struct Playlist {
	QString id;
	QString name;
	QVector<PlaylistItem> items;
	bool loop = false;
	bool autoAdvance = true;

	int indexOf(const QString &itemId) const;
};

struct Project {
	QString id;
	QString name;
	QVector<Playlist> playlists;
	QString activePlaylistId;

	int indexOf(const QString &playlistId) const;
};

/* Everything the plugin saves to disk: all projects, which one is open,
 * and global options. Stored as JSON in the OBS plugin config folder. */
struct Library {
	QVector<Project> projects;
	QString activeProjectId;

	bool returnAfterBreak = true;
	bool monitorAudio = true;
	bool dockIntroduced = false; // dock auto-shown once on first launch
	QString breakSceneName = QStringLiteral("DDP Commercial Break");

	int indexOf(const QString &projectId) const;

	bool load(const QString &path, QString *hotkeysJson);
	bool save(const QString &path, const QString &hotkeysJson) const;

	static bool exportProject(const Project &project, const QString &path);
	static bool importProject(const QString &path, Project *out);

	static QString newId();
	static QString displayNameFromPath(const QString &path);
	static QStringList mediaExtensions();
};
