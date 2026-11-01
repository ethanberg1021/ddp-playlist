/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QStringList>

struct obs_source;
class QTimer;

/* Makes a small preview picture and reads the length of each clip.
 * It opens the file in a hidden Media Source, lets it play for about a
 * second, then renders that frame offscreen with OBS's own renderer, so it
 * needs nothing beyond OBS itself. Results are cached on disk, one clip at a
 * time, so it never competes with the live show. */
class Thumbnailer : public QObject {
	Q_OBJECT

public:
	static Thumbnailer *instance();
	static void create();
	static void destroy();

	void setReady(); // call once OBS has finished loading

	QIcon iconFor(const QString &path);   // cached thumbnail or placeholder; queues work
	int64_t durationMs(const QString &path); // 0 if not known yet

	static QSize iconSize() { return QSize(80, 45); }

signals:
	void thumbnailReady(const QString &path);

private:
	explicit Thumbnailer(QObject *parent = nullptr);
	~Thumbnailer() override;

	struct Info {
		int64_t ms = 0;
		bool video = false;
		bool failed = false;
	};

	QString keyFor(const QString &path) const;
	QString imagePath(const QString &key) const;
	void loadIndex();
	void saveIndex() const;
	void queue(const QString &path);
	void startNext();
	void poll();
	void finish(const QImage &img, int64_t ms, bool failed);
	QImage capture(obs_source *src);
	QIcon placeholder(bool audio) const;

	bool ready = false;
	QString dir;
	QHash<QString, Info> index;    // key -> info
	QHash<QString, QIcon> icons;   // key -> icon (memory cache)
	QStringList pending;           // paths
	QString currentPath;
	obs_source *job = nullptr;
	QElapsedTimer jobTimer;
	QTimer *pollTimer;
};
