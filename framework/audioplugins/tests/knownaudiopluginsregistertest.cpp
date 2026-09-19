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
#include <gmock/gmock.h>

#include "global/serialization/json.h"

#include "audioplugins/internal/knownaudiopluginsregister.h"

#include "global/tests/mocks/filesystemmock.h"
#include "global/tests/mocks/globalconfigurationmock.h"
#include "mocks/audiopluginsconfigurationmock.h"
#include "mocks/knownaudiopluginsmigrationregistermock.h"

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

using namespace muse;
using namespace muse::audioplugins;
using namespace muse::io;

namespace {
const String kRuntimeAttrKey(u"playbackSetupData");
const String kRuntimeAttrValue(u"general");
}

namespace muse::audioplugins {
class AudioPlugins_KnownAudioPluginsRegisterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_knownPlugins = std::make_shared<KnownAudioPluginsRegister>();
        m_fileSystem = std::make_shared<NiceMock<FileSystemMock> >();
        m_configuration = std::make_shared<NiceMock<AudioPluginsConfigurationMock> >();
        m_migrations = std::make_shared<NiceMock<KnownAudioPluginsMigrationRegisterMock> >();
        m_globalConfiguration = std::make_shared<NiceMock<GlobalConfigurationMock> >();

        m_knownPlugins->fileSystem.set(m_fileSystem);
        m_knownPlugins->configuration.set(m_configuration);
        m_knownPlugins->migrations.set(m_migrations);
        m_knownPlugins->globalConfiguration.set(m_globalConfiguration);

        m_knownAudioPluginsFilePath = "/test/some dir/known_audio_plugins.json";
        ON_CALL(*m_configuration, knownAudioPluginsFilePath())
        .WillByDefault(Return(m_knownAudioPluginsFilePath));

        m_runtimeDefaults = PluginAttributes { { kRuntimeAttrKey, kRuntimeAttrValue } };
        ON_CALL(*m_configuration, runtimeAttributeDefaults())
        .WillByDefault(ReturnRef(m_runtimeDefaults));

