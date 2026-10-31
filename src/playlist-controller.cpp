/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "playlist-controller.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QFileInfo>
#include <QTimer>

static PlaylistController *g_controller = nullptr;

static const char *kPlayerFlag = "ddp_playlist_player";
static const char *kPlayerName = "DDP Playlist Player";
static const int kIgnoreEndedMs = 750; // ignore "ended" noise right after loading a clip

PlaylistController *PlaylistController::instance()
{
	return g_controller;
}

void PlaylistController::create()
{
	if (!g_controller)
		g_controller = new PlaylistController();
}

void PlaylistController::destroy()
{
	delete g_controller;
	g_controller = nullptr;
}

PlaylistController::PlaylistController(QObject *parent) : QObject(parent)
{
	char *path = obs_module_config_path("library.json");
	libraryPath = QString::fromUtf8(path);
	bfree(path);
	sinceLoad.start();
}

PlaylistController::~PlaylistController()
{
	detachPlayer();
}

/* ============================================================
 * Library & selection
 * ============================================================ */

void PlaylistController::loadLibrary()
{
	QString hk;
	if (lib.load(libraryPath, &hk)) {
		hotkeysJson = hk;
		obs_log(LOG_INFO, "Loaded %d project(s) from %s", (int)lib.projects.size(),
			libraryPath.toUtf8().constData());
	} else {
		obs_log(LOG_INFO, "No saved library yet, starting fresh");
	}
	ensureDefaults();
	emit libraryChanged();
}

void PlaylistController::saveLibrary()
{
	if (!lib.save(libraryPath, hotkeysJson))
		obs_log(LOG_WARNING, "Could not save library to %s", libraryPath.toUtf8().constData());
}

bool PlaylistController::takeFirstLaunch()
{
	if (lib.dockIntroduced)
		return false;
	lib.dockIntroduced = true;
	saveLibrary();
	return true;
}

void PlaylistController::ensureDefaults()
{
	if (lib.projects.isEmpty()) {
		Project p;
		p.id = Library::newId();
		p.name = QStringLiteral("My Show");
		lib.projects.append(p);
	}
	if (lib.indexOf(lib.activeProjectId) < 0)
		lib.activeProjectId = lib.projects.first().id;

	for (auto &p : lib.projects) {
		if (p.playlists.isEmpty()) {
			Playlist pl;
			pl.id = Library::newId();
			pl.name = QStringLiteral("Commercial Break 1");
			p.playlists.append(pl);
		}
		if (p.indexOf(p.activePlaylistId) < 0)
			p.activePlaylistId = p.playlists.first().id;
	}
}

void PlaylistController::changed()
{
	ensureDefaults();
	saveLibrary();
	emit libraryChanged();
}

int PlaylistController::activeProjectIndex() const
{
	return lib.indexOf(lib.activeProjectId);
}

int PlaylistController::activePlaylistIndex() const
{
	const Project *p = activeProject();
	return p ? p->indexOf(p->activePlaylistId) : -1;
}

const Project *PlaylistController::activeProject() const
{
	int i = activeProjectIndex();
	return i >= 0 ? &lib.projects[i] : nullptr;
}

const Playlist *PlaylistController::activePlaylist() const
{
	const Project *p = activeProject();
	if (!p)
		return nullptr;
	int i = p->indexOf(p->activePlaylistId);
	return i >= 0 ? &p->playlists[i] : nullptr;
}

Project *PlaylistController::project()
{
	int i = activeProjectIndex();
	return i >= 0 ? &lib.projects[i] : nullptr;
}

Playlist *PlaylistController::playlist()
{
	Project *p = project();
	if (!p)
		return nullptr;
	int i = p->indexOf(p->activePlaylistId);
	return i >= 0 ? &p->playlists[i] : nullptr;
}

Playlist *PlaylistController::findPlaylist(const QString &playlistId, Project **owner)
{
	for (auto &p : lib.projects) {
		int i = p.indexOf(playlistId);
		if (i >= 0) {
			if (owner)
				*owner = &p;
			return &p.playlists[i];
		}
	}
	return nullptr;
}

const Playlist *PlaylistController::findPlaylist(const QString &playlistId) const
{
	for (const auto &p : lib.projects) {
		int i = p.indexOf(playlistId);
		if (i >= 0)
			return &p.playlists[i];
	}
	return nullptr;
}

