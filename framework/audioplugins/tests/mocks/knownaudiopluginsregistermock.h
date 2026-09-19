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

#include <gmock/gmock.h>

#include "audioplugins/iknownaudiopluginsregister.h"

namespace muse::audioplugins {
class KnownAudioPluginsRegisterMock : public IKnownAudioPluginsRegister
{
public:
    MOCK_METHOD(Ret, load, (), (override));
    MOCK_METHOD(Ret, clear, (), (override));

    MOCK_METHOD(AudioPluginInfoList, pluginInfoList, (PluginInfoAccepted), (const, override));
    MOCK_METHOD(async::Notification, pluginInfoListChanged, (), (const, override));

    MOCK_METHOD(const io::path_t&, pluginPath, (const PluginResourceId&), (const, override));

    MOCK_METHOD(bool, exists, (const io::path_t&), (const, override));
    MOCK_METHOD(bool, exists, (const PluginResourceId&), (const, override));

    MOCK_METHOD(Ret, registerPlugins, (const AudioPluginInfoList&), (override));
    MOCK_METHOD(Ret, unregisterPlugins, (const PluginResourceIdList&), (override));

    MOCK_METHOD(Ret, setPluginsState, (const io::paths_t&, AudioPluginState), (override));

    MOCK_METHOD(Ret, removePluginsAtPath, (const io::path_t&), (override));
    MOCK_METHOD(Ret, removePluginsAtPaths, (const io::paths_t&), (override));

    MOCK_METHOD(Ret, writePluginsTo, (const io::path_t&, const AudioPluginInfoList&), (const, override));
    MOCK_METHOD(RetVal<AudioPluginInfoList>, readPluginsFrom, (const io::path_t&), (const, override));
};
}