        ON_CALL(*m_migrations, migrate(_, _, _))
        .WillByDefault(Return(muse::make_ok()));
    }

    ByteArray pluginInfoListToJson(const std::vector<AudioPluginInfo>& infoList) const
    {
        const std::map<PluginType, std::string> RESOURCE_TYPE_TO_STR {
            { "VstPlugin", "VstPlugin" },
        };

        JsonArray array;

        for (const AudioPluginInfo& info : infoList) {
            JsonObject attributesObj;
            for (auto it = info.meta.attributes.cbegin(); it != info.meta.attributes.cend(); ++it) {
                if (it->first == kRuntimeAttrKey) {
                    continue;
                }

                attributesObj.set(it->first.toStdString(), it->second.toStdString());
            }

            JsonObject metaObj;
            metaObj.set("id", info.meta.id);
            metaObj.set("type", muse::value(RESOURCE_TYPE_TO_STR, info.meta.type, "Undefined"));

            if (!info.meta.vendor.empty()) {
                metaObj.set("vendor", info.meta.vendor);
            }

            if (!attributesObj.empty()) {
                metaObj.set("attributes", attributesObj);
            }

            JsonObject mainObj;
            mainObj.set("meta", metaObj);
            mainObj.set("path", info.path.toStdString());
            mainObj.set("state", audioPluginStateName(info.state));

            if (info.errorCode != 0) {
                mainObj.set("errorCode", info.errorCode);
            }

            array << mainObj;
        }

        JsonObject root;
        root.set("version", CURRENT_KNOWN_AUDIO_PLUGINS_VERSION);
        root.set("plugins", array);

        return JsonDocument(root).toJson();
    }

    std::vector<AudioPluginInfo> setupTestData()
    {
        std::vector<AudioPluginInfo> plugins;

        AudioPluginInfo pluginInfo1;
        pluginInfo1.path = "/some/path/to/vst/plugin/AAA.vst3";
        pluginInfo1.meta.id = "AAA";
        pluginInfo1.meta.type = "VstPlugin";
        pluginInfo1.meta.vendor = "Some vendor";
        pluginInfo1.meta.attributes = { { String(u"categories"), u"Fx|Reverb" },
            { String(u"hasNativeEditorSupport"), u"true" },
            { kRuntimeAttrKey, kRuntimeAttrValue } };
        pluginInfo1.state = AudioPluginState::Validated;
        plugins.push_back(pluginInfo1);

        AudioPluginInfo pluginInfo2;
        pluginInfo2.path = "/some/path/to/vst/plugin/BBB.vst3";
        pluginInfo2.meta.id = "BBB";
        pluginInfo2.meta.type = "VstPlugin";
        pluginInfo2.meta.vendor = "Another vendor";
        pluginInfo2.meta.attributes = { { String(u"categories"), u"Fx|Distortion" },
            { kRuntimeAttrKey, kRuntimeAttrValue } };
        pluginInfo2.state = AudioPluginState::Validated;
        plugins.push_back(pluginInfo2);

        AudioPluginInfo erroredPluginInfo;
        erroredPluginInfo.path = "/some/path/to/vst/plugin/CCC.vst3";
        erroredPluginInfo.meta.id = "CCC";
        erroredPluginInfo.meta.type = "VstPlugin";
        erroredPluginInfo.state = AudioPluginState::Error;
        erroredPluginInfo.meta.attributes = {
            { String(u"categories"), u"Instrument|Synth" },
            { kRuntimeAttrKey, kRuntimeAttrValue }
        };
        erroredPluginInfo.errorCode = -1;
        plugins.push_back(erroredPluginInfo);

        ByteArray data = pluginInfoListToJson(plugins);
        ON_CALL(*m_fileSystem, readFile(m_knownAudioPluginsFilePath))
        .WillByDefault(Return(RetVal<ByteArray>::make_ok(data)));

        return plugins;
    }

    std::shared_ptr<KnownAudioPluginsRegister> m_knownPlugins;
    std::shared_ptr<FileSystemMock> m_fileSystem;
    std::shared_ptr<AudioPluginsConfigurationMock> m_configuration;
    std::shared_ptr<KnownAudioPluginsMigrationRegisterMock> m_migrations;
    std::shared_ptr<GlobalConfigurationMock> m_globalConfiguration;

    path_t m_knownAudioPluginsFilePath;
    PluginAttributes m_runtimeDefaults;
};