bool PlaylistController::selectProject(int index)
{
	if (index < 0 || index >= lib.projects.size())
		return false;
	lib.activeProjectId = lib.projects[index].id;
	changed();
	return true;
}

bool PlaylistController::selectProjectByName(const QString &name)
{
	for (int i = 0; i < lib.projects.size(); i++)
		if (lib.projects[i].name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
			return selectProject(i);
	return false;
}

bool PlaylistController::selectPlaylist(int index)
{
	Project *p = project();
	if (!p || index < 0 || index >= p->playlists.size())
		return false;
	p->activePlaylistId = p->playlists[index].id;
	changed();
	return true;
}

bool PlaylistController::selectPlaylistByName(const QString &name)
{
	const Project *p = activeProject();
	if (!p)
		return false;
	for (int i = 0; i < p->playlists.size(); i++)
		if (p->playlists[i].name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
			return selectPlaylist(i);
	return false;
}

QString PlaylistController::addProject(const QString &name)
{
	Project p;
	p.id = Library::newId();
	p.name = name.trimmed().isEmpty() ? QStringLiteral("New Project") : name.trimmed();
	lib.projects.append(p);
	lib.activeProjectId = p.id;
	changed();
	return p.id;
}

bool PlaylistController::renameProject(int index, const QString &name)
{
	if (index < 0 || index >= lib.projects.size() || name.trimmed().isEmpty())
		return false;
	lib.projects[index].name = name.trimmed();
	changed();
	return true;
}

bool PlaylistController::duplicateProject(int index)
{
	if (index < 0 || index >= lib.projects.size())
		return false;
	Project copy = lib.projects[index];
	copy.id = Library::newId();
	copy.name += QStringLiteral(" (copy)");
	QString oldActive = copy.activePlaylistId;
	for (auto &pl : copy.playlists) {
		QString nid = Library::newId();
		if (pl.id == oldActive)
			copy.activePlaylistId = nid;
		pl.id = nid;
		for (auto &it : pl.items)
			it.id = Library::newId();
	}
	lib.projects.insert(index + 1, copy);
	lib.activeProjectId = copy.id;
	changed();
	return true;
}

bool PlaylistController::deleteProject(int index)
{
	if (index < 0 || index >= lib.projects.size())
		return false;
	for (const auto &pl : lib.projects[index].playlists)
		if (pl.id == playingPlaylist)
			stop();
	lib.projects.removeAt(index);
	if (!lib.projects.isEmpty())
		lib.activeProjectId = lib.projects[qMin(index, (int)lib.projects.size() - 1)].id;
	changed();
	return true;
}

bool PlaylistController::importProject(const QString &path)
{
	Project p;
	if (!Library::importProject(path, &p))
		return false;
	lib.projects.append(p);
	lib.activeProjectId = p.id;
	changed();
	return true;
}

bool PlaylistController::exportProject(int index, const QString &path) const
{
	if (index < 0 || index >= lib.projects.size())
		return false;
	return Library::exportProject(lib.projects[index], path);
}

QString PlaylistController::addPlaylist(const QString &name)
{
	Project *p = project();
	if (!p)
		return {};
	Playlist pl;
	pl.id = Library::newId();
	pl.name = name.trimmed().isEmpty() ? QStringLiteral("New Playlist") : name.trimmed();
	p->playlists.append(pl);
	p->activePlaylistId = pl.id;
	changed();
	return pl.id;
}

bool PlaylistController::renamePlaylist(int index, const QString &name)
{
	Project *p = project();
	if (!p || index < 0 || index >= p->playlists.size() || name.trimmed().isEmpty())
		return false;
	p->playlists[index].name = name.trimmed();
	changed();
	return true;
}

bool PlaylistController::duplicatePlaylist(int index)
{
	Project *p = project();
	if (!p || index < 0 || index >= p->playlists.size())
		return false;
	Playlist copy = p->playlists[index];
	copy.id = Library::newId();
	copy.name += QStringLiteral(" (copy)");
	for (auto &it : copy.items)
		it.id = Library::newId();
	p->playlists.insert(index + 1, copy);
	p->activePlaylistId = copy.id;
	changed();
	return true;
}

bool PlaylistController::deletePlaylist(int index)
{
	Project *p = project();
	if (!p || index < 0 || index >= p->playlists.size())
		return false;
	if (p->playlists[index].id == playingPlaylist)
		stop();
	p->playlists.removeAt(index);
	if (!p->playlists.isEmpty())
		p->activePlaylistId = p->playlists[qMin(index, (int)p->playlists.size() - 1)].id;
	changed();
	return true;
}

int PlaylistController::addFiles(const QStringList &paths, int insertAt)
{
	Playlist *pl = playlist();
	if (!pl)
		return 0;
	const QStringList exts = Library::mediaExtensions();
	int added = 0;
	int pos = (insertAt < 0 || insertAt > pl->items.size()) ? pl->items.size() : insertAt;
	for (const QString &path : paths) {
		QFileInfo fi(path);
		if (!fi.isFile() || !exts.contains(fi.suffix().toLower()))
			continue;
		PlaylistItem it;
		it.id = Library::newId();
		it.path = fi.absoluteFilePath();
		it.name = Library::displayNameFromPath(path);
		pl->items.insert(pos++, it);
		added++;
	}
	if (added)
		changed();
	return added;
}

void PlaylistController::removeItems(const QStringList &itemIds)
{
	Playlist *pl = playlist();
	if (!pl || itemIds.isEmpty())
		return;
	if (pl->id == playingPlaylist && itemIds.contains(playingItem))
		stop();
	for (int i = pl->items.size() - 1; i >= 0; i--)
		if (itemIds.contains(pl->items[i].id))
			pl->items.removeAt(i);
	changed();
}

void PlaylistController::reorderItems(const QStringList &orderedIds)
{
	Playlist *pl = playlist();
	if (!pl || orderedIds.size() != pl->items.size())
		return;
	QVector<PlaylistItem> reordered;
	for (const QString &id : orderedIds) {
		int i = pl->indexOf(id);
		if (i < 0)
			return; // out of sync, ignore
		reordered.append(pl->items[i]);
	}
	pl->items = reordered;
	changed();
}

void PlaylistController::moveItem(const QString &itemId, int delta)
{
	Playlist *pl = playlist();
	if (!pl)
		return;
	int from = pl->indexOf(itemId);
	int to = from + delta;
	if (from < 0 || to < 0 || to >= pl->items.size())
		return;
	pl->items.move(from, to);
	changed();
}

bool PlaylistController::renameItem(const QString &itemId, const QString &name)
{
	Playlist *pl = playlist();
	if (!pl || name.trimmed().isEmpty())
		return false;
	int i = pl->indexOf(itemId);
	if (i < 0)
		return false;
	pl->items[i].name = name.trimmed();
	changed();
	if (itemId == playingItem)
		emit playbackChanged();
	return true;
}

void PlaylistController::setLoop(bool on)
{
	if (Playlist *pl = playlist()) {
		pl->loop = on;
		changed();
	}
}

void PlaylistController::setAutoAdvance(bool on)
{
	if (Playlist *pl = playlist()) {
		pl->autoAdvance = on;
		changed();
	}
}

void PlaylistController::setReturnAfterBreak(bool on)
{
	lib.returnAfterBreak = on;
	changed();
}

void PlaylistController::setMonitorAudio(bool on)
{
	lib.monitorAudio = on;
	OBSSourceAutoRelease player = findPlayer();
	if (player)
		obs_source_set_monitoring_type(player, on ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT
							  : OBS_MONITORING_TYPE_NONE);
	changed();
}

/* ============================================================
 * Player source & break scene
 * ============================================================ */

static bool isOurPlayer(obs_source_t *src)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(src);
	return obs_data_get_bool(priv, kPlayerFlag);
}

OBSSourceAutoRelease PlaylistController::findPlayer() const
{
	if (weakPlayer) {
		obs_source_t *src = obs_weak_source_get_source(weakPlayer);
		if (src && !obs_source_removed(src))
			return OBSSourceAutoRelease(src);
		obs_source_release(src);
	}

	obs_source_t *found = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (!obs_source_removed(src) && isOurPlayer(src)) {
				*static_cast<obs_source_t **>(param) = obs_source_get_ref(src);
				return false;
			}
			return true;
		},
		&found);
	return OBSSourceAutoRelease(found);
}

