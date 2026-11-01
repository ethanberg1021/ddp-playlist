/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "thumbnailer.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QStyle>
#include <QTimer>

static Thumbnailer *g_thumbs = nullptr;

static const int kThumbWidth = 192;    // stored size; the list shows it smaller
static const int kGiveUpMs = 6000;     // per clip
static const int kAudioDecideMs = 2000; // no picture by now + has length = audio file

Thumbnailer *Thumbnailer::instance()
{
	return g_thumbs;
}

void Thumbnailer::create()
{
	if (!g_thumbs)
		g_thumbs = new Thumbnailer();
}

void Thumbnailer::destroy()
{
	delete g_thumbs;
	g_thumbs = nullptr;
}

Thumbnailer::Thumbnailer(QObject *parent) : QObject(parent)
{
	char *path = obs_module_config_path("thumbs");
	dir = QString::fromUtf8(path);
	bfree(path);
	QDir().mkpath(dir);
	loadIndex();

	pollTimer = new QTimer(this);
	pollTimer->setInterval(100);
	connect(pollTimer, &QTimer::timeout, this, &Thumbnailer::poll);
}

Thumbnailer::~Thumbnailer()
{
	pollTimer->stop();
	if (job) {
		obs_source_release(job);
		job = nullptr;
	}
}

void Thumbnailer::setReady()
{
	ready = true;
	startNext();
}

/* ---------- cache ---------- */

QString Thumbnailer::keyFor(const QString &path) const
{
	QFileInfo fi(path);
	QByteArray raw = (fi.absoluteFilePath() + "|" + QString::number(fi.size()) + "|" +
			  QString::number(fi.lastModified().toMSecsSinceEpoch()))
				 .toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha1).toHex());
}

QString Thumbnailer::imagePath(const QString &key) const
{
	return dir + "/" + key + ".jpg";
}

void Thumbnailer::loadIndex()
{
	QFile f(dir + "/index.json");
	if (!f.open(QIODevice::ReadOnly))
		return;
	QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	for (auto it = root.begin(); it != root.end(); ++it) {
		QJsonObject o = it.value().toObject();
		Info info;
		info.ms = (int64_t)o["ms"].toDouble();
		info.video = o["video"].toBool();
		info.failed = o["failed"].toBool();
		index.insert(it.key(), info);
	}
}

void Thumbnailer::saveIndex() const
{
	QJsonObject root;
	for (auto it = index.begin(); it != index.end(); ++it) {
		QJsonObject o;
		o["ms"] = (double)it.value().ms;
		o["video"] = it.value().video;
		o["failed"] = it.value().failed;
		root[it.key()] = o;
	}
	QSaveFile f(dir + "/index.json");
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
		f.commit();
	}
}

QIcon Thumbnailer::placeholder(bool audio) const
{
	QSize s = iconSize();
	QPixmap pm(s);
	pm.fill(QColor(40, 40, 44));
	QPainter p(&pm);
	QIcon glyph = qApp->style()->standardIcon(audio ? QStyle::SP_MediaVolume : QStyle::SP_MediaPlay);
	int g = s.height() / 2;
	glyph.paint(&p, QRect((s.width() - g) / 2, (s.height() - g) / 2, g, g));
	p.end();
	return QIcon(pm);
}

QIcon Thumbnailer::iconFor(const QString &path)
{
	if (!QFileInfo::exists(path))
		return placeholder(false);

	QString key = keyFor(path);
	auto cached = icons.find(key);
	if (cached != icons.end())
		return cached.value();

	auto info = index.find(key);
	if (info != index.end()) {
		if (info->video && QFileInfo::exists(imagePath(key))) {
			QIcon icon(QPixmap::fromImage(QImage(imagePath(key))));
			icons.insert(key, icon);
			return icon;
		}
		if (!info->video) {
			QIcon icon = placeholder(info->ms > 0 && !info->failed);
			icons.insert(key, icon);
			return icon;
		}
	}

	queue(path);
	return placeholder(false);
}

int64_t Thumbnailer::durationMs(const QString &path)
{
	if (!QFileInfo::exists(path))
		return 0;
	auto info = index.find(keyFor(path));
	return info != index.end() ? info->ms : 0;
}

/* ---------- work queue ---------- */

