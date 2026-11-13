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

class QPushButton;
class QToolButton;

namespace qc {

/**
 * A show the user has joined before, as a card in the dock: one click joins it again with the camera and
 * microphone chosen last time.
 */
class CollaboratorRow final : public QFrame {
	Q_OBJECT

public:
	CollaboratorRow(SessionController *controller, const Collaborator &collaborator, QWidget *parent = nullptr);

	/** Re-applies colours after the OBS theme changed. */
	void restyle();

private:
	void rename();
	void forget();

	SessionController *controller_;
	std::string collaboratorId_;
	QString name_;

	ui::ElidedLabel *nameLabel_;
	ui::ElidedLabel *detailLabel_;
	QPushButton *joinButton_;
	QToolButton *menuButton_;
};

} // namespace qc