static obs_sceneitem_t *findItemForSource(obs_scene_t *scene, obs_source_t *source)
{
	struct Ctx {
		obs_source_t *src;
		obs_sceneitem_t *item;
	} ctx{source, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			if (obs_sceneitem_get_source(item) == c->src) {
				c->item = item;
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.item;
}

static void addFitted(obs_scene_t *scene, obs_source_t *source)
{
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (!item)
		return;
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	vec2 bounds;
	vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
	vec2 pos;
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &pos);
}

OBSSourceAutoRelease PlaylistController::ensureBreakScene(obs_source_t *player)
{
	OBSSourceAutoRelease sceneSrc = obs_get_source_by_name(lib.breakSceneName.toUtf8().constData());
	obs_scene_t *scene = sceneSrc ? obs_scene_from_source(sceneSrc) : nullptr;

	if (!scene) {
		QString name = lib.breakSceneName;
		for (int n = 2; sceneSrc; n++) { // a non-scene already owns the name
			name = QStringLiteral("%1 %2").arg(lib.breakSceneName).arg(n);
			sceneSrc = obs_get_source_by_name(name.toUtf8().constData());
		}
		OBSSceneAutoRelease created = obs_scene_create(name.toUtf8().constData());
		scene = created.Get();
		sceneSrc = obs_source_get_ref(obs_scene_get_source(scene));
		obs_log(LOG_INFO, "Created break scene '%s'", name.toUtf8().constData());
	}

	if (player && !findItemForSource(scene, player))
		addFitted(scene, player);

	return sceneSrc;
}

OBSSourceAutoRelease PlaylistController::ensurePlayer()
{
	OBSSourceAutoRelease player = findPlayer();
	if (!player) {
		QString name = QString::fromUtf8(kPlayerName);
		for (int n = 2;; n++) {
			OBSSourceAutoRelease clash = obs_get_source_by_name(name.toUtf8().constData());
			if (!clash)
				break;
			name = QStringLiteral("%1 %2").arg(QString::fromUtf8(kPlayerName)).arg(n);
		}

		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", "");
		obs_data_set_bool(settings, "looping", false);
		obs_data_set_bool(settings, "restart_on_activate", false);
		obs_data_set_bool(settings, "close_when_inactive", false);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		obs_data_set_bool(settings, "hw_decode", true);

		player = obs_source_create("ffmpeg_source", name.toUtf8().constData(), settings, nullptr);
		if (!player) {
			obs_log(LOG_ERROR, "Could not create the player Media Source");
			return player;
		}
		OBSDataAutoRelease priv = obs_source_get_private_settings(player);
		obs_data_set_bool(priv, kPlayerFlag, true);
		obs_source_set_monitoring_type(player, lib.monitorAudio ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT
									: OBS_MONITORING_TYPE_NONE);

		// a source only survives if a scene holds it, so give it a home right away
		OBSSourceAutoRelease scene = ensureBreakScene(player);
		obs_log(LOG_INFO, "Created player source '%s'", name.toUtf8().constData());
	}
	attachPlayer(player);
	return player;
}

bool PlaylistController::hasPlayerSource() const
{
	OBSSourceAutoRelease player = findPlayer();
	return player.Get() != nullptr;
}

void PlaylistController::attachPlayer(obs_source_t *player)
{
	if (weakPlayer && obs_weak_source_references_source(weakPlayer, player))
		return;
	detachPlayer();
	weakPlayer = obs_source_get_weak_source(player);
	signal_handler_connect(obs_source_get_signal_handler(player), "media_ended", mediaEndedCallback, this);
}

void PlaylistController::detachPlayer()
{
	if (!weakPlayer)
		return;
	obs_source_t *src = obs_weak_source_get_source(weakPlayer);
	if (src) {
		signal_handler_disconnect(obs_source_get_signal_handler(src), "media_ended", mediaEndedCallback,
					  this);
		obs_source_release(src);
	}
	weakPlayer = nullptr;
}

void PlaylistController::onCollectionLoaded()
{
	OBSSourceAutoRelease player = findPlayer();
	if (player) {
		attachPlayer(player);
		// never auto-play the last clip when OBS opens
		OBSDataAutoRelease s = obs_data_create();
		obs_data_set_string(s, "local_file", "");
		obs_source_update(player, s);
	}
	emit playbackChanged();
}

void PlaylistController::onCollectionCleanup()
{
	playState = State::Stopped;
	breakActive = false;
	playingItem.clear();
	playingPlaylist.clear();
	weakPreviousScene = nullptr;
	detachPlayer();
	emit playbackChanged();
}

void PlaylistController::mediaEndedCallback(void *data, calldata_t *)
{
	auto *self = static_cast<PlaylistController *>(data);
	QMetaObject::invokeMethod(self, "handleMediaEnded", Qt::QueuedConnection);
}

/* ============================================================
 * Playback
 * ============================================================ */

void PlaylistController::loadFile(obs_source_t *player, const QString &path)
{
	OBSDataAutoRelease cur = obs_source_get_settings(player);
	bool same = QString::fromUtf8(obs_data_get_string(cur, "local_file")) == path;

	obs_source_media_stop(player);
	sinceLoad.restart();

	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "clear_on_media_end", false);

	if (same) {
		// the Media Source ignores an unchanged path, so bounce it to force a fresh start
		obs_data_set_string(s, "local_file", "");
		obs_source_update(player, s);
	}
	obs_data_set_string(s, "local_file", path.toUtf8().constData());
	obs_source_update(player, s);
}

