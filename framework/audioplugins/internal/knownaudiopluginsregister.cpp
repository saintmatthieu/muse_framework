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

#include "knownaudiopluginsregister.h"

#include "global/serialization/json.h"

#include "log.h"

using namespace muse;
using namespace muse::audioplugins;

namespace muse::audioplugins {
static JsonObject attributesToJson(const PluginAttributes& attributes,
                                   const PluginAttributes& runtimeOnly)
{
    JsonObject result;

    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
        if (runtimeOnly.find(it->first) != runtimeOnly.cend()) {
            continue;
        }

        result.set(it->first.toStdString(), it->second.toStdString());
    }

    return result;
}

static JsonObject metaToJson(const PluginMeta& meta, const PluginAttributes& runtimeOnly)
{
    JsonObject result;

    result.set("id", meta.id);
    result.set("type", meta.type);

    if (!meta.vendor.empty()) {
        result.set("vendor", meta.vendor);
    }

    JsonObject attributesJson = attributesToJson(meta.attributes, runtimeOnly);
    if (!attributesJson.empty()) {
        result.set("attributes", attributesJson);
    }

    return result;
}

static PluginAttributes attributesFromJson(const JsonObject& object)
{
    PluginAttributes result;

    for (const std::string& key : object.keys()) {
        result.insert({ String::fromStdString(key), object.value(key).toString() });
    }

    return result;
}

static PluginMeta metaFromJson(const JsonObject& object)
{
    PluginMeta result;

    result.id = object.value("id").toStdString();
    result.type = object.value("type").toStdString();
    result.vendor = object.value("vendor").toStdString();

    JsonValue attributes = object.value("attributes");
    if (attributes.isObject()) {
        result.attributes = attributesFromJson(attributes.toObject());
    }

    return result;
}
}

Ret KnownAudioPluginsRegister::load()
{
    TRACEFUNC;

    m_loaded = false;
    m_pluginInfoMap.clear();
    m_pluginPaths.clear();

    io::path_t knownAudioPluginsPath = configuration()->knownAudioPluginsFilePath();
    if (!fileSystem()->exists(knownAudioPluginsPath)) {
        m_loaded = true;
        return muse::make_ok();
    }

    RetVal<ByteArray> file = fileSystem()->readFile(knownAudioPluginsPath);
    if (!file.ret) {
        LOGE() << "Failed to read known-audio-plugins cache "
               << knownAudioPluginsPath << ": " << file.ret.toString();
        return file.ret;
    }

    std::string err;
    JsonDocument json = JsonDocument::fromJson(file.val, &err);
    if (!err.empty()) {
        LOGE() << "Failed to parse known-audio-plugins cache "
               << knownAudioPluginsPath << ": " << err;
        return Ret(static_cast<int>(Ret::Code::UnknownError), err);
    }

    JsonArray array;
    int fileVersion = 0;

    if (json.isArray()) {
        // legacy format: bare array, treated as version 0
        array = json.rootArray();
    } else if (json.isObject()) {
        JsonObject root = json.rootObject();
        const JsonValue versionVal = root.value("version");
        const JsonValue pluginsVal = root.value("plugins");
        if (!versionVal.isNumber() || !pluginsVal.isArray()) {
            LOGE() << "Malformed known-audio-plugins.json envelope at "
                   << knownAudioPluginsPath << " (expected numeric \"version\" and array \"plugins\")";
            return Ret(static_cast<int>(Ret::Code::UnknownError), "Malformed known_audio_plugins.json envelope");
        }
        fileVersion = versionVal.toInt();
        array = pluginsVal.toArray();
    } else {
        LOGE() << "Unrecognized known-audio-plugins.json root type at "
               << knownAudioPluginsPath << " (expected array or object)";
        return Ret(static_cast<int>(Ret::Code::UnknownError), "Unrecognized known_audio_plugins.json root type");
    }

    if (fileVersion > CURRENT_KNOWN_AUDIO_PLUGINS_VERSION) {
        LOGW() << "known-audio-plugins cache is newer (v" << fileVersion << ") than this build (v"
               << CURRENT_KNOWN_AUDIO_PLUGINS_VERSION << "); resetting it";
        m_ignoredEntries.clear();
        m_loaded = true;
        pluginInfoListChanged().notify();
        return muse::make_ok();
    }

    Ret migrationRet = migrations()->migrate(fileVersion, CURRENT_KNOWN_AUDIO_PLUGINS_VERSION, array);
    if (!migrationRet) {
        LOGE() << "Failed to migrate known-audio-plugins cache from v" << fileVersion
               << " to v" << CURRENT_KNOWN_AUDIO_PLUGINS_VERSION
               << ": " << migrationRet.toString();
        return migrationRet;
    }

    m_ignoredEntries.clear();

    for (size_t i = 0; i < array.size(); ++i) {
        JsonObject object = array.at(i).toObject();

        AudioPluginInfo info;
        info.meta = metaFromJson(object.value("meta").toObject());
        for (const auto& kv : configuration()->runtimeAttributeDefaults()) {
            // assign, not emplace: defaults must override stale values from a legacy cache
            info.meta.attributes[kv.first] = kv.second;
        }
        const io::path_t storedPath = object.value("path").toString();
        info.path = globalConfiguration()->isBundledPath(storedPath)
                    ? globalConfiguration()->fromBundledPath(storedPath)
                    : storedPath;
        info.state = audioPluginStateFromName(object.value("state").toStdString());
        info.errorCode = object.value("errorCode").toInt();

        // id and path are the lookup keys; skip just the malformed row,
        // aborting the whole load would trip asserts in the next scan
        if (info.meta.id.empty() || info.path.empty()) {
            LOGW() << "Skipping malformed known-audio-plugins entry at "
                   << knownAudioPluginsPath << " (missing id or path)";
            continue;
        }

        // a bundled plugin not found in this app may belong to another installed
        // instance; keep its record but don't expose it
        if (globalConfiguration()->isBundledPath(storedPath) && !fileSystem()->exists(info.path)) {
            m_ignoredEntries.push_back(object);
            continue;
        }

        m_pluginPaths.insert(info.path);
        m_pluginInfoMap.emplace(info.meta.id, std::move(info));
    }

    m_loaded = true;
    pluginInfoListChanged().notify();
    return muse::make_ok();
}

