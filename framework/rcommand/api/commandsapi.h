/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
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
#pragma once

#include <QString>
#include <QVariantMap>

#include "api/apiobject.h"
#include "modularity/ioc.h"
#include "rcommand/icommanddispatcher.h"

namespace muse::rcommand::api {
//! NOTE Dispatches commands from scripts, and unlike the actions dispatcher, waits for their response:
//! the dispatcher only queues a command, which then runs once the queue is processed
class CommandsApi : public muse::api::ApiObject
{
    Q_OBJECT

    ContextInject<ICommandDispatcher> dispatcher = { this };

public:
    explicit CommandsApi(muse::api::IApiEngine* e);

    //! Returns { success, code, text } once the command has responded, or failed with a timeout if it did not
    //! respond in time, e.g. because it waits for a dialog to close
    Q_INVOKABLE QVariantMap dispatch(const QString& command, const QVariantMap& params = QVariantMap(), int timeoutMs = 30000);
};
}
