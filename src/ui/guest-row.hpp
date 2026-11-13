/*
Quick Cast - remote guests for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include "obs/session-controller.hpp"
#include "ui-style.hpp"

#include <QFrame>

class QAction;
class QLabel;
class QPushButton;
class QToolButton;

namespace qc {

/**
 * One guest in the dock, as a card: who they are, how their connection is doing, and the two things a host does most
 * (put them on screen, send them their link) one click away. Everything else lives in the overflow menu.
 */
class GuestRow final : public QFrame {
	Q_OBJECT

public:
	GuestRow(SessionController *controller, const GuestView &guest, QWidget *parent = nullptr);

	const std::string &guestId() const { return guestId_; }
	/** Re-reads everything that can change without the guest list changing. */
	void refresh(const GuestView &guest);
	/** Re-applies colours after the OBS theme changed. */
	void restyle();

signals:
	/** A short confirmation the dock should show (e.g. "link copied"). */
	void notice(const QString &text);

private:
	void copyLink();
	void toggleScene();
	void toggleMute();
	void rename();
	void remove();
	void applySceneState();

	SessionController *controller_;
	std::string guestId_;
	QString name_;
	bool inScene_ = false;
	bool muted_ = false;
	bool sceneStateKnown_ = false;
	bool connected_ = false;
	bool styledConnected_ = false;
	/** Whether this guest sits in our own show. Someone met in a show we joined has no link of ours to copy and
	 *  is not ours to disconnect. */
	bool ownGuest_ = true;

	ui::Avatar *avatar_;
	ui::ElidedLabel *nameLabel_;
	ui::SignalBars *signal_;
	ui::ElidedLabel *detailLabel_;
	QLabel *flagsLabel_;
	QPushButton *sceneButton_;
	QToolButton *linkButton_;
	QToolButton *muteButton_;
	QToolButton *menuButton_;
	QAction *refreshAction_;
	QAction *disconnectAction_;
};

} // namespace qc
