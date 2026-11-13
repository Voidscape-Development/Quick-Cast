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

#include "source-lists.hpp"

#include "obs/scene-integration.hpp"

#include <obs.h>

#include <QComboBox>

#include <cstring>

namespace qc {

QStringList videoSourceNames()
{
	QStringList scenes;
	QStringList sources;
	struct Lists {
		QStringList *scenes;
		QStringList *sources;
	} lists{&scenes, &sources};

	const auto collect = [](void *param, obs_source_t *source) {
		auto *out = static_cast<Lists *>(param);
		const char *name = obs_source_get_name(source);
		if (!name || !*name || (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0)
			return true;
		const char *id = obs_source_get_unversioned_id(source);
		if (id && strcmp(id, kGuestSourceId) == 0)
			return true;
		if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
			out->scenes->append(QString::fromUtf8(name));
		else
			out->sources->append(QString::fromUtf8(name));
		return true;
	};
	obs_enum_scenes(collect, &lists);
	obs_enum_sources(collect, &lists);
	scenes.sort(Qt::CaseInsensitive);
	sources.sort(Qt::CaseInsensitive);
	return sources + scenes;
}

QStringList audioSourceNames()
{
	QStringList names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			const char *id = obs_source_get_unversioned_id(source);
			if (!name || !*name || (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;
			if (id && strcmp(id, kGuestSourceId) == 0)
				return true;
			static_cast<QStringList *>(param)->append(QString::fromUtf8(name));
			return true;
		},
		&names);
	names.sort(Qt::CaseInsensitive);
	return names;
}

void selectSourceName(QComboBox *combo, const QString &name)
{
	int index = combo->findData(name);
	if (index < 0) {
		combo->addItem(name, name);
		index = combo->count() - 1;
	}
	combo->setCurrentIndex(index);
}

} // namespace qc
