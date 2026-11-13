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

#include <QElapsedTimer>
#include <QWidget>

#include <map>

class QLabel;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace qc {

class GuestRow;

/**
 * The Quick Cast dock.
 *
 * Top to bottom it follows the host's own sequence: is the show running, who is here, bring someone new in. Exactly
 * one button is emphasised at a time, and it is always the next sensible step.
 */
class QuickCastDock final : public QWidget {
	Q_OBJECT

public:
	explicit QuickCastDock(SessionController *controller, QWidget *parent = nullptr);

protected:
	void changeEvent(QEvent *event) override;

private:
	void rebuildGuests();
	void refreshGuests();
	void refreshState();
	void restyle();
	void inviteGuest();
	void copyOpenLink();
	void openSettings();
	void showNotice(const QString &text);

	SessionController *controller_;

	QWidget *statusCard_;
	QLabel *statePill_;
	QLabel *timerLabel_;
	QLabel *headline_;
	QLabel *subline_;
	QPushButton *showButton_;
	QLabel *noticeLabel_;
	QWidget *emptyState_;
	QLabel *emptyTitle_;
	QLabel *emptyText_;
	QLabel *listHeading_;
	QWidget *guestContainer_;
	QVBoxLayout *guestLayout_;
	QPushButton *inviteButton_;
	QToolButton *openLinkButton_;
	QToolButton *settingsButton_;
	QTimer *refreshTimer_;
	QTimer *noticeTimer_;
	QElapsedTimer liveClock_;
	bool restyling_ = false;
	std::map<std::string, GuestRow *> rows_;
};

} // namespace qc
