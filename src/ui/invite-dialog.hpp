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

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace qc {

/**
 * Two-step invite: name the guest, then see (and copy) their link.
 *
 * The link is shown rather than only placed on the clipboard so the host can see what they are about to send and
 * copy it again, and the dialog says the one thing a first-time host needs to know: what the guest has to do.
 */
class InviteDialog final : public QDialog {
	Q_OBJECT

public:
	explicit InviteDialog(SessionController *controller, QWidget *parent = nullptr);

private:
	void createLink();
	void copyLink();
	void refreshShowHint();

	SessionController *controller_;
	QStackedWidget *pages_;
	QLineEdit *nameEdit_;
	QPushButton *createButton_;
	QLabel *linkTitle_;
	QLineEdit *linkEdit_;
	QPushButton *copyButton_;
	QLabel *instructions_;
	QLabel *showHint_;
	QPushButton *startShowButton_;
	QString guestName_;
};

} // namespace qc
