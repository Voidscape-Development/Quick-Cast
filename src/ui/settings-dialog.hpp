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

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace qc {

class SettingsDialog final : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(SessionController *controller, QWidget *parent = nullptr);

private:
	void regenerateLinks();
	void save();

	SessionController *controller_;

	QLineEdit *hostName_;
	QLineEdit *room_;
	QLineEdit *password_;
	QCheckBox *startWithObs_;

	QComboBox *quality_;
	QComboBox *codec_;
	QCheckBox *hardwareDecoding_;
	QCheckBox *fitToCanvas_;

	QCheckBox *returnEnabled_;
	QComboBox *returnVideo_;
	QComboBox *secondVideo_;
	QComboBox *returnTrack_;
	QComboBox *returnResolution_;
	QSpinBox *returnBitrate_;
	QComboBox *secondResolution_;
	QSpinBox *secondBitrate_;

	QLineEdit *turnUrl_;
	QLineEdit *turnUsername_;
	QLineEdit *turnPassword_;
};

} // namespace qc