void Thumbnailer::queue(const QString &path)
{
	if (path == currentPath || pending.contains(path))
		return;
	pending.append(path);
	startNext();
}

void Thumbnailer::startNext()
{
	if (!ready || job || pending.isEmpty())
		return;

	currentPath = pending.takeFirst();
	if (!QFileInfo::exists(currentPath)) {
		currentPath.clear();
		QTimer::singleShot(0, this, &Thumbnailer::startNext);
		return;
	}

	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", currentPath.toUtf8().constData());
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", false); // start decoding right away, even hidden
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "clear_on_media_end", false);
	obs_data_set_bool(s, "hw_decode", false);
	obs_data_set_bool(s, "log_changes", false);

	job = obs_source_create_private("ffmpeg_source", "ddp-playlist-thumbnailer", s);
	if (!job) {
		finish(QImage(), 0, true);
		return;
	}
	obs_source_set_volume(job, 0.0f);
	jobTimer.start();
	pollTimer->start();
}

void Thumbnailer::poll()
{
	if (!job) {
		pollTimer->stop();
		return;
	}

	int64_t dur = obs_source_media_get_duration(job);
	int64_t t = obs_source_media_get_time(job);
	uint32_t w = obs_source_get_width(job);
	qint64 elapsed = jobTimer.elapsed();

	// grab a frame a little way in, so we skip black fade-ins
	int64_t target = dur > 0 ? qMin<int64_t>(1500, dur / 4) : 1000;
	bool ended = obs_source_media_get_state(job) == OBS_MEDIA_STATE_ENDED;

	if (w > 0 && (t >= target || ended || elapsed > kGiveUpMs)) {
		QImage img = capture(job);
		finish(img, dur, img.isNull());
	} else if (w == 0 && dur > 0 && elapsed > kAudioDecideMs) {
		finish(QImage(), dur, false); // audio-only file
	} else if (elapsed > kGiveUpMs) {
		finish(QImage(), dur, true);
	}
}

QImage Thumbnailer::capture(obs_source *src)
{
	uint32_t sw = obs_source_get_width(src);
	uint32_t sh = obs_source_get_height(src);
	if (!sw || !sh)
		return QImage();

	int tw = kThumbWidth;
	int th = qMax(1, (int)((double)sh * tw / sw));
	QImage out;

	obs_enter_graphics();
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(tw, th, GS_RGBA);

	gs_texrender_reset(texrender);
	if (gs_texrender_begin(texrender, tw, th)) {
		vec4 background;
		vec4_zero(&background);
		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_inc_showing(src);
		obs_source_video_render(src);
		obs_source_dec_showing(src);
		gs_blend_state_pop();
		gs_texrender_end(texrender);

		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			out = QImage(tw, th, QImage::Format_RGBA8888);
			for (int y = 0; y < th; y++)
				memcpy(out.scanLine(y), data + (size_t)y * linesize, (size_t)tw * 4);
			gs_stagesurface_unmap(stage);
		}
	}

	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();

	// an all-black grab usually means no frame was ready yet; treat as failure
	if (!out.isNull()) {
		bool anything = false;
		for (int y = 0; y < out.height() && !anything; y += 4) {
			const uchar *line = out.constScanLine(y);
			for (int x = 0; x < out.width() * 4; x += 16)
				if (line[x] > 12 || line[x + 1] > 12 || line[x + 2] > 12) {
					anything = true;
					break;
				}
		}
		if (!anything)
			return QImage();
	}
	return out.convertToFormat(QImage::Format_RGB32);
}

void Thumbnailer::finish(const QImage &img, int64_t ms, bool failed)
{
	pollTimer->stop();
	if (job) {
		obs_source_media_stop(job);
		obs_source_release(job);
		job = nullptr;
	}

	QString path = currentPath;
	currentPath.clear();

	if (!path.isEmpty() && QFileInfo::exists(path)) {
		QString key = keyFor(path);
		Info info;
		info.ms = ms > 0 ? ms : 0;
		info.video = !img.isNull();
		info.failed = failed && img.isNull();
		if (info.video)
			img.save(imagePath(key), "JPG", 85);
		index.insert(key, info);
		icons.remove(key);
		saveIndex();
		emit thumbnailReady(path);
	}

	// small gap between clips keeps the load invisible during a show
	QTimer::singleShot(150, this, &Thumbnailer::startNext);
}