/* Starts the first playable clip from index, stepping by direction past missing files. */
bool PlaylistController::startAt(const Playlist &pl, int index, int direction)
{
	if (pl.items.isEmpty())
		return false;
	OBSSourceAutoRelease player = ensurePlayer();
	if (!player)
		return false;

	int n = pl.items.size();
	for (int tries = 0; tries < n; tries++) {
		if (index < 0 || index >= n) {
			if (!pl.loop)
				return false;
			index = (index + n) % n;
		}
		const PlaylistItem &it = pl.items[index];
		if (QFileInfo::exists(it.path)) {
			stopToken++;
			loadFile(player, it.path);
			playingPlaylist = pl.id;
			playingItem = it.id;
			obs_log(LOG_INFO, "Playing '%s' (%d/%d in '%s')", it.name.toUtf8().constData(), index + 1, n,
				pl.name.toUtf8().constData());
			setState(State::Playing);
			return true;
		}
		obs_log(LOG_WARNING, "Skipping missing file: %s", it.path.toUtf8().constData());
		index += direction;
	}
	return false;
}

void PlaylistController::setState(State s)
{
	playState = s;
	emit playbackChanged();
}

void PlaylistController::playItem(const QString &itemId)
{
	const Playlist *pl = activePlaylist();
	if (!pl)
		return;
	int i = pl->indexOf(itemId);
	if (i >= 0)
		playIndex(i);
}

