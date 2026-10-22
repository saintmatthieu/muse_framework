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
#include "commandsapi.h"

#include <memory>

#include <QEventLoop>
#include <QTimer>

#include "log.h"

using namespace muse;
using namespace muse::rcommand;
using namespace muse::rcommand::api;

CommandsApi::CommandsApi(muse::api::IApiEngine* e)
    : muse::api::ApiObject(e)
{
}

QVariantMap CommandsApi::dispatch(const QString& command, const QVariantMap& params, int timeoutMs)
{
    // The command may carry parameters of its own, like command://foo?bar=1
    CommandQuery query(command.toStdString());
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        query.addParam(it.key().toStdString(), Val::fromQVariant(it.value()));
    }

    // Shared with the response handler, which may still be called once this has returned on timeout
    struct State {
        QEventLoop loop;
        bool responded = false;
        Ret ret;
    };
    auto state = std::make_shared<State>();

    dispatcher()->dispatch(query).onResolve(nullptr, [state](const Response& response) {
        state->responded = true;
        state->ret = response.ret;
        state->loop.quit();
    });

    if (!state->responded) {
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &state->loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        state->loop.exec();
    }

    if (!state->responded) {
        state->ret = make_ret(Ret::Code::UnknownError, "no response in " + std::to_string(timeoutMs) + " ms");
        LOGE() << state->ret.text() << " from: " << command;
    }

    return {
        { "success", state->ret.success() },
        { "code", state->ret.code() },
        { "text", QString::fromStdString(state->ret.text()) },
    };
}
