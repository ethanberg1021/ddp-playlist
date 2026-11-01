/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "player-decks.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QTimer>

static const char *kOutputFlag = "ddp_playlist_output"; // private setting on the output scene
static const char *kDeckFlag = "ddp_playlist_deck";     // private setting on each deck: 1 or 2
static const char *kLegacyFlag = "ddp_playlist_player"; // the single player from 1.0 / 1.1
static const char *kOutputName = "DDP Playlist Player";
static const char *kDeckNames[2] = {"DDP Playlist Deck A", "DDP Playlist Deck B"};

static const int64_t kSettleNs = 150 * 1000000LL; // preloaded frame is ready by then
static const qint64 kTakeTimeoutMs = 3000;        // cut anyway if a file is slow to open
static const qint64 kIgnoreEndedMs = 500;          // "ended" noise right after a cut
static const int kOldDeckHoldMs = 120;             // old deck stays under the new one this long

static std::atomic<obs_source_t *> g_deckPtr[2]; // identity only, never dereferenced
static std::atomic<bool> g_toggle{false};

/* ---------- small helpers ---------- */

static bool flagOf(obs_source_t *src, const char *flag, long long *value = nullptr)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(src);
	if (!obs_data_has_user_value(priv, flag))
		return false;
	if (value)
		*value = obs_data_get_int(priv, flag);
	return true;
}

