/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "workmode.h"

#include "global/settings.h"

#include "muse_framework_config.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;

workmode::Mode workmode::m_mode = workmode::Mode::Undefined;
workmode::Mode workmode::m_desiredMode = workmode::Mode::Undefined;
workmode::Mode workmode::m_sessionMode = workmode::Mode::Undefined;

static const Settings::Key WORKMODE_KEY("audio", "dev/workmode");

void workmode::load()
{
    if (m_sessionMode != Mode::Undefined) {
        m_mode = m_sessionMode;
        m_desiredMode = m_mode;
        return;
    }

    int val = settings()->value(WORKMODE_KEY).toInt();
    if (val > 0 && val <= 3) {
        m_mode = static_cast<Mode>(val);
    } else {
        m_mode = defaultMode();
    }

    if (m_mode == Mode::WorkerMode_NOT_SUPPORTED) {
        LOGW() << "WorkerMode is not supported, using HybridMode instead";
        m_mode = Mode::HybridMode;
    }

    m_desiredMode = m_mode;
}

workmode::Mode workmode::defaultMode()
{
    return static_cast<Mode>(MUSE_MODULE_AUDIO_WORKMODE);
}

workmode::Mode workmode::mode()
{
    return m_mode;
}

void workmode::setMode(Mode m)
{
    //! NOTE Need restart
    settings()->setLocalValue(WORKMODE_KEY, Val(static_cast<int>(m)));
    m_desiredMode = m;
}

void workmode::setSessionMode(Mode m)
{
    m_sessionMode = m;
}

workmode::Mode workmode::desiredMode()
{
    return m_desiredMode;
}
