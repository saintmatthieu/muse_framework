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

#pragma once

#include "modularity/imoduleinterface.h"

#include "global/types/ret.h"
#include "global/io/path.h"
#include "global/progress.h"
#include "global/async/channel.h"
#include "global/async/notification.h"
#include "audiopluginstypes.h"

namespace muse::audioplugins {
struct PluginScanResult {
    io::paths_t newPluginPaths;      // not in cache, or back after Missing; validated via subprocess
    io::paths_t missingPluginPaths;  // in cache but not found by any scanner
};

class IRegisterAudioPluginsScenario : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IRegisterAudioPluginsScenario)

public:
    virtual ~IRegisterAudioPluginsScenario() = default;

    virtual PluginScanResult scanPlugins(Progress* progress = nullptr) const = 0;

    virtual Ret updatePluginsRegistry() = 0;
    virtual Ret rescanAllPlugins() = 0;

    // validate=false only persists Discovered placeholders, to be validated on the next scan
    virtual Ret registerNewPlugins(const io::paths_t& pluginPaths, bool validate = true) = 0;

    // Returns as soon as Discovered placeholders are persisted; validation
    // continues in the background and results are flushed to the registry
    // (with pluginInfoListChanged notifications) as they arrive. If a
    // background validation is already running, the paths join its queue.
    virtual Ret registerNewPluginsAsync(const io::paths_t& pluginPaths) = 0;

    // Validate-on-first-use: a plugin path is "validated in this session" once a
    // validation subprocess succeeded on it since the app started (at startup
    // for new plugins, or on demand). Loading a third-party plugin in-process
    // for the first time in a session should first go through this.
    virtual bool isValidatedInSession(const io::path_t& pluginPath) const = 0;

    // Queues a known plugin path at the front of the background TODO list.
    // No-op if it is already queued, in flight, or validated in this session.
    // On success the registry entries are kept; on failure they are marked Error.
    virtual void validatePluginAsync(const io::path_t& pluginPath) = 0;

    // Sent on the main thread when a path's background validation has finished,
    // whatever the outcome (check isValidatedInSession for the result).
    virtual async::Channel<io::path_t> pluginValidationFinished() const = 0;

    // True while a background validation scan is in progress (its TODO queue is
    // not yet drained). Main thread only.
    virtual bool isValidating() const = 0;

    // Sent on the main thread when a background validation scan has fully
    // finished (queue drained). Useful to reflect scan progress in the UI.
    virtual async::Notification pluginValidationScanFinished() const = 0;
    virtual Ret unregisterRemovedPlugins(const PluginResourceIdList& pluginIds) = 0;

    virtual Ret registerPlugin(const io::path_t& pluginPath) = 0;
    virtual Ret validatePlugin(const io::path_t& pluginPath, const io::path_t& outputFile) = 0;
};
}
