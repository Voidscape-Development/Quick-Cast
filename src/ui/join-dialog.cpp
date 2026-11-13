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

#include "join-dialog.hpp"

#include "obs/scene-integration.hpp"
#include "source-lists.hpp"
#include "ui-helpers.hpp"
#include "ui-style.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace qc {

JoinDialog::JoinDialog(SessionController *controller, QWidget *parent) : QDialog(parent), controller_(controller)
{
	setWindowTitle(tr_("Join.Title"));
	setMinimumWidth(480);
	const Settings &settings = controller_->settings();
	const ui::Colors c = ui::colors(this);

	auto *title = new QLabel(tr_("Join.Heading"), this);
	ui::styleHeading(title, 1.25);
	auto *help = new QLabel(tr_("Join.Help"), this);
	help->setWordWrap(true);
	ui::styleMuted(help, 1.0);

	auto *linkLabel = new QLabel(tr_("Join.Link"), this);
	linkEdit_ = new QLineEdit(this);
	linkEdit_->setPlaceholderText(QStringLiteral("https://vdo.ninja/?room=…"));
	linkEdit_->setClearButtonEnabled(true);
	linkLabel->setBuddy(linkEdit_);
	// The link usually arrives in a chat message and is copied a moment before this dialog opens.
	const QString clipboard = QApplication::clipboard()->text().trimmed();
	if (controller_->isInviteLink(clipboard))
		linkEdit_->setText(clipboard);

	problem_ = new QLabel(this);
	problem_->setWordWrap(true);
	problem_->setStyleSheet(QStringLiteral("color: %1;").arg(ui::css(c.warning)));
	problem_->setVisible(false);

	auto *nameLabel = new QLabel(tr_("Join.Name"), this);
	nameEdit_ = new QLineEdit(this);
	nameEdit_->setMaxLength(40);
	nameEdit_->setPlaceholderText(tr_("Join.Name.Placeholder"));
	nameLabel->setBuddy(nameEdit_);

	// The host sees this next to our video, and it names anything we share alongside the camera. The out-of-the-box
	// name is "Host", which is exactly wrong in someone else's show, so that one has to be replaced.
	auto *ownNameLabel = new QLabel(tr_("Join.OwnName"), this);
	ownNameEdit_ = new QLineEdit(this);
	ownNameEdit_->setMaxLength(40);
	ownNameEdit_->setPlaceholderText(tr_("Join.OwnName.Placeholder"));
	if (!settings.hostName.empty() && settings.hostName != Settings().hostName)
		ownNameEdit_->setText(QString::fromStdString(settings.hostName));
	ownNameLabel->setBuddy(ownNameEdit_);

	// --- What you send ---
	auto *sendHeading = new QLabel(tr_("Join.Send"), this);
	ui::styleHeading(sendHeading, 1.05);

	// Until the user has chosen, suggest their camera and microphone: the program would show the host their own
	// picture, and a whole audio track would play the host's stream back to them underneath yours.
	const QString cameraChoice = QString::fromStdString(settings.collabConfigured ? settings.collabVideoSource
										      : detectCameraSourceName());
	const QString microphoneChoice = QString::fromStdString(
		settings.collabConfigured ? settings.collabAudioSource : detectMicrophoneSourceName());

	camera_ = new QComboBox(this);
	camera_->addItem(tr_("Join.Camera.Program"), QString());
	for (const QString &name : videoSourceNames())
		camera_->addItem(name, name);
	selectSourceName(camera_, cameraChoice);
	cameraHint_ = new QLabel(tr_("Join.Camera.ProgramHint"), this);
	cameraHint_->setWordWrap(true);
	cameraHint_->setStyleSheet(QStringLiteral("color: %1;").arg(ui::css(c.warning)));

	microphone_ = new QComboBox(this);
	microphone_->addItem(tr_("Join.Microphone.Track").arg(settings.returnAudioTrack), QString());
	for (const QString &name : audioSourceNames())
		microphone_->addItem(name, name);
	selectSourceName(microphone_, microphoneChoice);

	second_ = new QComboBox(this);
	second_->addItem(tr_("Join.Second.Nothing"), QString());
	for (const QString &name : videoSourceNames())
		second_->addItem(name, name);
	selectSourceName(second_, settings.collabSecondEnabled ? QString::fromStdString(settings.collabSecondSource)
							       : QString());

	auto *form = new QFormLayout();
	form->setHorizontalSpacing(14);
	form->setVerticalSpacing(8);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->addRow(tr_("Join.Camera"), camera_);
	form->addRow(cameraHint_);
	form->addRow(tr_("Join.Microphone"), microphone_);
	form->addRow(tr_("Join.Second"), second_);

	auto *echoHint = new QLabel(tr_("Join.EchoHint"), this);
	echoHint->setWordWrap(true);
	ui::styleMuted(echoHint);

	joinButton_ = new QPushButton(tr_("Join.Join"), this);
	joinButton_->setDefault(true);
	ui::styleButton(joinButton_, ui::ButtonKind::Primary);
	auto *cancelButton = new QPushButton(tr_("Invite.Cancel"), this);
	ui::styleButton(cancelButton, ui::ButtonKind::Secondary);
	auto *buttons = new QHBoxLayout();
	buttons->addStretch(1);
	buttons->addWidget(cancelButton);
	buttons->addWidget(joinButton_);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 20, 20, 16);
	layout->setSpacing(8);
	layout->addWidget(title);
	layout->addWidget(help);
	layout->addSpacing(8);
	layout->addWidget(linkLabel);
	layout->addWidget(linkEdit_);
	layout->addWidget(problem_);
	layout->addWidget(nameLabel);
	layout->addWidget(nameEdit_);
	layout->addWidget(ownNameLabel);
	layout->addWidget(ownNameEdit_);
	layout->addSpacing(10);
	layout->addWidget(sendHeading);
	layout->addLayout(form);
	layout->addWidget(echoHint);
	layout->addStretch(1);
	layout->addSpacing(12);
	layout->addLayout(buttons);

	connect(linkEdit_, &QLineEdit::textChanged, this, &JoinDialog::validate);
	connect(ownNameEdit_, &QLineEdit::textChanged, this, &JoinDialog::validate);
	connect(camera_, &QComboBox::currentIndexChanged, this, &JoinDialog::validate);
	connect(joinButton_, &QPushButton::clicked, this, &JoinDialog::join);
	connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

	validate();
	if (linkEdit_->text().isEmpty())
		linkEdit_->setFocus();
	else if (ownNameEdit_->text().isEmpty())
		ownNameEdit_->setFocus();
	else
		joinButton_->setFocus();
}

