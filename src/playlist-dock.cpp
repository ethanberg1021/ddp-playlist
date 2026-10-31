/*
DDP Playlist Player - Drag Drop Production
Copyright (C) 2026 Drag Drop Production

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "playlist-dock.hpp"
#include "playlist-controller.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QApplication>
#include <QStyle>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

static const char *kGold = "#d4a24c";
static const char *kInk = "#0a0a0a";

/* ============================================================
 * ClipList
 * ============================================================ */

ClipList::ClipList(QWidget *parent) : QListWidget(parent)
{
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	setAlternatingRowColors(true);
	setContextMenuPolicy(Qt::CustomContextMenu);
}

static QStringList localFiles(const QMimeData *mime)
{
	QStringList out;
	if (!mime || !mime->hasUrls())
		return out;
	for (const QUrl &u : mime->urls())
		if (u.isLocalFile())
			out << u.toLocalFile();
	return out;
}

void ClipList::dragEnterEvent(QDragEnterEvent *e)
{
	if (e->source() != this && !localFiles(e->mimeData()).isEmpty()) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
		return;
	}
	QListWidget::dragEnterEvent(e);
}

void ClipList::dragMoveEvent(QDragMoveEvent *e)
{
	if (e->source() != this && !localFiles(e->mimeData()).isEmpty()) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
		return;
	}
	QListWidget::dragMoveEvent(e);
}

void ClipList::dropEvent(QDropEvent *e)
{
	if (e->source() != this) {
		QStringList files = localFiles(e->mimeData());
		if (!files.isEmpty()) {
			int row = indexAt(e->position().toPoint()).row();
			e->setDropAction(Qt::CopyAction);
			e->accept();
			emit filesDropped(files, row);
			return;
		}
	}
	QListWidget::dropEvent(e);
	// let the view finish moving the row before reading the new order
	QTimer::singleShot(0, this, [this]() { emit orderChanged(); });
}

/* ============================================================
 * PlaylistDock
 * ============================================================ */

static QPushButton *transportButton(QStyle::StandardPixmap icon, const QString &tip)
{
	auto *b = new QPushButton();
	b->setIcon(qApp->style()->standardIcon(icon));
	b->setIconSize(QSize(18, 18));
	b->setToolTip(tip);
	b->setMinimumHeight(30);
	b->setFocusPolicy(Qt::NoFocus);
	return b;
}

