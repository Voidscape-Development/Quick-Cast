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

#include <QString>
#include <QStringList>

class QComboBox;

namespace qc {

/** Everything that produces video, sources first and then scenes, excluding the guests themselves. */
QStringList videoSourceNames();
/** Every source that produces audio, excluding the guests themselves (they must never be sent back to anyone). */
QStringList audioSourceNames();

/**
 * Selects the entry whose data is `name`. A name that is not listed (a source from another scene collection, say)
 * is appended first, so a stored choice is never silently replaced by something else.
 */
void selectSourceName(QComboBox *combo, const QString &name);

} // namespace qc