Ret KnownAudioPluginsRegister::clear()
{
    m_pluginInfoMap.clear();
    m_pluginPaths.clear();

    Ret ret = writePluginsInfo();
    m_pluginInfoListChanged.notify();

    return ret;
}

AudioPluginInfoList KnownAudioPluginsRegister::pluginInfoList(PluginInfoAccepted accepted) const
{
    if (!accepted) {
        return muse::values(m_pluginInfoMap);
    }

    AudioPluginInfoList result;
    result.reserve(m_pluginInfoMap.size());

    for (auto it = m_pluginInfoMap.cbegin(); it != m_pluginInfoMap.cend(); ++it) {
        if (accepted(it->second)) {
            result.push_back(it->second);
        }
    }

    return result;
}

muse::async::Notification KnownAudioPluginsRegister::pluginInfoListChanged() const
{
    return m_pluginInfoListChanged;
}

const io::path_t& KnownAudioPluginsRegister::pluginPath(const PluginResourceId& resourceId) const
{
    auto it = m_pluginInfoMap.find(resourceId);
    if (it == m_pluginInfoMap.end()) {
        static const io::path_t _dummy;
        return _dummy;
    }

    return it->second.path;
}

bool KnownAudioPluginsRegister::exists(const io::path_t& pluginPath) const
{
    return muse::contains(m_pluginPaths, pluginPath);
}

bool KnownAudioPluginsRegister::exists(const PluginResourceId& resourceId) const
{
    return muse::contains(m_pluginInfoMap, resourceId);
}

Ret KnownAudioPluginsRegister::registerPlugins(const AudioPluginInfoList& list)
{
    IF_ASSERT_FAILED(m_loaded) {
        return false;
    }

    if (list.empty()) {
        return make_ok();
    }

    bool changed = false;

    for (const AudioPluginInfo& info : list) {
        auto it = m_pluginInfoMap.find(info.meta.id);
        if (it != m_pluginInfoMap.end()) {
            if (it->second.path == info.path) {
                LOGW() << "Plugin is already registered: " << info.path << ", ID: " << info.meta.id;
                continue;
            }
        }

        m_pluginInfoMap.emplace(info.meta.id, info);
        m_pluginPaths.insert(info.path);
        changed = true;
    }

    if (changed) {
        return writePluginsInfo();
    }

    return make_ok();
}

Ret KnownAudioPluginsRegister::setPluginsState(const io::paths_t& paths, AudioPluginState state)
{
    IF_ASSERT_FAILED(m_loaded) {
        return false;
    }

    if (paths.empty()) {
        return make_ok();
    }

    bool changed = false;
    for (auto it = m_pluginInfoMap.begin(); it != m_pluginInfoMap.end(); ++it) {
        if (it->second.state != state && muse::contains(paths, it->second.path)) {
            it->second.state = state;
            changed = true;
        }
    }

    if (!changed) {
        return make_ok();
    }

    return writePluginsInfo();
}

Ret KnownAudioPluginsRegister::unregisterPlugins(const PluginResourceIdList& resourceIds)
{
    IF_ASSERT_FAILED(m_loaded) {
        return false;
    }

    if (resourceIds.empty()) {
        return make_ok();
    }

    bool changed = false;

    for (const PluginResourceId& resourceId : resourceIds) {
        if (!exists(resourceId)) {
            continue;
        }

        for (const auto& pair : m_pluginInfoMap) {
            if (pair.first == resourceId) {
                muse::remove(m_pluginPaths, pair.second.path);
            }
        }

        m_pluginInfoMap.erase(resourceId);
        changed = true;
    }

    if (changed) {
        return writePluginsInfo();
    }

    return make_ok();
}

