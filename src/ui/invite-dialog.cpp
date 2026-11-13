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

#include "invite-dialog.hpp"

#include "ui-helpers.hpp"
#include "ui-style.hpp"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace qc {

InviteDialog::InviteDialog(SessionController *controller, QWidget *parent) : QDialog(parent), controller_(controller)
{
	setWindowTitle(tr_("Invite.Title"));
	setMinimumWidth(440);

	// --- Step 1: who ---
	auto *namePage = new QWidget(this);
	auto *nameTitle = new QLabel(tr_("Invite.Heading"), namePage);
	ui::styleHeading(nameTitle, 1.25);
	auto *nameHelp = new QLabel(tr_("Invite.Help"), namePage);
	nameHelp->setWordWrap(true);
	ui::styleMuted(nameHelp, 1.0);
	auto *nameLabel = new QLabel(tr_("Invite.Label"), namePage);
	nameEdit_ = new QLineEdit(namePage);
	nameEdit_->setMaxLength(40);
	nameEdit_->setPlaceholderText(tr_("Invite.Placeholder"));
	nameLabel->setBuddy(nameEdit_);
	createButton_ = new QPushButton(tr_("Invite.Create"), namePage);
	createButton_->setDefault(true);
	createButton_->setEnabled(false);
	ui::styleButton(createButton_, ui::ButtonKind::Primary);
	auto *cancelButton = new QPushButton(tr_("Invite.Cancel"), namePage);
	ui::styleButton(cancelButton, ui::ButtonKind::Secondary);

	auto *nameButtons = new QHBoxLayout();
	nameButtons->addStretch(1);
	nameButtons->addWidget(cancelButton);
	nameButtons->addWidget(createButton_);

	auto *nameLayout = new QVBoxLayout(namePage);
	nameLayout->setSpacing(8);
	nameLayout->addWidget(nameTitle);
	nameLayout->addWidget(nameHelp);
	nameLayout->addSpacing(8);
	nameLayout->addWidget(nameLabel);
	nameLayout->addWidget(nameEdit_);
	nameLayout->addStretch(1);
	nameLayout->addSpacing(12);
	nameLayout->addLayout(nameButtons);

	// --- Step 2: the link ---
	auto *linkPage = new QWidget(this);
	linkTitle_ = new QLabel(linkPage);
	ui::styleHeading(linkTitle_, 1.25);
	linkEdit_ = new QLineEdit(linkPage);
	linkEdit_->setReadOnly(true);
	linkEdit_->setAccessibleName(tr_("Invite.LinkField"));
	copyButton_ = new QPushButton(linkPage);
	ui::styleButton(copyButton_, ui::ButtonKind::Primary);
	instructions_ = new QLabel(linkPage);
	instructions_->setWordWrap(true);
	ui::styleMuted(instructions_, 1.0);
	showHint_ = new QLabel(tr_("Invite.ShowOff"), linkPage);
	showHint_->setWordWrap(true);
	showHint_->setStyleSheet(QStringLiteral("color: %1;").arg(ui::css(ui::colors(this).warning)));
	startShowButton_ = new QPushButton(tr_("Dock.StartShow"), linkPage);
	ui::styleButton(startShowButton_, ui::ButtonKind::Secondary);
	auto *doneButton = new QPushButton(tr_("Invite.Done"), linkPage);
	ui::styleButton(doneButton, ui::ButtonKind::Secondary);

	auto *linkRow = new QHBoxLayout();
	linkRow->setSpacing(6);
	linkRow->addWidget(linkEdit_, 1);
	linkRow->addWidget(copyButton_);

	auto *linkButtons = new QHBoxLayout();
	linkButtons->addWidget(startShowButton_);
	linkButtons->addStretch(1);
	linkButtons->addWidget(doneButton);

	auto *linkLayout = new QVBoxLayout(linkPage);
	linkLayout->setSpacing(8);
	linkLayout->addWidget(linkTitle_);
	linkLayout->addLayout(linkRow);
	linkLayout->addWidget(instructions_);
	linkLayout->addWidget(showHint_);
	linkLayout->addStretch(1);
	linkLayout->addSpacing(12);
	linkLayout->addLayout(linkButtons);

	pages_ = new QStackedWidget(this);
	pages_->addWidget(namePage);
	pages_->addWidget(linkPage);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 20, 20, 16);
	layout->addWidget(pages_);

	connect(nameEdit_, &QLineEdit::textChanged, this,
		[this](const QString &text) { createButton_->setEnabled(!text.trimmed().isEmpty()); });
	connect(nameEdit_, &QLineEdit::returnPressed, this, [this] {
		if (createButton_->isEnabled())
			createLink();
	});
	connect(createButton_, &QPushButton::clicked, this, &InviteDialog::createLink);
	connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
	connect(copyButton_, &QPushButton::clicked, this, &InviteDialog::copyLink);
	connect(doneButton, &QPushButton::clicked, this, &QDialog::accept);
	connect(startShowButton_, &QPushButton::clicked, this, [this] {
		controller_->startShow();
		refreshShowHint();
	});

	nameEdit_->setFocus();
}

void InviteDialog::createLink()
{
	const GuestSeat seat = controller_->addGuest(nameEdit_->text());
	guestName_ = QString::fromStdString(seat.name);

	linkTitle_->setText(tr_("Invite.LinkHeading").arg(guestName_));
	linkEdit_->setText(controller_->inviteLink(seat.id));
	linkEdit_->setCursorPosition(0);
	instructions_->setText(tr_("Invite.Instructions").arg(guestName_));
	refreshShowHint();
	pages_->setCurrentIndex(1);
	// Most hosts paste straight away, so the link is already on the clipboard when the page appears.
	copyLink();
	copyButton_->setFocus();
}

void InviteDialog::copyLink()
{
	QApplication::clipboard()->setText(linkEdit_->text());
	copyButton_->setText(tr_("Invite.Copied"));
	copyButton_->setIcon(ui::icon(ui::Icon::Check, ui::colors(this).accentText));
	QTimer::singleShot(2500, this, [this] {
		copyButton_->setText(tr_("Invite.Copy"));
		copyButton_->setIcon(ui::icon(ui::Icon::Copy, ui::colors(this).accentText));
	});
}

void InviteDialog::refreshShowHint()
{
	const bool running = controller_->showRunning();
	showHint_->setVisible(!running);
	startShowButton_->setVisible(!running);
}

} // namespace qc