void PlaylistController::playIndex(int index)
{
	const Playlist *pl = activePlaylist();
	if (!pl || index < 0 || index >= pl->items.size())
		return;
	if (!startAt(*pl, index, +1))
		stop();
}

void PlaylistController::play()
{
	OBSSourceAutoRelease player = findPlayer();
	if (playState == State::Paused && player) {
		obs_source_media_play_pause(player, false);
		setState(State::Playing);
		return;
	}
	if (playState == State::Playing)
		return;
	playIndex(0);
}

void PlaylistController::pause()
{
	OBSSourceAutoRelease player = findPlayer();
	if (playState != State::Playing || !player)
		return;
	obs_source_media_play_pause(player, true);
	setState(State::Paused);
}

void PlaylistController::playPause()
{
	if (playState == State::Playing)
		pause();
	else
		play();
}

void PlaylistController::stop()
{
	stopToken++;
	OBSSourceAutoRelease player = findPlayer();
	if (player) {
		obs_source_media_stop(player);
		OBSDataAutoRelease s = obs_data_create();
		obs_data_set_string(s, "local_file", "");
		obs_source_update(player, s);
	}
	playingItem.clear();
	playingPlaylist.clear();
	setState(State::Stopped);
}

void PlaylistController::next()
{
	const Playlist *pl = findPlaylist(playingPlaylist);
	if (!pl) {
		play();
		return;
	}
	int i = pl->indexOf(playingItem);
	if (!startAt(*pl, i + 1, +1))
		finishPlaylist();
}

void PlaylistController::previous()
{
	const Playlist *pl = findPlaylist(playingPlaylist);
	if (!pl) {
		play();
		return;
	}
	int i = pl->indexOf(playingItem);
	// more than 3 seconds in: restart this clip, like any media player
	if (timeMs() > 3000 || i <= 0) {
		restart();
		return;
	}
	startAt(*pl, i - 1, -1);
}

void PlaylistController::restart()
{
	const Playlist *pl = findPlaylist(playingPlaylist);
	if (!pl) {
		play();
		return;
	}
	int i = qMax(0, pl->indexOf(playingItem));
	startAt(*pl, i, +1);
}

void PlaylistController::finishPlaylist()
{
	if (breakActive)
		endBreak();
	else
		stop();
}

