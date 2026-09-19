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

#include "global/async/notification.h"
#include "global/types/ret.h"
#include "global/types/retval.h"
#include "global/io/path.h"

#include "audiopluginstypes.h"

namespace muse::audioplugins {
class IKnownAudioPluginsRegister : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IKnownAudioPluginsRegister)

public:
    virtual ~IKnownAudioPluginsRegister() = default;

    virtual Ret load() = 0;
    virtual Ret clear() = 0;

    using PluginInfoAccepted = std::function<bool (const AudioPluginInfo& info)>;

    virtual AudioPluginInfoList pluginInfoList(PluginInfoAccepted accepted = PluginInfoAccepted()) const = 0;
    virtual muse::async::Notification pluginInfoListChanged() const = 0;

    virtual const io::path_t& pluginPath(const PluginResourceId& resourceId) const = 0;

    virtual bool exists(const io::path_t& pluginPath) const = 0;
    virtual bool exists(const PluginResourceId& resourceId) const = 0;

    virtual Ret registerPlugins(const AudioPluginInfoList& list) = 0;
    virtual Ret unregisterPlugins(const PluginResourceIdList& resourceIds) = 0;

    virtual Ret setPluginsState(const io::paths_t& paths, AudioPluginState state) = 0;

    virtual Ret removePluginsAtPath(const io::path_t& path) = 0;
    virtual Ret removePluginsAtPaths(const io::paths_t& paths) = 0;

    virtual Ret writePluginsTo(const io::path_t& file, const AudioPluginInfoList& list) const = 0;
    virtual RetVal<AudioPluginInfoList> readPluginsFrom(const io::path_t& file) const = 0;
};
}
