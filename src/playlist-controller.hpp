/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "playlist-library.hpp"

#include <obs.hpp>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>

/* The brain of the plugin. Owns the saved library (projects > playlists >
 * clips), drives one Media Source called "DDP Playlist Player", and handles
 * the commercial-break scene. Everything here runs on the OBS UI thread;
 * the hotkey and websocket layers marshal onto it. */
class PlaylistController : public QObject {
	Q_OBJECT

public:
	enum class State { Stopped, Playing, Paused };

	static PlaylistController *instance();
	static void create();
	static void destroy();

	/* ---- library / selection ---- */
	const Library &library() const { return lib; }
	int activeProjectIndex() const;
	int activePlaylistIndex() const;
	const Project *activeProject() const;
	const Playlist *activePlaylist() const;

	bool selectProject(int index);
	bool selectProjectByName(const QString &name);
	bool selectPlaylist(int index);
	bool selectPlaylistByName(const QString &name);

	QString addProject(const QString &name);
	bool renameProject(int index, const QString &name);
	bool duplicateProject(int index);
	bool deleteProject(int index);
	bool importProject(const QString &path);
	bool exportProject(int index, const QString &path) const;

	QString addPlaylist(const QString &name);
	bool renamePlaylist(int index, const QString &name);
	bool duplicatePlaylist(int index);
	bool deletePlaylist(int index);

	int addFiles(const QStringList &paths, int insertAt = -1);
	void removeItems(const QStringList &itemIds);
	void reorderItems(const QStringList &orderedIds);
	void moveItem(const QString &itemId, int delta);
	bool renameItem(const QString &itemId, const QString &name);

	void setLoop(bool on);
	void setAutoAdvance(bool on);
	void setAfterBreak(int mode);
	void setMonitorAudio(bool on);

	/* ---- playback ---- */
	void playItem(const QString &itemId);
	void playIndex(int index); // index in the active playlist, 0-based
	void play();               // resume, or start the selected/first clip
	void pause();
	void playPause();
	void stop();
	void next();
	void previous();
	void restart();
	void takeBreak();
	void endBreak();
	void toggleBreak();
	void addPlayerToCurrentScene();

	State state() const { return playState; }
	bool onBreak() const { return breakActive; }
	QString playingItemId() const { return playingItem; }
	QString playingPlaylistId() const { return playingPlaylist; }
	QString playingName() const;
	int playingIndex() const; // 0-based in its playlist, -1 if none
	int64_t timeMs() const;
	int64_t durationMs() const;
	bool hasPlayerSource() const;

	QJsonObject stateJson() const;

	/* ---- persistence & OBS lifecycle ---- */
	void loadLibrary();
	void saveLibrary();
	void setHotkeyData(const QString &json) { hotkeysJson = json; }
	QString hotkeyData() const { return hotkeysJson; }
	bool takeFirstLaunch();

	void onCollectionLoaded();
	void onCollectionCleanup();
	void onTransitionStopped();

signals:
	void libraryChanged();
	void playbackChanged();

private slots:
	void handleMediaEnded();

private:
	explicit PlaylistController(QObject *parent = nullptr);
	~PlaylistController() override;

	Project *project();
	Playlist *playlist();
	Playlist *findPlaylist(const QString &playlistId, Project **owner = nullptr);
	const Playlist *findPlaylist(const QString &playlistId) const;
	void ensureDefaults();
	void changed();

	OBSSourceAutoRelease findPlayer() const;
	OBSSourceAutoRelease ensurePlayer();
	OBSSourceAutoRelease ensureBreakScene(obs_source_t *player);
	void attachPlayer(obs_source_t *player);
	void detachPlayer();
	void loadFile(obs_source_t *player, const QString &path);
	bool startAt(const Playlist &pl, int index, int direction);
	void finishPlaylist();
	void setState(State s);

	static void mediaEndedCallback(void *data, calldata_t *cd);

	Library lib;
	QString libraryPath;
	QString hotkeysJson;

	State playState = State::Stopped;
	bool breakActive = false;
	QString playingPlaylist;
	QString playingItem;
	QElapsedTimer sinceLoad;
	quint64 stopToken = 0;

	OBSWeakSourceAutoRelease weakPlayer;
	OBSWeakSourceAutoRelease weakPreviousScene;
	OBSWeakSourceAutoRelease pendingCue; // Preview scene to put back after a break starts
};
