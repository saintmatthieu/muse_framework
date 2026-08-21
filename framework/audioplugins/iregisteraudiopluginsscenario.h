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
    virtual Ret unregisterRemovedPlugins(const PluginResourceIdList& pluginIds) = 0;

    virtual Ret registerPlugin(const io::path_t& pluginPath) = 0;
    virtual Ret validatePlugin(const io::path_t& pluginPath, const io::path_t& outputFile) = 0;
};
}
