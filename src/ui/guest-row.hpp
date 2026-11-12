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

#include <QFrame>

class QAction;
class QLabel;
class QPushButton;
class QToolButton;

namespace qc {

/** One guest in the dock: who they are, how their connection is doing, and the things you can do with them. */
class GuestRow final : public QFrame {
	Q_OBJECT

public:
	GuestRow(SessionController *controller, const GuestView &guest, QWidget *parent = nullptr);

	const std::string &guestId() const { return guestId_; }
	/** Re-reads everything that can change without the guest list changing. */
	void refresh(const GuestView &guest);

signals:
	/** A short confirmation the dock should show (e.g. "link copied"). */
	void notice(const QString &text);

private:
	void copyLink();
	void toggleScene();
	void rename();
	void remove();

	SessionController *controller_;
	std::string guestId_;
	QString name_;
	bool inScene_ = false;

	QLabel *dot_;
	QLabel *nameLabel_;
	QLabel *detailLabel_;
	QPushButton *sceneButton_;
	QPushButton *linkButton_;
	QToolButton *menuButton_;
	QAction *muteAction_;
	QAction *refreshAction_;
	QAction *disconnectAction_;
};

} // namespace qc
