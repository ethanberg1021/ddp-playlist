/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.hpp>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>

class QTimer;

/* Gapless playout with two Media Sources ("decks"), like an A/B playout server.
 *
 * Both decks live inside a scene called "DDP Playlist Player"; that scene is
 * what goes in the break scene (or any scene you add it to). One deck is on
 * air. The other sits hidden and silent with the next clip opened and its
 * first frame preloaded, but not playing. On the last frame of the on-air clip
 * (timed on OBS's own video clock, not the UI), the waiting deck is shown on
 * top and started from that preloaded frame. The old deck stays underneath for
 * a few frames as a safety net, so the worst case is a held frame, never black. */
class PlayerDecks : public QObject {
	Q_OBJECT

public:
	explicit PlayerDecks(QObject *parent = nullptr);
	~PlayerDecks() override;

	/* Creates the output scene and decks if missing (and upgrades the single
	 * 1.0/1.1 player). Returns the output scene's source, or null. */
	OBSSourceAutoRelease ensure(bool monitorAudio);
	OBSSourceAutoRelease output() const;
	bool exists() const;

	/* Load into the waiting deck. With autoTake, it cuts to it by itself on the
	 * last frame of the clip that's on air now. */
	void cue(const QString &path, bool autoTake);
	void playNow(const QString &path); // cut to this clip as soon as it's loaded
	void pause(bool paused);
	void stopAll();                    // stop both decks and clear the picture
	void setMonitoring(bool on);

	int64_t timeMs() const;
	int64_t durationMs() const;
	bool onAirEnded() const;

	void reattach(); // after a scene collection loads
	void detach();   // before a scene collection unloads

signals:
	void tookAir(bool automatic); // a new clip is on air (automatic = the end-of-clip cut)
	void clipEnded();             // the on-air clip ended with nothing cued to follow it

private slots:
	void pollCue();
	void afterTake(int oldDeck, quint64 token, bool automatic);
	void handleEnded(quintptr source);

private:
	OBSSourceAutoRelease deck(int i) const;
	bool cueReady(int i) const;
	void connectDeck(obs_source_t *src);
	void disconnectDeck(obs_source_t *src);
	void loadWaiting(obs_source_t *src, const QString &path);
	void takeLocked(bool automatic); // needs `lock` held; safe on any thread
	void refreshItems();

	static void signalCallback(void *data, calldata_t *cd);
	static void volumeCallback(void *data, calldata_t *cd);
	static void muteCallback(void *data, calldata_t *cd);
	static void tickCallback(void *data, float seconds);

	OBSWeakSourceAutoRelease weakOutput;
	OBSWeakSourceAutoRelease weakDecks[2];

	/* shared with the video thread */
	mutable std::mutex lock;
	OBSSource decksStrong[2];  // valid while attached
	OBSSceneItem items[2];     // each deck's item in the output scene
	std::atomic<int> onAir{0}; // index of the deck the audience sees
	std::atomic<bool> autoTake{false};
	std::atomic<int64_t> loadedNs[2];
	std::atomic<bool> armed[2];
	quint64 takeCount = 0;

	bool monitor = true;
	QString cuedPath; // what the waiting deck holds
	QElapsedTimer cuedAt;
	bool takePending = false;
	QElapsedTimer sinceTake;
	QTimer *cueTimer;
	bool syncing = false;
	bool tickAdded = false;
};
