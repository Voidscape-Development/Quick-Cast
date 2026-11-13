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

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace qc {

/**
 * Joining another streamer's show: paste their invite link, check what you will send them, join.
 *
 * The same link a browser guest would open works here. The difference is what comes back: instead of a web page,
 * the host and their other guests turn up in the dock as sources, so both streamers have each other in OBS.
 */
class JoinDialog final : public QDialog {
	Q_OBJECT

public:
	explicit JoinDialog(SessionController *controller, QWidget *parent = nullptr);

private:
	void validate();
	void join();
	void showProblem(const QString &text);

	SessionController *controller_;
	QLineEdit *linkEdit_;
	QLineEdit *nameEdit_;
	QLabel *problem_;
	QComboBox *camera_;
	QLabel *cameraHint_;
	QComboBox *microphone_;
	QComboBox *second_;
	QPushButton *joinButton_;
};

} // namespace qc
