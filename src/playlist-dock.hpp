/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QListWidget>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QToolButton;
class PlaylistController;

/* List that accepts files dragged in from Explorer/Finder and lets the
 * operator drag rows to reorder. */
class ClipList : public QListWidget {
	Q_OBJECT
public:
	explicit ClipList(QWidget *parent = nullptr);

signals:
	void filesDropped(const QStringList &paths, int row);
	void orderChanged();

protected:
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dragMoveEvent(QDragMoveEvent *e) override;
	void dropEvent(QDropEvent *e) override;
};

class PlaylistDock : public QWidget {
	Q_OBJECT

public:
	explicit PlaylistDock(QWidget *parent = nullptr);

private slots:
	void rebuild();
	void refreshPlayback();
	void tick();

	void onProjectChosen(int index);
	void onPlaylistChosen(int index);
	void addClips();
	void removeSelected();
	void renameSelected();
	void moveSelected(int delta);
	void playSelectedOrResume();
	void showClipMenu(const QPoint &pos);

private:
	void applyRowLook(QListWidgetItem *row);
	QString askName(const QString &title, const QString &label, const QString &current);
	QStringList selectedIds() const;
	QString currentId() const;
	static QString formatTime(int64_t ms);

	PlaylistController *ctrl;
	bool rebuilding = false;

	QComboBox *projectCombo;
	QToolButton *projectMenuBtn;
	QComboBox *playlistCombo;
	QToolButton *playlistMenuBtn;
	ClipList *list;

	QPushButton *addBtn;
	QPushButton *removeBtn;
	QPushButton *upBtn;
	QPushButton *downBtn;

	QLabel *nowLabel;
	QLabel *timeLabel;
	QProgressBar *progress;

	QPushButton *prevBtn;
	QPushButton *playBtn;
	QPushButton *stopBtn;
	QPushButton *nextBtn;
	QPushButton *breakBtn;

	QCheckBox *autoAdvanceChk;
	QCheckBox *loopChk;
	QComboBox *afterBreakCombo;
	QCheckBox *monitorChk;

	QTimer *timer;
};