inline bool operator==(const AudioPluginInfo& info1, const AudioPluginInfo& info2)
{
    return info1.path == info2.path
           && info1.meta == info2.meta
           && info1.state == info2.state
           && info1.errorCode == info2.errorCode;
}
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, PluginInfoList)
{
    // [GIVEN] All known plugins
    std::vector<AudioPluginInfo> expectedPluginInfoList = setupTestData();

    // [GIVEN] File exists
    ON_CALL(*m_fileSystem, exists(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(muse::make_ok()));

    // [WHEN] Load the info
    Ret ret = m_knownPlugins->load();

    // [THEN] Successfully loaded the info
    EXPECT_TRUE(ret);

    // [WHEN] Request the info
    std::vector<AudioPluginInfo> actualPluginInfoList = m_knownPlugins->pluginInfoList();

    // [THEN] Successfully received the info
    EXPECT_EQ(actualPluginInfoList.size(), expectedPluginInfoList.size());
    for (const AudioPluginInfo& actualInfo : actualPluginInfoList) {
        EXPECT_TRUE(muse::contains(expectedPluginInfoList, actualInfo));
        EXPECT_TRUE(m_knownPlugins->exists(actualInfo.path));
        EXPECT_TRUE(m_knownPlugins->exists(actualInfo.meta.id));
        EXPECT_EQ(actualInfo.path, m_knownPlugins->pluginPath(actualInfo.meta.id));
    }

    // [THEN] Make sure that exists() does not always return true
    EXPECT_FALSE(m_knownPlugins->exists(path_t("/path/to/nonexistent/plugin.vst3")));
    EXPECT_FALSE(m_knownPlugins->exists(PluginResourceId("nonexistent_plugin")));

    // [GIVEN] New plugin for registration
    AudioPluginInfo newPluginInfo;
    newPluginInfo.meta.id = "DDD";
    newPluginInfo.meta.type = "VstPlugin";
    newPluginInfo.path = "/path/to/new/plugin/plugin.vst";
    newPluginInfo.state = AudioPluginState::Validated;
    expectedPluginInfoList.push_back(newPluginInfo);

    // [THEN] All the plugins will be written to the file
    ByteArray expectedNewPluginsData = pluginInfoListToJson(expectedPluginInfoList);
    EXPECT_CALL(*m_fileSystem, writeFile(m_knownAudioPluginsFilePath, expectedNewPluginsData))
    .WillOnce(Return(muse::make_ok()));

    // [WHEN] Register it
    ret = m_knownPlugins->registerPlugins({ newPluginInfo });

    // [THEN] The plugin successfully registered
    EXPECT_TRUE(ret);

    // [GIVEN] Same plugin (with the same resourceId) is installed in a different location
    AudioPluginInfo duplicatedPluginInfo = newPluginInfo;
    duplicatedPluginInfo.path = "/another/path/to/new/plugin/plugin.vst";
    expectedPluginInfoList.push_back(duplicatedPluginInfo);

    // [THEN] All the plugins will be written to the file
    expectedNewPluginsData = pluginInfoListToJson(expectedPluginInfoList);
    EXPECT_CALL(*m_fileSystem, writeFile(m_knownAudioPluginsFilePath, expectedNewPluginsData))
    .WillOnce(Return(muse::make_ok()));

    // [WHEN] Register it
    ret = m_knownPlugins->registerPlugins({ duplicatedPluginInfo });

    // [THEN] The duplicated plugin successfully registered
    EXPECT_TRUE(ret);

    actualPluginInfoList = m_knownPlugins->pluginInfoList();
    EXPECT_EQ(expectedPluginInfoList.size(), actualPluginInfoList.size());
    for (const AudioPluginInfo& actualInfo : actualPluginInfoList) {
        EXPECT_TRUE(muse::contains(expectedPluginInfoList, actualInfo));
        EXPECT_TRUE(m_knownPlugins->exists(actualInfo.path));
        EXPECT_TRUE(m_knownPlugins->exists(actualInfo.meta.id));
    }

    // [GIVEN] We want to unregister the first plugin in the list
    AudioPluginInfo unregisteredPlugin = muse::takeFirst(expectedPluginInfoList);

    // [THEN] All the plugins will be written to the file (except the unregistered one)
    expectedNewPluginsData = pluginInfoListToJson(expectedPluginInfoList);
    EXPECT_CALL(*m_fileSystem, writeFile(m_knownAudioPluginsFilePath, expectedNewPluginsData))
    .WillOnce(Return(muse::make_ok()));

    // [WHEN] Unregister the plugin
    ret = m_knownPlugins->unregisterPlugins({ unregisteredPlugin.meta.id });

    // [THEN] The plugin successfully unregistered
    EXPECT_TRUE(ret);
    EXPECT_FALSE(m_knownPlugins->exists(unregisteredPlugin.path));
    EXPECT_FALSE(m_knownPlugins->exists(unregisteredPlugin.meta.id));
    EXPECT_EQ(m_knownPlugins->pluginPath(unregisteredPlugin.meta.id), "");
    actualPluginInfoList = m_knownPlugins->pluginInfoList();
    EXPECT_FALSE(muse::contains(actualPluginInfoList, unregisteredPlugin));
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, RegisterPlugins_SkipsDuplicateSameIdSamePath)
{
    // Same id at the same path (e.g. a placeholder over a leftover from a
    // crashed run) must be skipped with a warning, not written twice.
    ON_CALL(*m_fileSystem, writeFile(_, _))
    .WillByDefault(Return(muse::make_ok()));

    ASSERT_TRUE(m_knownPlugins->load());

    AudioPluginInfo info;
    info.meta.id = "Dup";
    info.meta.type = "VstPlugin";
    info.path = "/some/path/Dup.vst3";
    info.state = AudioPluginState::Discovered;

    ASSERT_TRUE(m_knownPlugins->registerPlugins({ info }));

    // second register with same id + same path: graceful skip, no duplicate
    ASSERT_TRUE(m_knownPlugins->registerPlugins({ info }));

    int count = 0;
    for (const auto& plugin : m_knownPlugins->pluginInfoList()) {
        if (plugin.meta.id == "Dup" && plugin.path == "/some/path/Dup.vst3") {
            ++count;
        }
    }
    EXPECT_EQ(count, 1);
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, RegisterPlugins_SameIdDifferentPathSucceeds)
{
    // same id at distinct paths is allowed (a plugin installed twice)
    ON_CALL(*m_fileSystem, writeFile(_, _))
    .WillByDefault(Return(muse::make_ok()));

    ASSERT_TRUE(m_knownPlugins->load());

    AudioPluginInfo a;
    a.meta.id = "Dup";
    a.meta.type = "VstPlugin";
    a.path = "/path/A/Dup.vst3";
    a.state = AudioPluginState::Validated;

    AudioPluginInfo b = a;
    b.path = "/path/B/Dup.vst3";

    EXPECT_TRUE(m_knownPlugins->registerPlugins({ a }));
    EXPECT_TRUE(m_knownPlugins->registerPlugins({ b }));
    EXPECT_EQ(m_knownPlugins->pluginInfoList().size(), 2u);
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, RemovePluginsAtPaths_LeavesSameIdAtOtherPath)
{
    // Removal is keyed by path, not by id: the same plugin installed twice
    // shares an id, and evicting the sibling would make it look new to the next
    // scan, which would evict this one back -- validating both on every launch.
    ON_CALL(*m_fileSystem, writeFile(_, _))
    .WillByDefault(Return(muse::make_ok()));

    ASSERT_TRUE(m_knownPlugins->load());

    AudioPluginInfo a;
    a.meta.id = "Dup";
    a.meta.type = "VstPlugin";
    a.path = "/path/A/Dup.vst3";
    a.state = AudioPluginState::Validated;

    AudioPluginInfo b = a;
    b.path = "/path/B/Dup.vst3";

    ASSERT_TRUE(m_knownPlugins->registerPlugins({ a, b }));
    ASSERT_EQ(m_knownPlugins->pluginInfoList().size(), 2u);

    ASSERT_TRUE(m_knownPlugins->removePluginsAtPaths({ b.path }));

    const AudioPluginInfoList remaining = m_knownPlugins->pluginInfoList();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().path, a.path);
    EXPECT_TRUE(m_knownPlugins->exists(a.path));
    EXPECT_FALSE(m_knownPlugins->exists(b.path));
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, RemovePluginsAtPaths_WritesOnceForTheWholeBatch)
{
    ASSERT_TRUE(m_knownPlugins->load());

    AudioPluginInfoList infos;
    for (const char* name : { "AAA", "BBB", "CCC" }) {
        AudioPluginInfo info;
        info.meta.id = name;
        info.meta.type = "VstPlugin";
        info.path = "/some/path/" + std::string(name) + ".vst3";
        info.state = AudioPluginState::Validated;
        infos.push_back(info);
    }

    // one write for registerPlugins, one for the whole removal batch
    EXPECT_CALL(*m_fileSystem, writeFile(m_knownAudioPluginsFilePath, _))
    .Times(2)
    .WillRepeatedly(Return(muse::make_ok()));

    ASSERT_TRUE(m_knownPlugins->registerPlugins(infos));
    ASSERT_TRUE(m_knownPlugins->removePluginsAtPaths({ infos[0].path, infos[2].path }));

    const AudioPluginInfoList remaining = m_knownPlugins->pluginInfoList();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().meta.id, "BBB");
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, RemovePluginsAtPaths_UnknownPathsDoNotWrite)
{
    ASSERT_TRUE(m_knownPlugins->load());

    EXPECT_CALL(*m_fileSystem, writeFile(_, _))
    .Times(0);

    EXPECT_TRUE(m_knownPlugins->removePluginsAtPaths({ "/not/known/XXX.vst3" }));
    EXPECT_TRUE(m_knownPlugins->removePluginsAtPaths({}));
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, SetPluginsState_MarksEveryEntryUnderPath)
{
    // State changes are keyed by path: a vanished binary flips every id it
    // hosts, the same id installed at another path is left alone.
    ON_CALL(*m_fileSystem, writeFile(_, _))
    .WillByDefault(Return(muse::make_ok()));

    ASSERT_TRUE(m_knownPlugins->load());

    AudioPluginInfo shellA;
    shellA.meta.id = "Shell FxA";
    shellA.meta.type = "VstPlugin";
    shellA.path = "/some/path/SHELL.vst3";
    shellA.state = AudioPluginState::Validated;

    AudioPluginInfo shellB = shellA;
    shellB.meta.id = "Shell FxB";

    AudioPluginInfo copyA = shellA;
    copyA.path = "/another/path/SHELL.vst3";

    ASSERT_TRUE(m_knownPlugins->registerPlugins({ shellA, shellB, copyA }));

    ASSERT_TRUE(m_knownPlugins->setPluginsState({ shellA.path }, AudioPluginState::Missing));

    for (const AudioPluginInfo& plugin : m_knownPlugins->pluginInfoList()) {
        const AudioPluginState expected = plugin.path == shellA.path
                                          ? AudioPluginState::Missing : AudioPluginState::Validated;
        EXPECT_EQ(plugin.state, expected) << plugin.path.toStdString() << " " << plugin.meta.id;
    }
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, Load_MigrationFailure_LeavesRegisterEmpty)
{
    // A failed migration must propagate from load() and leave the register unpopulated.
    JsonObject root;
    root.set("version", 1);
    root.set("plugins", JsonArray {});

    ByteArray oldData = JsonDocument(root).toJson();

    ON_CALL(*m_fileSystem, exists(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(muse::make_ok()));
    ON_CALL(*m_fileSystem, readFile(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(RetVal<ByteArray>::make_ok(oldData)));

    EXPECT_CALL(*m_migrations, migrate(1, CURRENT_KNOWN_AUDIO_PLUGINS_VERSION, _))
    .WillOnce(Return(Ret(static_cast<int>(Ret::Code::UnknownError), std::string("missing migrator"))));

    Ret ret = m_knownPlugins->load();

    EXPECT_FALSE(ret);
    EXPECT_NE(ret.text().find("migrator"), std::string::npos);
    EXPECT_TRUE(m_knownPlugins->pluginInfoList().empty());
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, Load_NewerVersion_ResetsToEmpty)
{
    // A newer-build cache can't be migrated down: load() resets to empty
    // (the scan rebuilds it) instead of failing.
    JsonObject meta;
    meta.set("id", std::string("AAA"));
    meta.set("type", std::string("VstPlugin"));
    JsonObject entry;
    entry.set("meta", meta);
    entry.set("path", std::string("/some/path/AAA.vst3"));
    entry.set("state", audioPluginStateName(AudioPluginState::Validated));

    JsonArray plugins;
    plugins << entry;

    JsonObject root;
    root.set("version", CURRENT_KNOWN_AUDIO_PLUGINS_VERSION + 1);
    root.set("plugins", plugins);

    ON_CALL(*m_fileSystem, exists(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(muse::make_ok()));
    ON_CALL(*m_fileSystem, readFile(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(RetVal<ByteArray>::make_ok(JsonDocument(root).toJson())));

    EXPECT_CALL(*m_migrations, migrate(_, _, _)).Times(0);

    Ret ret = m_knownPlugins->load();

    EXPECT_TRUE(ret);
    EXPECT_TRUE(m_knownPlugins->pluginInfoList().empty());
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, Load_LegacyArrayFormat)
{
    // [GIVEN] A legacy bare-array JSON file (pre-versioning)
    JsonArray array;

    JsonObject metaObj;
    metaObj.set("id", std::string("AAA"));
    metaObj.set("type", std::string("VstPlugin"));
    metaObj.set("hasNativeEditorSupport", true);

    JsonObject mainObj;
    mainObj.set("meta", metaObj);
    mainObj.set("path", std::string("/some/path/to/vst/plugin/AAA.vst3"));
    mainObj.set("enabled", true);

    array << mainObj;

    ByteArray legacyData = JsonDocument(array).toJson();

    ON_CALL(*m_fileSystem, exists(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(muse::make_ok()));
    ON_CALL(*m_fileSystem, readFile(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(RetVal<ByteArray>::make_ok(legacyData)));

    // [THEN] migrate() is called from version 0 to current
    EXPECT_CALL(*m_migrations, migrate(0, CURRENT_KNOWN_AUDIO_PLUGINS_VERSION, _))
    .WillOnce(Return(muse::make_ok()));

    // [WHEN] Loading
    Ret ret = m_knownPlugins->load();

    // [THEN] Plugins parsed successfully
    EXPECT_TRUE(ret);
    EXPECT_TRUE(m_knownPlugins->exists(PluginResourceId("AAA")));
}

TEST_F(AudioPlugins_KnownAudioPluginsRegisterTest, Load_MalformedRow_SkippedKeepsValidRows)
{
    // [GIVEN] A cache with one valid row and one row with an empty id (older
    // builds persisted LV2 entries this way); the bad row must be skipped,
    // not abort the whole load.
    JsonObject goodMeta;
    goodMeta.set("id", std::string("AAA"));
    goodMeta.set("type", std::string("VstPlugin"));
    JsonObject goodRow;
    goodRow.set("meta", goodMeta);
    goodRow.set("path", std::string("/some/path/AAA.vst3"));
    goodRow.set("state", audioPluginStateName(AudioPluginState::Validated));

    JsonObject badMeta;
    badMeta.set("id", std::string(""));
    badMeta.set("type", std::string("Lv2Plugin"));
    JsonObject badRow;
    badRow.set("meta", badMeta);
    badRow.set("path", std::string("http://x/plugin@file:///usr/lib/lv2/x.lv2/"));
    badRow.set("state", audioPluginStateName(AudioPluginState::Discovered));

    JsonArray plugins;
    plugins << goodRow;
    plugins << badRow;

    JsonObject root;
    root.set("version", CURRENT_KNOWN_AUDIO_PLUGINS_VERSION);
    root.set("plugins", plugins);

    ByteArray data = JsonDocument(root).toJson();

    ON_CALL(*m_fileSystem, exists(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(muse::make_ok()));
    ON_CALL(*m_fileSystem, readFile(m_knownAudioPluginsFilePath))
    .WillByDefault(Return(RetVal<ByteArray>::make_ok(data)));

    EXPECT_CALL(*m_migrations, migrate(CURRENT_KNOWN_AUDIO_PLUGINS_VERSION, CURRENT_KNOWN_AUDIO_PLUGINS_VERSION, _))
    .WillOnce(Return(muse::make_ok()));

    // [WHEN] Loading
    Ret ret = m_knownPlugins->load();

    // [THEN] Load succeeds, the malformed row is skipped, the valid row remains.
    EXPECT_TRUE(ret);
    AudioPluginInfoList list = m_knownPlugins->pluginInfoList();
    ASSERT_EQ(list.size(), size_t(1));
    EXPECT_EQ(list.front().meta.id, "AAA");
}