Ret KnownAudioPluginsRegister::removePluginsAtPath(const io::path_t& path)
{
    return removePluginsAtPaths({ path });
}

Ret KnownAudioPluginsRegister::removePluginsAtPaths(const io::paths_t& paths)
{
    IF_ASSERT_FAILED(m_loaded) {
        return false;
    }

    if (paths.empty()) {
        return make_ok();
    }

    const std::set<io::path_t> pathsToRemove(paths.cbegin(), paths.cend());

    bool removed = false;
    for (auto it = m_pluginInfoMap.begin(); it != m_pluginInfoMap.end();) {
        if (muse::contains(pathsToRemove, it->second.path)) {
            it = m_pluginInfoMap.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }

    if (!removed) {
        return make_ok();
    }

    for (const io::path_t& path : pathsToRemove) {
        muse::remove(m_pluginPaths, path);
    }

    return writePluginsInfo();
}

Ret KnownAudioPluginsRegister::writePluginsInfo()
{
    TRACEFUNC;

    JsonArray array;

    const PluginAttributes& runtimeOnly = configuration()->runtimeAttributeDefaults();

    for (const auto& pair : m_pluginInfoMap) {
        const AudioPluginInfo& info = pair.second;

        const io::path_t storedPath = globalConfiguration()->isBundledWithApp(info.path)
                                      ? globalConfiguration()->toBundledPath(info.path)
                                      : info.path;

        JsonObject obj;
        obj.set("meta", metaToJson(info.meta, runtimeOnly));
        obj.set("path", storedPath.toStdString());

        // load() reads a missing "state" back as Undefined, so omitting it round-trips
        if (info.state != AudioPluginState::Undefined) {
            obj.set("state", audioPluginStateName(info.state));
        }

        if (info.errorCode != 0) {
            obj.set("errorCode", info.errorCode);
        }

        array << obj;
    }

    // write ignored records back untouched to preserve other instances' settings
    for (const JsonObject& ignored : m_ignoredEntries) {
        array << ignored;
    }

    JsonObject root;
    root.set("version", CURRENT_KNOWN_AUDIO_PLUGINS_VERSION);
    root.set("plugins", array);

    io::path_t knownAudioPluginsPath = configuration()->knownAudioPluginsFilePath();
    Ret ret = fileSystem()->writeFile(knownAudioPluginsPath, JsonDocument(root).toJson());

    return ret;
}

Ret KnownAudioPluginsRegister::writePluginsTo(const io::path_t& file, const AudioPluginInfoList& list) const
{
    TRACEFUNC;

    JsonArray array;
    const PluginAttributes& runtimeOnly = configuration()->runtimeAttributeDefaults();

    for (const AudioPluginInfo& info : list) {
        JsonObject obj;
        obj.set("meta", metaToJson(info.meta, runtimeOnly));
        obj.set("path", info.path.toStdString());

        if (info.state != AudioPluginState::Undefined) {
            obj.set("state", audioPluginStateName(info.state));
        }

        if (info.errorCode != 0) {
            obj.set("errorCode", info.errorCode);
        }

        array << obj;
    }

    JsonObject root;
    root.set("version", CURRENT_KNOWN_AUDIO_PLUGINS_VERSION);
    root.set("plugins", array);

    return fileSystem()->writeFile(file, JsonDocument(root).toJson());
}

RetVal<AudioPluginInfoList> KnownAudioPluginsRegister::readPluginsFrom(const io::path_t& file) const
{
    TRACEFUNC;

    RetVal<ByteArray> bytes = fileSystem()->readFile(file);
    if (!bytes.ret) {
        return RetVal<AudioPluginInfoList>(bytes.ret);
    }

    std::string err;
    JsonDocument json = JsonDocument::fromJson(bytes.val, &err);
    if (!err.empty()) {
        return RetVal<AudioPluginInfoList>(Ret(static_cast<int>(Ret::Code::UnknownError), err));
    }

    JsonArray array = json.rootObject().value("plugins").toArray();

    AudioPluginInfoList result;
    result.reserve(array.size());

    for (size_t i = 0; i < array.size(); ++i) {
        JsonObject object = array.at(i).toObject();

        AudioPluginInfo info;
        info.meta = metaFromJson(object.value("meta").toObject());
        info.path = object.value("path").toString();
        info.state = audioPluginStateFromName(object.value("state").toStdString());
        info.errorCode = object.value("errorCode").toInt();

        if (info.meta.id.empty() || info.path.empty()) {
            continue;
        }

        result.push_back(std::move(info));
    }

    return RetVal<AudioPluginInfoList>::make_ok(result);
}