void PlaylistController::handleMediaEnded()
{
	if (playState != State::Playing || sinceLoad.elapsed() < kIgnoreEndedMs)
		return;
	OBSSourceAutoRelease player = findPlayer();
	if (!player || obs_source_media_get_state(player) != OBS_MEDIA_STATE_ENDED)
		return;

	const Playlist *pl = findPlaylist(playingPlaylist);
	if (!pl) {
		finishPlaylist();
		return;
	}
	if (!pl->autoAdvance) {
		if (breakActive)
			endBreak();
		else
			setState(State::Stopped); // hold the last frame, operator decides
		return;
	}
	int i = pl->indexOf(playingItem);
	if (!startAt(*pl, i + 1, +1))
		finishPlaylist();
}

void PlaylistController::takeBreak()
{
	const Playlist *pl = activePlaylist();
	if (!pl || pl->items.isEmpty())
		return;

	OBSSourceAutoRelease player = ensurePlayer();
	if (!player)
		return;
	OBSSourceAutoRelease breakScene = ensureBreakScene(player);

	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	if (program && program.Get() != breakScene.Get())
		weakPreviousScene = obs_source_get_weak_source(program);

	if (!startAt(*pl, 0, +1))
		return;

	breakActive = true;
	obs_frontend_set_current_scene(breakScene);
	emit playbackChanged();
}

void PlaylistController::endBreak()
{
	if (!breakActive) {
		stop();
		return;
	}
	breakActive = false;

	OBSSourceAutoRelease prev =
		weakPreviousScene ? obs_weak_source_get_source(weakPreviousScene) : nullptr;
	weakPreviousScene = nullptr;

	if (lib.returnAfterBreak && prev && !obs_source_removed(prev)) {
		obs_frontend_set_current_scene(prev);
		// let the transition finish on the clip before clearing it
		quint64 token = ++stopToken;
		int wait = obs_frontend_get_transition_duration() + 150;
		QTimer::singleShot(wait, this, [this, token]() {
			if (token == stopToken)
				stop();
		});
		emit playbackChanged();
	} else {
		stop();
	}
}

void PlaylistController::toggleBreak()
{
	if (breakActive)
		endBreak();
	else
		takeBreak();
}

void PlaylistController::addPlayerToCurrentScene()
{
	OBSSourceAutoRelease player = ensurePlayer();
	if (!player)
		return;
	OBSSourceAutoRelease target = obs_frontend_preview_program_mode_active()
					       ? obs_frontend_get_current_preview_scene()
					       : obs_frontend_get_current_scene();
	obs_scene_t *scene = target ? obs_scene_from_source(target) : nullptr;
	if (scene && !findItemForSource(scene, player))
		addFitted(scene, player);
}

/* ============================================================
 * Status
 * ============================================================ */

QString PlaylistController::playingName() const
{
	const Playlist *pl = findPlaylist(playingPlaylist);
	if (!pl)
		return {};
	int i = pl->indexOf(playingItem);
	return i >= 0 ? pl->items[i].name : QString();
}

int PlaylistController::playingIndex() const
{
	const Playlist *pl = findPlaylist(playingPlaylist);
	return pl ? pl->indexOf(playingItem) : -1;
}

int64_t PlaylistController::timeMs() const
{
	if (playingItem.isEmpty())
		return 0;
	OBSSourceAutoRelease player = findPlayer();
	return player ? obs_source_media_get_time(player) : 0;
}

int64_t PlaylistController::durationMs() const
{
	if (playingItem.isEmpty())
		return 0;
	OBSSourceAutoRelease player = findPlayer();
	return player ? obs_source_media_get_duration(player) : 0;
}

QJsonObject PlaylistController::stateJson() const
{
	QJsonObject o;
	const char *st = playState == State::Playing ? "playing" : playState == State::Paused ? "paused" : "stopped";
	o["state"] = st;
	o["onBreak"] = breakActive ? "true" : "false";

	const Project *p = activeProject();
	const Playlist *sel = activePlaylist();
	o["project"] = p ? p->name : QString();
	o["selectedPlaylist"] = sel ? sel->name : QString();

	const Playlist *playing = findPlaylist(playingPlaylist);
	o["playlist"] = playing ? playing->name : QString();
	o["item"] = playingName();
	int idx = playingIndex();
	o["index"] = idx >= 0 ? QString::number(idx + 1) : QString();
	o["count"] = playing ? QString::number(playing->items.size()) : QString();
	return o;
}
