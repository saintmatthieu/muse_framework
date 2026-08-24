/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
#include "process.h"

#include "../processspawnguard.h"

#include "muse_framework_config.h"

#ifdef QT_QPROCESS_SUPPORTED
#include <algorithm>
#include <QElapsedTimer>
#include <QProcess>
#endif

#include "log.h"

using namespace muse;

std::mutex& muse::processSpawnMutex()
{
    static std::mutex m;
    return m;
}

#ifdef QT_QPROCESS_SUPPORTED
static QStringList toQList(const std::vector<std::string>& args)
{
    QStringList list;
    for (const std::string& a : args) {
        list << QString::fromStdString(a);
    }
    return list;
}

#endif

int Process::execute(const std::string& program, const std::vector<std::string>& args)
{
#ifdef QT_QPROCESS_SUPPORTED
    int ret = QProcess::execute(QString::fromStdString(program), toQList(args));
    return ret;
#else
    UNUSED(program);
    UNUSED(args);
    NOT_SUPPORTED;
    return -1;
#endif
}

int Process::execute(const std::string& program, const std::vector<std::string>& args, int timeout_ms)
{
    return execute(program, args, timeout_ms, {});
}

int Process::execute(const std::string& program, const std::vector<std::string>& args, int timeout_ms,
                     const std::function<bool()>& shouldCancel)
{
#ifdef QT_QPROCESS_SUPPORTED
    QProcess process;
    QElapsedTimer elapsed;
    elapsed.start();
    {
        // the fork happens inside start(); see processSpawnMutex()
        std::lock_guard lock(processSpawnMutex());
        process.start(QString::fromStdString(program), toQList(args));
    }

    if (!process.waitForStarted()) {
        return ExecuteStartFailedCode;
    }

    auto killProcess = [&](const char* reason, int returnCode) {
        UNUSED(reason);
        if (returnCode == ExecuteTimeoutCode) {
            LOGW() << "Process timed out, killing: " << program;
        }

        process.kill();
        if (!process.waitForFinished(3000)) {
            if (returnCode == ExecuteTimeoutCode) {
                LOGW() << "Timed-out process did not finish after kill: " << program;
            }
        }
        return returnCode;
    };

    while (true) {
        if (shouldCancel && shouldCancel()) {
            return killProcess("canceled", ExecuteCanceledCode);
        }

        const qint64 remaining = timeout_ms < 0 ? 50 : std::max<qint64>(0, timeout_ms - elapsed.elapsed());
        const int waitMs = timeout_ms < 0 ? 50 : static_cast<int>(std::min<qint64>(50, remaining));

        if (process.waitForFinished(waitMs)) {
            break;
        }

        if (timeout_ms >= 0 && elapsed.elapsed() >= timeout_ms) {
            return killProcess("timed out", ExecuteTimeoutCode);
        }
    }

    {
        const bool normalExit = process.exitStatus() == QProcess::NormalExit;
        const int code = normalExit ? process.exitCode() : ExecuteCrashCode;
        return code;
    }
#else
    UNUSED(program);
    UNUSED(args);
    UNUSED(timeout_ms);
    UNUSED(shouldCancel);
    NOT_SUPPORTED;
    return ExecuteCrashCode;
#endif
}

bool Process::startDetached(const std::string& program, const std::vector<std::string>& args)
{
#ifdef QT_QPROCESS_SUPPORTED
    std::lock_guard lock(processSpawnMutex());
    bool ok = QProcess::startDetached(QString::fromStdString(program), toQList(args));
    return ok;
#else
    UNUSED(program);
    UNUSED(args);
    NOT_SUPPORTED;
    return false;
#endif
}