void JoinDialog::validate()
{
	const QString link = linkEdit_->text().trimmed();
	const bool valid = controller_->isInviteLink(link);
	joinButton_->setEnabled(valid && !ownNameEdit_->text().trimmed().isEmpty());
	// Say what is wrong only once there is something to be wrong about.
	if (link.isEmpty() || valid)
		problem_->setVisible(false);
	else
		showProblem(tr_("Join.NotAnInvite"));
	cameraHint_->setVisible(camera_->currentData().toString().isEmpty());
}

void JoinDialog::showProblem(const QString &text)
{
	problem_->setText(text);
	problem_->setVisible(true);
}

void JoinDialog::join()
{
	// What we send is a standing choice, not part of this one show, so it is saved with the other settings and
	// used again when a remembered show is joined with one click.
	Settings settings = controller_->settings();
	settings.hostName = ownNameEdit_->text().trimmed().toStdString();
	settings.collabConfigured = true;
	settings.collabVideoSource = camera_->currentData().toString().toStdString();
	settings.collabAudioSource = microphone_->currentData().toString().toStdString();
	settings.collabSecondSource = second_->currentData().toString().toStdString();
	settings.collabSecondEnabled = !settings.collabSecondSource.empty();
	controller_->applySettings(settings);

	switch (controller_->joinShow(linkEdit_->text(), nameEdit_->text())) {
	case JoinResult::Joining:
		accept();
		break;
	case JoinResult::OwnShow:
		showProblem(tr_("Join.OwnShow"));
		break;
	case JoinResult::NotAnInvite:
		showProblem(tr_("Join.NotAnInvite"));
		break;
	}
}

} // namespace qc