static obs_source_t *findScene(const char *flag)
{
	struct Ctx {
		const char *flag;
		obs_source_t *found;
	} ctx{flag, nullptr};
	obs_enum_scenes(
		[](void *p, obs_source_t *src) {
			auto *c = static_cast<Ctx *>(p);
			if (!obs_source_removed(src) && flagOf(src, c->flag)) {
				c->found = obs_source_get_ref(src);
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

static obs_source_t *findSource(const char *flag, long long want)
{
	struct Ctx {
		const char *flag;
		long long want;
		obs_source_t *found;
	} ctx{flag, want, nullptr};
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			auto *c = static_cast<Ctx *>(p);
			long long v = 0;
			if (!obs_source_removed(src) && flagOf(src, c->flag, &v) && (c->want < 0 || v == c->want)) {
				c->found = obs_source_get_ref(src);
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

static obs_sceneitem_t *itemFor(obs_scene_t *scene, obs_source_t *source)
{
	struct Ctx {
		obs_source_t *src;
		obs_sceneitem_t *item;
	} ctx{source, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			auto *c = static_cast<Ctx *>(p);
			if (obs_sceneitem_get_source(item) == c->src) {
				c->item = item;
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.item;
}

static void fitToCanvas(obs_sceneitem_t *item)
{
	if (!item)
		return;
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	vec2 bounds, pos;
	vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &pos);
}

static QString uniqueName(const QString &base)
{
	QString name = base;
	for (int n = 2;; n++) {
		OBSSourceAutoRelease clash = obs_get_source_by_name(name.toUtf8().constData());
		if (!clash)
			return name;
		name = QStringLiteral("%1 %2").arg(base).arg(n);
	}
}

static void setMonitor(obs_source_t *src, bool on)
{
	obs_monitoring_type want = on ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT : OBS_MONITORING_TYPE_NONE;
	if (obs_source_get_monitoring_type(src) != want)
		obs_source_set_monitoring_type(src, want);
}

/* A harmless option that alternates on every load, so the Media Source always
 * reopens the file, even when it's the same clip as last time. */
static const char *nextToggle()
{
	bool t = !g_toggle.load();
	g_toggle = t;
	return t ? "probesize=5000000" : "probesize=5000001";
}

/* Waiting deck: open the file and preload frame 1, but don't start.
 * (restart_on_activate + not visible = OBS opens without playing; with
 * clear_on_media_end it keeps that first frame ready to show instantly.) */
static void waitingSettings(obs_data_t *s, const QString &path)
{
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", path.toUtf8().constData());
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", true);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "clear_on_media_end", true);
	obs_data_set_string(s, "ffmpeg_options", nextToggle());
}

/* ---------- lifetime ---------- */

PlayerDecks::PlayerDecks(QObject *parent) : QObject(parent)
{
	cueTimer = new QTimer(this);
	cueTimer->setInterval(10);
	connect(cueTimer, &QTimer::timeout, this, &PlayerDecks::pollCue);
	for (int i = 0; i < 2; i++) {
		loadedNs[i] = 0;
		armed[i] = false;
		g_deckPtr[i] = nullptr;
	}
	sinceTake.start();
	obs_add_tick_callback(tickCallback, this);
	tickAdded = true;
}

PlayerDecks::~PlayerDecks()
{
	if (tickAdded)
		obs_remove_tick_callback(tickCallback, this);
	detach();
}

OBSSourceAutoRelease PlayerDecks::deck(int i) const
{
	if (!weakDecks[i])
		return nullptr;
	obs_source_t *src = obs_weak_source_get_source(weakDecks[i]);
	if (src && obs_source_removed(src)) {
		obs_source_release(src);
		return nullptr;
	}
	return OBSSourceAutoRelease(src);
}

OBSSourceAutoRelease PlayerDecks::output() const
{
	if (!weakOutput)
		return nullptr;
	obs_source_t *src = obs_weak_source_get_source(weakOutput);
	if (src && obs_source_removed(src)) {
		obs_source_release(src);
		return nullptr;
	}
	return OBSSourceAutoRelease(src);
}

bool PlayerDecks::exists() const
{
	OBSSourceAutoRelease out = output();
	OBSSourceAutoRelease a = deck(0);
	OBSSourceAutoRelease b = deck(1);
	return out && a && b;
}

void PlayerDecks::refreshItems()
{
	OBSSourceAutoRelease out = output();
	obs_scene_t *scene = out ? obs_scene_from_source(out) : nullptr;
	std::lock_guard<std::mutex> g(lock);
	for (int i = 0; i < 2; i++) {
		OBSSourceAutoRelease d = deck(i);
		decksStrong[i] = d.Get();
		items[i] = (scene && d) ? itemFor(scene, d) : nullptr;
	}
}

void PlayerDecks::connectDeck(obs_source_t *src)
{
	signal_handler_t *sh = obs_source_get_signal_handler(src);
	signal_handler_connect(sh, "media_ended", signalCallback, this);
	signal_handler_connect(sh, "update", signalCallback, this);
	signal_handler_connect(sh, "volume", volumeCallback, this);
	signal_handler_connect(sh, "mute", muteCallback, this);
}

void PlayerDecks::disconnectDeck(obs_source_t *src)
{
	signal_handler_t *sh = obs_source_get_signal_handler(src);
	signal_handler_disconnect(sh, "media_ended", signalCallback, this);
	signal_handler_disconnect(sh, "update", signalCallback, this);
	signal_handler_disconnect(sh, "volume", volumeCallback, this);
	signal_handler_disconnect(sh, "mute", muteCallback, this);
}

void PlayerDecks::detach()
{
	cueTimer->stop();
	takePending = false;
	cuedPath.clear();
	autoTake = false;
	for (int i = 0; i < 2; i++) {
		armed[i] = false;
		OBSSourceAutoRelease d = deck(i);
		if (d)
			disconnectDeck(d);
		weakDecks[i] = nullptr;
		g_deckPtr[i] = nullptr;
	}
	weakOutput = nullptr;
	std::lock_guard<std::mutex> g(lock);
	for (int i = 0; i < 2; i++) {
		decksStrong[i] = nullptr;
		items[i] = nullptr;
	}
}

/* Shared tail of reattach() and ensure(): put the decks in a known state
 * (A showing and enabled, B hidden and silent). */
static void resetLayout(obs_source_t *a, obs_source_t *b, OBSSceneItem *items, bool monitor)
{
	obs_source_set_enabled(a, true);
	obs_source_set_enabled(b, false);
	setMonitor(a, monitor);
	setMonitor(b, monitor);
	if (items[0])
		obs_sceneitem_set_visible(items[0], true);
	if (items[1])
		obs_sceneitem_set_visible(items[1], false);
}

void PlayerDecks::reattach()
{
	detach();
	OBSSourceAutoRelease out = findScene(kOutputFlag);
	OBSSourceAutoRelease a = findSource(kDeckFlag, 1);
	OBSSourceAutoRelease b = findSource(kDeckFlag, 2);
	if (!out || !a || !b)
		return; // created on first use (or upgraded from the old player) by ensure()

	weakOutput = obs_source_get_weak_source(out);
	weakDecks[0] = obs_source_get_weak_source(a);
	weakDecks[1] = obs_source_get_weak_source(b);
	g_deckPtr[0] = a.Get();
	g_deckPtr[1] = b.Get();
	connectDeck(a);
	connectDeck(b);
	refreshItems();
	onAir = 0;
	resetLayout(a, b, items, monitor);
	stopAll();
}

OBSSourceAutoRelease PlayerDecks::ensure(bool monitorAudio)
{
	monitor = monitorAudio;
	if (exists())
		return output();

	detach();

	OBSSourceAutoRelease decks[2] = {findSource(kDeckFlag, 1), findSource(kDeckFlag, 2)};
	OBSSourceAutoRelease out = findScene(kOutputFlag);

	/* Upgrade from 1.0/1.1: the old single player becomes Deck A and every scene
	 * that showed it now shows the two-deck output scene in the same spot. */
	OBSSourceAutoRelease legacy = decks[0] ? nullptr : findSource(kLegacyFlag, -1);
	if (legacy) {
		obs_source_set_name(legacy, uniqueName(kDeckNames[0]).toUtf8().constData());
		OBSDataAutoRelease priv = obs_source_get_private_settings(legacy);
		obs_data_erase(priv, kLegacyFlag);
		obs_data_set_int(priv, kDeckFlag, 1);
		decks[0] = obs_source_get_ref(legacy);
		obs_log(LOG_INFO, "Upgrading the 1.x player to two decks");
	}

	for (int i = 0; i < 2; i++) {
		if (decks[i])
			continue;
		OBSDataAutoRelease s = obs_data_create();
		waitingSettings(s, QString());
		obs_data_set_bool(s, "hw_decode", true);
		obs_data_set_bool(s, "log_changes", false);
		decks[i] = obs_source_create("ffmpeg_source", uniqueName(kDeckNames[i]).toUtf8().constData(), s,
					     nullptr);
		if (!decks[i]) {
			obs_log(LOG_ERROR, "Could not create a playlist deck");
			return nullptr;
		}
		OBSDataAutoRelease priv = obs_source_get_private_settings(decks[i]);
		obs_data_set_int(priv, kDeckFlag, i + 1);
	}

	// the two decks share one volume; start B where A is
	obs_source_set_volume(decks[1], obs_source_get_volume(decks[0]));
	obs_source_set_muted(decks[1], obs_source_muted(decks[0]));

	if (!out) {
		OBSSceneAutoRelease scene = obs_scene_create(uniqueName(kOutputName).toUtf8().constData());
		out = obs_source_get_ref(obs_scene_get_source(scene));
		OBSDataAutoRelease priv = obs_source_get_private_settings(out);
		obs_data_set_bool(priv, kOutputFlag, true);
	}
	obs_scene_t *outScene = obs_scene_from_source(out);
	for (int i = 0; i < 2; i++) {
		if (!itemFor(outScene, decks[i]))
			fitToCanvas(obs_scene_add(outScene, decks[i]));
	}

	if (legacy) {
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *src = scenes.sources.array[i];
			obs_scene_t *scene = obs_scene_from_source(src);
			if (!scene || src == out.Get())
				continue;
			obs_sceneitem_t *old = itemFor(scene, decks[0]);
			if (!old)
				continue;
			obs_transform_info info;
			obs_sceneitem_crop crop;
			obs_sceneitem_get_info2(old, &info);
			obs_sceneitem_get_crop(old, &crop);
			bool visible = obs_sceneitem_visible(old);
			int position = obs_sceneitem_get_order_position(old);

			obs_sceneitem_t *fresh = obs_scene_add(scene, out);
			if (fresh) {
				obs_sceneitem_set_info2(fresh, &info);
				obs_sceneitem_set_crop(fresh, &crop);
				obs_sceneitem_set_visible(fresh, visible);
				obs_sceneitem_set_order_position(fresh, position);
				obs_sceneitem_remove(old);
				obs_log(LOG_INFO, "Swapped the player in scene '%s'", obs_source_get_name(src));
			}
		}
		obs_frontend_source_list_free(&scenes);
	}

	weakOutput = obs_source_get_weak_source(out);
	for (int i = 0; i < 2; i++) {
		weakDecks[i] = obs_source_get_weak_source(decks[i]);
		g_deckPtr[i] = decks[i].Get();
		connectDeck(decks[i]);
	}
	refreshItems();
	onAir = 0;
	resetLayout(decks[0], decks[1], items, monitor);
	obs_log(LOG_INFO, "Two-deck player ready");
	return out;
}

/* ---------- cueing ---------- */

bool PlayerDecks::cueReady(int i) const
{
	int64_t loaded = loadedNs[i];
	return loaded > 0 && (int64_t)os_gettime_ns() - loaded >= kSettleNs;
}

void PlayerDecks::loadWaiting(obs_source_t *src, const QString &path)
{
	OBSDataAutoRelease s = obs_data_create();
	waitingSettings(s, path);
	obs_source_update(src, s);
}

void PlayerDecks::cue(const QString &path, bool wantAutoTake)
{
	int standby = 1 - onAir;
	OBSSourceAutoRelease d = deck(standby);
	if (!d || path.isEmpty())
		return;
	if (path == cuedPath) {
		autoTake = wantAutoTake;
		return;
	}
	if (takePending && !cuedPath.isEmpty())
		return; // a cut is waiting on this deck; don't swap the clip under it

	/* Right after a cut, the old deck is still the safety net under the new
	 * one. Load into it once that moment has passed. */
	qint64 holdLeft = kOldDeckHoldMs + 30 - sinceTake.elapsed();
	if (holdLeft > 0) {
		quint64 token = takeCount;
		QTimer::singleShot((int)holdLeft, this, [this, path, wantAutoTake, token]() {
			if (token == takeCount)
				cue(path, wantAutoTake);
		});
		return;
	}

	autoTake = false;
	{
		std::lock_guard<std::mutex> g(lock);
		if (items[standby])
			obs_sceneitem_set_visible(items[standby], false);
	}
	obs_source_set_enabled(d, false); // silent everywhere, headphones included, until its cut
	obs_source_media_stop(d);         // and holding no stale picture
	cuedPath = path;
	cuedAt.start();
	loadedNs[standby] = 0;
	armed[standby] = true;
	loadWaiting(d, path);
	autoTake = wantAutoTake;
}

void PlayerDecks::playNow(const QString &path)
{
	cue(path, false);
	cuedAt.start();
	takePending = true;
	pollCue();
	if (takePending)
		cueTimer->start();
}

void PlayerDecks::pollCue()
{
	if (!takePending) {
		cueTimer->stop();
		return;
	}
	if (cuedPath.isEmpty())
		return; // its load is still queued behind a cut
	if (cueReady(1 - onAir) || cuedAt.elapsed() > kTakeTimeoutMs) {
		cueTimer->stop();
		takePending = false;
		autoTake = false;
		std::lock_guard<std::mutex> g(lock);
		takeLocked(false);
	}
}

/* ---------- the cut ---------- */

void PlayerDecks::takeLocked(bool automatic)
{
	int in = 1 - onAir;
	int out = onAir;
	obs_source_t *din = decksStrong[in];
	obs_sceneitem_t *iin = items[in];
	if (!din || !iin)
		return;

	OBSSourceAutoRelease outScene = output();
	if (!outScene)
		return;
	/* The preloaded first frame is only used when the player is on screen. For
	 * the first spot of a break it isn't yet (we cut to the break scene a moment
	 * later), so count it as showing for a second, like a projector would. */
	obs_source_inc_showing(outScene);

	// new deck on top, audible, visible...
	obs_sceneitem_set_order(iin, OBS_ORDER_MOVE_TOP);
	obs_source_set_enabled(din, true);
	obs_sceneitem_set_visible(iin, true);

	/* ...and started. OBS runs this before it applies the settings change
	 * below, so it starts on the preloaded first frame: no blank frame. */
	obs_source_media_restart(din);
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_bool(s, "clear_on_media_end", false); // hold its last frame until the next cut
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_source_update(din, s);

	// a skipped clip goes quiet now; the old deck itself leaves in afterTake()
	obs_source_t *dout = decksStrong[out];
	if (dout && obs_source_media_get_state(dout) == OBS_MEDIA_STATE_PLAYING)
		obs_source_media_play_pause(dout, true);

	onAir = in;
	loadedNs[out] = 0; // the old deck holds nothing ready any more
	quint64 token = ++takeCount;
	obs_weak_source_t *weakOut = obs_source_get_weak_source(outScene);
	QMetaObject::invokeMethod(
		this,
		[this, out, token, automatic, weakOut]() {
			afterTake(out, token, automatic);
			QTimer::singleShot(1000, this, [weakOut]() {
				obs_source_t *src = obs_weak_source_get_source(weakOut);
				obs_weak_source_release(weakOut);
				if (src)
					obs_source_dec_showing(src);
				obs_source_release(src);
			});
		},
		Qt::QueuedConnection);
}

void PlayerDecks::afterTake(int oldDeck, quint64 token, bool automatic)
{
	sinceTake.restart();
	cuedPath.clear();
	emit tookAir(automatic);

	/* Keep the old deck under the new one for a few frames as a safety net,
	 * then hide and silence it, unless it's already been re-cued. */
	QTimer::singleShot(kOldDeckHoldMs, this, [this, oldDeck, token]() {
		if (token != takeCount || oldDeck == onAir || !cuedPath.isEmpty())
			return;
		std::lock_guard<std::mutex> g(lock);
		if (items[oldDeck])
			obs_sceneitem_set_visible(items[oldDeck], false);
		if (decksStrong[oldDeck])
			obs_source_set_enabled(decksStrong[oldDeck], false);
	});
}

/* Runs on OBS's video thread once per frame. When the on-air clip reaches its
 * end and the next one is preloaded, cut right here: frame-accurate, and no
 * waiting on the UI. */
void PlayerDecks::tickCallback(void *data, float)
{
	auto *self = static_cast<PlayerDecks *>(data);
	if (!self->autoTake)
		return;
	std::unique_lock<std::mutex> g(self->lock, std::try_to_lock);
	if (!g.owns_lock())
		return; // the UI is mid-change; try again next frame

	int on = self->onAir;
	obs_source_t *d = self->decksStrong[on];
	if (!d || !self->decksStrong[1 - on] || !self->cueReady(1 - on))
		return;

	enum obs_media_state st = obs_source_media_get_state(d);
	bool atEnd = st == OBS_MEDIA_STATE_ENDED;
	if (!atEnd && st == OBS_MEDIA_STATE_PLAYING) {
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		int64_t frameMs = ovi.fps_num ? (int64_t)ovi.fps_den * 1000 / ovi.fps_num : 33;
		int64_t dur = obs_source_media_get_duration(d);
		int64_t t = obs_source_media_get_time(d);
		atEnd = dur > 0 && t > 0 && dur - t < frameMs / 2;
	}
	if (!atEnd)
		return;

	if (self->autoTake.exchange(false))
		self->takeLocked(true);
}

/* ---------- transport ---------- */

void PlayerDecks::pause(bool paused)
{
	OBSSourceAutoRelease d = deck(onAir);
	if (d)
		obs_source_media_play_pause(d, paused);
}

void PlayerDecks::stopAll()
{
	cueTimer->stop();
	takePending = false;
	autoTake = false;
	cuedPath.clear();
	++takeCount; // cancels a pending old-deck hide
	for (int i = 0; i < 2; i++) {
		armed[i] = false;
		loadedNs[i] = 0;
		OBSSourceAutoRelease d = deck(i);
		if (!d)
			continue;
		obs_source_media_stop(d); // clears the picture
		OBSDataAutoRelease s = obs_data_create();
		waitingSettings(s, QString());
		obs_source_update(d, s);
		obs_source_set_enabled(d, i == onAir);
	}
	std::lock_guard<std::mutex> g(lock);
	for (int i = 0; i < 2; i++)
		if (items[i])
			obs_sceneitem_set_visible(items[i], i == onAir);
}

void PlayerDecks::setMonitoring(bool on)
{
	monitor = on;
	for (int i = 0; i < 2; i++) {
		OBSSourceAutoRelease d = deck(i);
		if (d)
			setMonitor(d, on);
	}
}

int64_t PlayerDecks::timeMs() const
{
	OBSSourceAutoRelease d = deck(onAir);
	return d ? obs_source_media_get_time(d) : 0;
}

int64_t PlayerDecks::durationMs() const
{
	OBSSourceAutoRelease d = deck(onAir);
	return d ? obs_source_media_get_duration(d) : 0;
}

bool PlayerDecks::onAirEnded() const
{
	OBSSourceAutoRelease d = deck(onAir);
	return d && obs_source_media_get_state(d) == OBS_MEDIA_STATE_ENDED;
}

/* ---------- OBS signals (any thread) ---------- */

void PlayerDecks::signalCallback(void *data, calldata_t *cd)
{
	auto *self = static_cast<PlayerDecks *>(data);
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));

	/* "update" on a deck we're loading: the file is open and frame 1 is being
	 * preloaded. Switch off restart-on-activate now so that showing it later
	 * never triggers a second start. (Same file, so this doesn't reopen it.) */
	for (int i = 0; i < 2; i++) {
		if (g_deckPtr[i].load() != src)
			continue;
		if (self->armed[i].exchange(false)) {
			OBSDataAutoRelease s = obs_data_create();
			obs_data_set_bool(s, "restart_on_activate", false);
			obs_source_update(src, s);
			self->loadedNs[i] = (int64_t)os_gettime_ns();
			return;
		}
	}

	if (obs_source_media_get_state(src) == OBS_MEDIA_STATE_ENDED) {
		quintptr id = (quintptr)src;
		QMetaObject::invokeMethod(self, [self, id]() { self->handleEnded(id); }, Qt::QueuedConnection);
	}
}

void PlayerDecks::handleEnded(quintptr source)
{
	OBSSourceAutoRelease d = deck(onAir);
	if (!d || (quintptr)d.Get() != source || sinceTake.elapsed() < kIgnoreEndedMs)
		return;
	if (obs_source_media_get_state(d) != OBS_MEDIA_STATE_ENDED)
		return;
	if (autoTake && cueReady(1 - onAir)) {
		/* The next clip is ready and set to follow. Normally the video thread
		 * has already cut to it; if it hasn't (it was busy), cut from here.
		 * Whoever gets there first wins; the other sees autoTake is gone. */
		std::lock_guard<std::mutex> g(lock);
		if (autoTake.exchange(false))
			takeLocked(true);
		return;
	}
	emit clipEnded();
}

void PlayerDecks::volumeCallback(void *data, calldata_t *cd)
{
	auto *self = static_cast<PlayerDecks *>(data);
	if (self->syncing)
		return;
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	float vol = (float)calldata_float(cd, "volume");
	self->syncing = true;
	for (int i = 0; i < 2; i++) {
		OBSSourceAutoRelease d = self->deck(i);
		if (d && d.Get() != src && obs_source_get_volume(d) != vol)
			obs_source_set_volume(d, vol);
	}
	self->syncing = false;
}

void PlayerDecks::muteCallback(void *data, calldata_t *cd)
{
	auto *self = static_cast<PlayerDecks *>(data);
	if (self->syncing)
		return;
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	bool muted = calldata_bool(cd, "muted");
	self->syncing = true;
	for (int i = 0; i < 2; i++) {
		OBSSourceAutoRelease d = self->deck(i);
		if (d && d.Get() != src && obs_source_muted(d) != muted)
			obs_source_set_muted(d, muted);
	}
	self->syncing = false;
}