PlaylistDock::PlaylistDock(QWidget *parent) : QWidget(parent), ctrl(PlaylistController::instance())
{
	setObjectName("DDPPlaylistDock");
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	/* --- project & playlist pickers --- */
	auto *pick = new QGridLayout();
	pick->setHorizontalSpacing(4);
	pick->setVerticalSpacing(4);

	projectCombo = new QComboBox();
	projectCombo->setToolTip("Project (show / client). Each project keeps its own playlists.");
	projectMenuBtn = new QToolButton();
	projectMenuBtn->setText("⋯");
	projectMenuBtn->setToolTip("Project options");
	projectMenuBtn->setPopupMode(QToolButton::InstantPopup);

	playlistCombo = new QComboBox();
	playlistCombo->setToolTip("Playlist (e.g. Commercial Break 1)");
	playlistMenuBtn = new QToolButton();
	playlistMenuBtn->setText("⋯");
	playlistMenuBtn->setToolTip("Playlist options");
	playlistMenuBtn->setPopupMode(QToolButton::InstantPopup);

	pick->addWidget(new QLabel("Project"), 0, 0);
	pick->addWidget(projectCombo, 0, 1);
	pick->addWidget(projectMenuBtn, 0, 2);
	pick->addWidget(new QLabel("Playlist"), 1, 0);
	pick->addWidget(playlistCombo, 1, 1);
	pick->addWidget(playlistMenuBtn, 1, 2);
	pick->setColumnStretch(1, 1);
	root->addLayout(pick);

	/* project menu */
	auto *pm = new QMenu(this);
	pm->addAction("New project…", this, [this]() {
		QString n = askName("New Project", "Project name:", "");
		if (!n.isEmpty())
			ctrl->addProject(n);
	});
	pm->addAction("Rename project…", this, [this]() {
		const Project *p = ctrl->activeProject();
		if (!p)
			return;
		QString n = askName("Rename Project", "Project name:", p->name);
		if (!n.isEmpty())
			ctrl->renameProject(ctrl->activeProjectIndex(), n);
	});
	pm->addAction("Duplicate project", this, [this]() { ctrl->duplicateProject(ctrl->activeProjectIndex()); });
	pm->addAction("Delete project…", this, [this]() {
		const Project *p = ctrl->activeProject();
		if (!p)
			return;
		if (QMessageBox::question(this, "Delete Project",
					  QString("Delete project \"%1\" and all of its playlists?\n\nYour video "
						  "files are not touched.")
						  .arg(p->name)) == QMessageBox::Yes)
			ctrl->deleteProject(ctrl->activeProjectIndex());
	});
	pm->addSeparator();
	pm->addAction("Import project…", this, [this]() {
		QString path = QFileDialog::getOpenFileName(this, "Import Project", QString(),
							    "DDP Playlist Project (*.ddpplaylist *.json)");
		if (!path.isEmpty() && !ctrl->importProject(path))
			QMessageBox::warning(this, "Import Project", "That file isn't a DDP Playlist project.");
	});
	pm->addAction("Export project…", this, [this]() {
		const Project *p = ctrl->activeProject();
		if (!p)
			return;
		QString start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + p->name +
				".ddpplaylist";
		QString path = QFileDialog::getSaveFileName(this, "Export Project", start,
							    "DDP Playlist Project (*.ddpplaylist)");
		if (!path.isEmpty() && !ctrl->exportProject(ctrl->activeProjectIndex(), path))
			QMessageBox::warning(this, "Export Project", "Couldn't write that file.");
	});
	projectMenuBtn->setMenu(pm);

	/* playlist menu */
	auto *plm = new QMenu(this);
	plm->addAction("New playlist…", this, [this]() {
		QString n = askName("New Playlist", "Playlist name:", "");
		if (!n.isEmpty())
			ctrl->addPlaylist(n);
	});
	plm->addAction("Rename playlist…", this, [this]() {
		const Playlist *pl = ctrl->activePlaylist();
		if (!pl)
			return;
		QString n = askName("Rename Playlist", "Playlist name:", pl->name);
		if (!n.isEmpty())
			ctrl->renamePlaylist(ctrl->activePlaylistIndex(), n);
	});
	plm->addAction("Duplicate playlist", this, [this]() { ctrl->duplicatePlaylist(ctrl->activePlaylistIndex()); });
	plm->addAction("Delete playlist…", this, [this]() {
		const Playlist *pl = ctrl->activePlaylist();
		if (!pl)
			return;
		if (QMessageBox::question(this, "Delete Playlist", QString("Delete playlist \"%1\"?").arg(pl->name)) ==
		    QMessageBox::Yes)
			ctrl->deletePlaylist(ctrl->activePlaylistIndex());
	});
	playlistMenuBtn->setMenu(plm);

	/* --- clip list --- */
	list = new ClipList();
	list->setMinimumHeight(140);
	list->setToolTip("Drag videos in from your desktop. Drag rows to reorder.\n"
			 "Double-click to play  •  F2 rename  •  Delete removes");
	root->addWidget(list, 1);

	auto *edit = new QHBoxLayout();
	edit->setSpacing(4);
	addBtn = new QPushButton("+ Add");
	addBtn->setToolTip("Add video or audio files");
	removeBtn = new QPushButton("Remove");
	upBtn = new QPushButton();
	upBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
	upBtn->setToolTip("Move up");
	downBtn = new QPushButton();
	downBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
	downBtn->setToolTip("Move down");
	upBtn->setMaximumWidth(36);
	downBtn->setMaximumWidth(36);
	edit->addWidget(addBtn, 1);
	edit->addWidget(removeBtn, 1);
	edit->addWidget(upBtn);
	edit->addWidget(downBtn);
	root->addLayout(edit);

	/* --- now playing --- */
	nowLabel = new QLabel();
	nowLabel->setTextFormat(Qt::PlainText);
	QFont nf = nowLabel->font();
	nf.setBold(true);
	nowLabel->setFont(nf);
	timeLabel = new QLabel();
	timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	auto *nowRow = new QHBoxLayout();
	nowRow->addWidget(nowLabel, 1);
	nowRow->addWidget(timeLabel);
	root->addLayout(nowRow);

	progress = new QProgressBar();
	progress->setRange(0, 1000);
	progress->setTextVisible(false);
	progress->setMaximumHeight(5);
	progress->setStyleSheet(QString("QProgressBar{border:none;background:rgba(127,127,127,0.25);}"
					"QProgressBar::chunk{background:%1;}")
					.arg(kGold));
	root->addWidget(progress);

	/* --- transport --- */
	auto *transport = new QHBoxLayout();
	transport->setSpacing(4);
	prevBtn = transportButton(QStyle::SP_MediaSkipBackward, "Previous clip");
	playBtn = transportButton(QStyle::SP_MediaPlay, "Play / Pause");
	stopBtn = transportButton(QStyle::SP_MediaStop, "Stop");
	nextBtn = transportButton(QStyle::SP_MediaSkipForward, "Next clip");
	transport->addWidget(prevBtn);
	transport->addWidget(playBtn);
	transport->addWidget(stopBtn);
	transport->addWidget(nextBtn);
	root->addLayout(transport);

	breakBtn = new QPushButton();
	breakBtn->setMinimumHeight(38);
	breakBtn->setFocusPolicy(Qt::NoFocus);
	breakBtn->setToolTip("Cuts program to the commercial-break scene and plays this playlist from the top.\n"
			     "When the last clip ends it returns to the scene you came from.");
	root->addWidget(breakBtn);

	/* --- options --- */
	auto *opts = new QGridLayout();
	opts->setHorizontalSpacing(10);
	opts->setVerticalSpacing(2);
	autoAdvanceChk = new QCheckBox("Auto-advance");
	autoAdvanceChk->setToolTip("Play the next clip automatically when one ends");
	loopChk = new QCheckBox("Loop playlist");
	returnChk = new QCheckBox("Return to show after break");
	monitorChk = new QCheckBox("Hear audio (monitor)");
	monitorChk->setToolTip("Monitor and output the player's audio so you hear it in your headphones");
	opts->addWidget(autoAdvanceChk, 0, 0);
	opts->addWidget(loopChk, 0, 1);
	opts->addWidget(returnChk, 1, 0);
	opts->addWidget(monitorChk, 1, 1);
	root->addLayout(opts);

	auto *addToScene = new QPushButton("Add player to current scene");
	addToScene->setToolTip("Puts the \"DDP Playlist Player\" source into the scene you're looking at,\n"
			       "for when you want commercials over your own layout instead of the break scene.");
	addToScene->setFlat(true);
	root->addWidget(addToScene);

	/* --- wiring --- */
	connect(ctrl, &PlaylistController::libraryChanged, this, &PlaylistDock::rebuild);
	connect(ctrl, &PlaylistController::playbackChanged, this, &PlaylistDock::refreshPlayback);

	connect(projectCombo, qOverload<int>(&QComboBox::activated), this, &PlaylistDock::onProjectChosen);
	connect(playlistCombo, qOverload<int>(&QComboBox::activated), this, &PlaylistDock::onPlaylistChosen);

	connect(addBtn, &QPushButton::clicked, this, &PlaylistDock::addClips);
	connect(removeBtn, &QPushButton::clicked, this, &PlaylistDock::removeSelected);
	connect(upBtn, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
	connect(downBtn, &QPushButton::clicked, this, [this]() { moveSelected(+1); });

	connect(list, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *it) { ctrl->playItem(it->data(Qt::UserRole).toString()); });
	connect(list, &ClipList::filesDropped, this, [this](const QStringList &files, int row) {
		if (ctrl->addFiles(files, row) == 0)
			QMessageBox::information(this, "Add Clips",
						 "None of those files look like video or audio this player can use.");
	});
	connect(list, &ClipList::orderChanged, this, [this]() {
		QStringList ids;
		for (int i = 0; i < list->count(); i++)
			ids << list->item(i)->data(Qt::UserRole).toString();
		ctrl->reorderItems(ids);
	});
	connect(list, &QWidget::customContextMenuRequested, this, &PlaylistDock::showClipMenu);

	auto *del = new QShortcut(QKeySequence::Delete, list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(del, &QShortcut::activated, this, &PlaylistDock::removeSelected);
	auto *f2 = new QShortcut(QKeySequence(Qt::Key_F2), list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(f2, &QShortcut::activated, this, &PlaylistDock::renameSelected);
	auto *ret = new QShortcut(QKeySequence(Qt::Key_Return), list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(ret, &QShortcut::activated, this, [this]() {
		QString id = currentId();
		if (!id.isEmpty())
			ctrl->playItem(id);
	});

	connect(prevBtn, &QPushButton::clicked, ctrl, &PlaylistController::previous);
	connect(playBtn, &QPushButton::clicked, this, &PlaylistDock::playSelectedOrResume);
	connect(stopBtn, &QPushButton::clicked, ctrl, &PlaylistController::stop);
	connect(nextBtn, &QPushButton::clicked, ctrl, &PlaylistController::next);
	connect(breakBtn, &QPushButton::clicked, ctrl, &PlaylistController::toggleBreak);
	connect(addToScene, &QPushButton::clicked, ctrl, &PlaylistController::addPlayerToCurrentScene);

	connect(autoAdvanceChk, &QCheckBox::toggled, this, [this](bool on) {
		if (!rebuilding)
			ctrl->setAutoAdvance(on);
	});
	connect(loopChk, &QCheckBox::toggled, this, [this](bool on) {
		if (!rebuilding)
			ctrl->setLoop(on);
	});
	connect(returnChk, &QCheckBox::toggled, this, [this](bool on) {
		if (!rebuilding)
			ctrl->setReturnAfterBreak(on);
	});
	connect(monitorChk, &QCheckBox::toggled, this, [this](bool on) {
		if (!rebuilding)
			ctrl->setMonitorAudio(on);
	});

	timer = new QTimer(this);
	timer->setInterval(200);
	connect(timer, &QTimer::timeout, this, &PlaylistDock::tick);
	timer->start();

	rebuild();
}

QString PlaylistDock::askName(const QString &title, const QString &label, const QString &current)
{
	bool okPressed = false;
	QString n = QInputDialog::getText(this, title, label, QLineEdit::Normal, current, &okPressed);
	return okPressed ? n.trimmed() : QString();
}

QStringList PlaylistDock::selectedIds() const
{
	QStringList ids;
	for (QListWidgetItem *it : list->selectedItems())
		ids << it->data(Qt::UserRole).toString();
	return ids;
}

QString PlaylistDock::currentId() const
{
	QListWidgetItem *it = list->currentItem();
	return it ? it->data(Qt::UserRole).toString() : QString();
}

QString PlaylistDock::formatTime(int64_t ms)
{
	if (ms < 0)
		ms = 0;
	int64_t s = ms / 1000;
	return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

void PlaylistDock::rebuild()
{
	rebuilding = true;
	const Library &lib = ctrl->library();

	projectCombo->clear();
	for (const auto &p : lib.projects)
		projectCombo->addItem(p.name);
	projectCombo->setCurrentIndex(ctrl->activeProjectIndex());

	playlistCombo->clear();
	if (const Project *p = ctrl->activeProject())
		for (const auto &pl : p->playlists)
			playlistCombo->addItem(QString("%1  (%2)").arg(pl.name).arg(pl.items.size()));
	playlistCombo->setCurrentIndex(ctrl->activePlaylistIndex());

	QStringList keepSelected = selectedIds();
	QString keepCurrent = currentId();
	list->clear();
	if (const Playlist *pl = ctrl->activePlaylist()) {
		int n = 1;
		for (const auto &it : pl->items) {
			auto *row = new QListWidgetItem();
			row->setData(Qt::UserRole, it.id);
			row->setToolTip(it.path);
			bool missing = !QFileInfo::exists(it.path);
			QString text = QString("%1.  %2%3").arg(n++).arg(it.name).arg(missing ? "   (file missing)" : "");
			row->setText(text);
			row->setData(Qt::UserRole + 1, text);
			if (missing) {
				QFont f = row->font();
				f.setItalic(true);
				row->setFont(f);
				row->setForeground(QColor("#e05555"));
			}
			list->addItem(row);
			if (keepSelected.contains(it.id))
				row->setSelected(true);
			if (it.id == keepCurrent)
				list->setCurrentItem(row, QItemSelectionModel::NoUpdate);
		}
		autoAdvanceChk->setChecked(pl->autoAdvance);
		loopChk->setChecked(pl->loop);
	}
	returnChk->setChecked(lib.returnAfterBreak);
	monitorChk->setChecked(lib.monitorAudio);

	rebuilding = false;
	refreshPlayback();
}

void PlaylistDock::refreshPlayback()
{
	using State = PlaylistController::State;
	State st = ctrl->state();

	// highlight the clip on air
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *row = list->item(i);
		bool onAir = row->data(Qt::UserRole).toString() == ctrl->playingItemId() && st != State::Stopped;
		QFont f = row->font();
		f.setBold(onAir);
		row->setFont(f);
		QString base = row->data(Qt::UserRole + 1).toString();
		row->setText(onAir ? QString::fromUtf8("\u25B6  ") + base : base);
		if (onAir)
			row->setForeground(QColor(kGold));
		else if (!f.italic())
			row->setForeground(QBrush());
	}

	QString name = ctrl->playingName();
	if (st == State::Stopped || name.isEmpty()) {
		nowLabel->setText("Stopped");
	} else {
		QString prefix = st == State::Paused ? "Paused: " : "▶ ";
		int idx = ctrl->playingIndex();
		const Playlist *pl = nullptr;
		for (const auto &p : ctrl->library().projects)
			for (const auto &x : p.playlists)
				if (x.id == ctrl->playingPlaylistId())
					pl = &x;
		QString pos = (pl && idx >= 0) ? QString("  [%1/%2]").arg(idx + 1).arg(pl->items.size()) : QString();
		nowLabel->setText(prefix + name + pos);
	}

	playBtn->setIcon(style()->standardIcon(st == State::Playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));

	if (ctrl->onBreak()) {
		breakBtn->setText("END BREAK  ●  back to show");
		breakBtn->setStyleSheet("QPushButton{background:#c0392b;color:white;font-weight:bold;border-radius:4px;}"
					"QPushButton:hover{background:#d84a3b;}");
	} else {
		breakBtn->setText("TAKE BREAK  —  play playlist on program");
		breakBtn->setStyleSheet(QString("QPushButton{background:%1;color:%2;font-weight:bold;border-radius:4px;}"
						"QPushButton:hover{background:#e0b366;}")
						.arg(kGold, kInk));
	}
	tick();
}

void PlaylistDock::tick()
{
	if (ctrl->state() == PlaylistController::State::Stopped) {
		timeLabel->clear();
		progress->setValue(0);
		return;
	}
	int64_t t = ctrl->timeMs();
	int64_t d = ctrl->durationMs();
	if (d > 0) {
		timeLabel->setText(QString("%1 / %2   -%3").arg(formatTime(t), formatTime(d), formatTime(d - t)));
		progress->setValue((int)qBound<int64_t>(int64_t(0), t * 1000 / d, int64_t(1000)));
	} else {
		timeLabel->setText(formatTime(t));
		progress->setValue(0);
	}
}

void PlaylistDock::onProjectChosen(int index)
{
	if (!rebuilding)
		ctrl->selectProject(index);
}

void PlaylistDock::onPlaylistChosen(int index)
{
	if (!rebuilding)
		ctrl->selectPlaylist(index);
}

void PlaylistDock::addClips()
{
	QStringList patterns;
	for (const QString &e : Library::mediaExtensions())
		patterns << "*." + e;
	QString filter = QString("Video & Audio (%1);;All files (*)").arg(patterns.join(' '));
	QStringList files = QFileDialog::getOpenFileNames(this, "Add Clips", QString(), filter);
	if (files.isEmpty())
		return;
	int row = list->currentRow();
	ctrl->addFiles(files, row >= 0 && list->selectedItems().size() == 1 ? row + 1 : -1);
}

void PlaylistDock::removeSelected()
{
	QStringList ids = selectedIds();
	if (!ids.isEmpty())
		ctrl->removeItems(ids);
}

void PlaylistDock::renameSelected()
{
	QString id = currentId();
	const Playlist *pl = ctrl->activePlaylist();
	if (id.isEmpty() || !pl)
		return;
	int i = pl->indexOf(id);
	if (i < 0)
		return;
	QString n = askName("Rename Clip", "Clip name (the file isn't renamed):", pl->items[i].name);
	if (!n.isEmpty())
		ctrl->renameItem(id, n);
}

void PlaylistDock::moveSelected(int delta)
{
	QString id = currentId();
	if (!id.isEmpty())
		ctrl->moveItem(id, delta);
}

void PlaylistDock::playSelectedOrResume()
{
	using State = PlaylistController::State;
	if (ctrl->state() == State::Stopped) {
		QString id = currentId();
		if (!id.isEmpty() && list->currentItem() && list->currentItem()->isSelected()) {
			ctrl->playItem(id);
			return;
		}
	}
	ctrl->playPause();
}

void PlaylistDock::showClipMenu(const QPoint &pos)
{
	QListWidgetItem *row = list->itemAt(pos);
	QMenu menu(this);
	if (row) {
		QString id = row->data(Qt::UserRole).toString();
		QString path = row->toolTip();
		menu.addAction("Play now", this, [this, id]() { ctrl->playItem(id); });
		menu.addAction("Rename…", this, &PlaylistDock::renameSelected);
		menu.addAction("Show file in folder", this, [path]() {
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
		});
		menu.addSeparator();
		menu.addAction("Remove from playlist", this, &PlaylistDock::removeSelected);
		menu.addSeparator();
	}
	menu.addAction("Add clips…", this, &PlaylistDock::addClips);
	menu.exec(list->viewport()->mapToGlobal(pos));
}
