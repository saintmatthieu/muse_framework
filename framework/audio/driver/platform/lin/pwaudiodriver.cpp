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

/*
 * Pipewire Audio Driver implementation.
 *
 * There are two main objects involved:
 *  - the registry, which is responsible to enumerate devices
 *  - the stream, which is responsible to fetch audio data and send it to the device
 *
 * An additional thread is created via the pw_thread_loop utility.
 * Pipewire call all events/listeners of the registry and stream on this thread with the loop lock held.
 * The main thread must also hold a lock on the loop when calling registry and stream functions
 * in order to be race safe.
 */

#include "pwaudiodriver.h"

#include "containers.h"
#include "log.h"
#include "runtime.h"
#include "translation.h"

#include <pipewire/pipewire.h>
#include <spa/debug/dict.h>
#include <spa/debug/pod.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/support/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#define DEBUG_AUDIO
#ifdef DEBUG_AUDIO
#define LOG_AUDIO LOGD
#else
#define LOG_AUDIO LOGN
#endif

using namespace muse;
using namespace muse::audio;

namespace {
// #define DEBUG_PW
#ifdef DEBUG_PW
static constexpr enum spa_log_level PW_LOG_LEVEL = SPA_LOG_LEVEL_TRACE;
#else
static constexpr enum spa_log_level PW_LOG_LEVEL = SPA_LOG_LEVEL_INFO;
#endif

static constexpr char PW_THREAD_NAME[] = "pw-loop";
static constexpr char PW_DEFAULT_DEVICE[] = "default"; // symbolic name only, could be anything

class PwLoopLock
{
    struct pw_thread_loop* loop;
public:
    explicit PwLoopLock(pw_thread_loop* loop)
        : loop(loop)
    {
        pw_thread_loop_lock(loop);
    }

    ~PwLoopLock()
    {
        pw_thread_loop_unlock(loop);
    }

    PwLoopLock(const PwLoopLock&) = delete;
    PwLoopLock& operator=(const PwLoopLock&) = delete;
};
}

namespace muse::audio {
// PwRegistry enumerates audio devices.
//
// It uses the same event loop and connection (pw_core) as the data stream.
// It also provides a notification when the list of devices changes.
//
// Pipewire will call the register function several times during intialization phase
// and then issue a "done" event when roundtrip is finished.
class PwRegistry
{
public:
    PwRegistry(pw_core* core);
    ~PwRegistry();

    PwRegistry() = delete;
    PwRegistry(const PwRegistry&) = delete;
    PwRegistry& operator=(const PwRegistry&) = delete;

    // init() method must be done at the same time the thread loop is started.
    void init();

    AudioDeviceList getDevices() const;
    async::Notification availableOutputDevicesChanged() const
    {
        return m_devicesChanged;
    }

private:

    // C-style callbacks and their corresponding C++ methods
    static void c_register_global(void* data, uint32_t id, uint32_t /* perms*/, const char* type, uint32_t /*version*/,
                                  const struct spa_dict* props);
    void registerGlobal(uint32_t id, const char* type, const struct spa_dict* props);

    static void c_unregister_global(void* data, uint32_t id);
    void unregisterGlobal(uint32_t id);

    static void c_roundtrip_done(void* data, uint32_t id, int seq);
    void roundtripDone(uint32_t id, int seq);

    bool initDone() const
    {
        return m_pending != 0 || m_roundtripped;
    }

    void devicesDirty();

    struct SinkNode {
        uint32_t id;
        std::string name;
        std::string nick;
        std::string desc;
        uint32_t deviceId;
    };

    struct Device {
        uint32_t id;
        std::string name;
        std::string nick;
        std::string desc;
    };

    pw_core* m_core = nullptr;
    pw_registry* m_registry = nullptr;

    struct spa_hook m_registryHook = {};  // binds m_registryEvents with this
    struct pw_registry_events m_registryEvents = {};
    struct spa_hook m_roundtripHook = {}; // binds m_roundtripEvents with this
    struct pw_core_events m_roundtripEvents = {};

    std::vector<SinkNode> m_pwNodes;
    std::vector<Device> m_pwDevices;

    async::Notification m_devicesChanged;

    mutable AudioDeviceList m_devices;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cond;
    mutable bool m_devicesDirty = true;
    int m_pending = 0;
    bool m_roundtripped = false;
};
}

PwRegistry::PwRegistry(pw_core* core)
    : m_core{core}
    , m_registry{nullptr}
    , m_registryEvents{PW_VERSION_REGISTRY_EVENTS, c_register_global, c_unregister_global}
    , m_roundtripEvents{}
    , m_devicesDirty{true}
    , m_pending{0}
    , m_roundtripped{false}
{
    m_roundtripEvents.version = PW_VERSION_CORE_EVENTS;
    m_roundtripEvents.done = c_roundtrip_done;

    m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);

    pw_core_add_listener(core, &m_roundtripHook, &m_roundtripEvents, this);
    pw_registry_add_listener(m_registry, &m_registryHook, &m_registryEvents, this);
}

PwRegistry::~PwRegistry()
{
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
}

void PwRegistry::init()
{
    // core sync method triggers the "done" event after roundtrip
    m_pending = pw_core_sync(m_core, PW_ID_CORE, 0);
}

AudioDeviceList PwRegistry::getDevices() const
{
    std::unique_lock<std::mutex> lk(m_mutex);

    if (!m_devicesDirty) {
        return m_devices;
    }

    LOGD() << "reconstructing device list";

    m_devices.clear();
    m_devices.push_back({ PW_DEFAULT_DEVICE, muse::trc("audio", "System default") });

    if (!initDone()) {
        // waiting for m_cond before init would deadlock !! (if not for timeout)
        // because this is the main thread, and the thread loop is not yet started
        return m_devices;
    }

    if (!m_roundtripped) {
        LOGD() << "waiting for devices...";
    }
    m_cond.wait_for(lk, std::chrono::milliseconds(500), [this] { return m_roundtripped; });

    LOGD() << "found " << m_pwNodes.size() + 1 << " devices";

    for (const auto& node : m_pwNodes) {
        const auto dev = std::find_if(
            m_pwDevices.begin(), m_pwDevices.end(),
            [node](const Device& device) { return device.id == node.deviceId; });

        const auto devFound = dev != m_pwDevices.end();

        // pipewire identifies node by the "node.name" property
        // (e.g. "alsa_output.pci-0000_04_00.6.analog-stereo")
        // or by the "object.serial" (which we are not using)
        AudioDeviceID id = node.name;

        // priority order for human readable name:
        // - device nickname        (e.g. "HD-Audio Generic")
        // - node nickname          (e.g. "Generic Analog")
        // - device description     (e.g. "Family 17h/19h HD Audio Controller")
        // - node description       (e.g. "Family 17h/19h HD Audio Controller Analog Stereo")
        // - node name              (e.g. "alsa_output.pci-0000_04_00.6.analog-stereo")
        std::string name;
        if (devFound && !dev->nick.empty()) {
            name = dev->nick;
        } else if (!node.nick.empty()) {
            name = node.nick;
        } else if (devFound && !dev->desc.empty()) {
            name = dev->desc;
        } else if (!node.desc.empty()) {
            name = node.desc;
        } else {
            name = node.name;
        }

        m_devices.push_back({ id, name });
    }

    m_devicesDirty = false;

    return m_devices;
}

void PwRegistry::c_roundtrip_done(void* data, uint32_t id, int seq)
{
    auto registry = static_cast<PwRegistry*>(data);
    registry->roundtripDone(id, seq);
}

void PwRegistry::roundtripDone(uint32_t id, int seq)
{
    if (id == PW_ID_CORE && seq == m_pending) {
        std::scoped_lock lk { m_mutex };

        m_roundtripped = true;
        m_pending = 0;
        m_cond.notify_all();
        m_devicesChanged.notify();
    }
}

void PwRegistry::c_register_global(void* data, uint32_t id, uint32_t /* permissions*/, const char* type, uint32_t /*version*/,
                                   const struct spa_dict* props)
{
    auto registry = static_cast<PwRegistry*>(data);
    registry->registerGlobal(id, type, props);
}

void PwRegistry::registerGlobal(uint32_t id, const char* type, const struct spa_dict* props)
{
    // For playback, we are interested in Audio/Sink only.
    // We fetch also Audio/Device, because they arguably have neater nick & description than Audio/Sink.
    // See `getDevices` for details.

    if (spa_streq(type, PW_TYPE_INTERFACE_Device)) {
        auto mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (spa_streq(mediaClass, "Audio/Device")) {
            auto name = spa_dict_lookup(props, PW_KEY_DEVICE_NAME);
            auto nick = spa_dict_lookup(props, PW_KEY_DEVICE_NICK);
            auto desc = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION);

            LOGD() << "Registering device: " << name << " (" << id << ")";

            std::scoped_lock<std::mutex> lock(m_mutex);
            m_pwDevices.push_back(Device {
                id,
                name,
                nick ? nick : "",
                desc ? desc : ""
            });
        }
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        auto mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (spa_streq(mediaClass, "Audio/Sink")) {
            auto name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            auto nick = spa_dict_lookup(props, PW_KEY_NODE_NICK);
            auto desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
            auto deviceIdStr = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
            auto deviceId = static_cast<uint32_t>(deviceIdStr ? std::strtoul(deviceIdStr, nullptr, 10) : 0);

            LOGD() << "Registering sink node: " << name << " (device " << deviceId << ")";

            std::scoped_lock<std::mutex> lock(m_mutex);
            m_pwNodes.push_back(SinkNode {
                id,
                name,
                nick ? nick : "",
                desc ? desc : "",
                deviceId
            });
            devicesDirty();
        }
    }
}

void PwRegistry::c_unregister_global(void* data, uint32_t id)
{
    auto registry = static_cast<PwRegistry*>(data);
    registry->unregisterGlobal(id);
}

void PwRegistry::unregisterGlobal(uint32_t id)
{
    std::scoped_lock<std::mutex> lock(m_mutex);

    const auto numN = m_pwNodes.size();

    muse::remove_if(m_pwDevices, [id](const Device& device) {
        return device.id == id;
    });
    muse::remove_if(m_pwNodes, [id](const SinkNode& node) {
        return node.id == id;
    });

    if (numN != m_pwNodes.size()) {
        devicesDirty();
    }
}

// Called from "pw-loop" thread
// Must be called with mutex locked
void PwRegistry::devicesDirty()
{
    m_devicesDirty = true;
    if (m_roundtripped) {
        // if already roundtripped, it is probably a device hotplug while application is running.
        m_devicesChanged.notify();
    }
}

namespace muse::audio {
//! NOTE The output monitor (IAudioOutputMonitor). A level is the quantum asked for (node.latency):
//! the graph's cycle, which is also what is kept queued for the device, since a stream that doesn't
//! process in PipeWire's realtime thread (as this one) fills a buffer for the next cycle. The engine's
//! block stays the spec's, the least level: a callback asking for more renders it all at once
struct PwMonitor {
    std::atomic<size_t> requestedLevel { std::numeric_limits<size_t>::max() }; // the last, until chosen
    std::atomic<double> delayMs { 0.0 };
    std::atomic<double> periodMs { 0.0 };
    //! NOTE Set when the level changes: the graph's change of quantum starves the stream for a cycle
    //! or so, which is no underrun of the app's (and happens while nothing sounds)
    std::atomic<bool> restartCounting { false };

    mutable std::mutex levelsMutex;
    std::vector<uint32_t> levelFrames; // under levelsMutex
    double rate = 0.0; // under levelsMutex

    //! NOTE What the windows are made of: gathered on the "pw-loop" thread, handed over when the lock
    //! is free (the stream's thread never waits for it)
    struct Gathered {
        uint32_t callbacks = 0;
        uint32_t underruns = 0;
        double headroomSumMs = 0.0;
        double headroomMinMs = std::numeric_limits<double>::infinity();
        double loadSum = 0.0;
        double loadMax = 0.0;
        double latencySumMs = 0.0;
        uint32_t latencyCount = 0;
        float peak = 0.0f;

        void add(const Gathered& o)
        {
            callbacks += o.callbacks;
            underruns += o.underruns;
            headroomSumMs += o.headroomSumMs;
            headroomMinMs = std::min(headroomMinMs, o.headroomMinMs);
            loadSum += o.loadSum;
            loadMax = std::max(loadMax, o.loadMax);
            latencySumMs += o.latencySumMs;
            latencyCount += o.latencyCount;
            peak = std::max(peak, o.peak);
        }
    };
    Gathered gathering; // "pw-loop" thread
    std::mutex windowMutex;
    Gathered gathered; // under windowMutex
    double lastLatencyMs = 0.0; // under windowMutex

    //! NOTE "pw-loop" thread, reset at each opening
    uint64_t callbacksSinceStart = 0;
    uint64_t framesQueued = 0;
    int64_t lastStarved = 0;
    int64_t lastNow = 0;

    void restart()
    {
        callbacksSinceStart = 0;
        framesQueued = 0;
        lastStarved = 0;
        lastNow = 0;
    }

    uint32_t levelFramesFor(size_t level) const
    {
        std::scoped_lock lock { levelsMutex };
        return levelFrames.empty() ? 0 : levelFrames[std::min(level, levelFrames.size() - 1)];
    }

    void observe(const pw_time& time, int64_t start, int64_t end, uint32_t frames, float peak);
};

class PwStream
{
public:
    PwStream(pw_core* core, const IAudioDriver::Spec& spec, const std::string& deviceId, uint32_t latencyFrames,
             PwMonitor* monitor);
    ~PwStream();

    IAudioDriver::Spec spec() const
    {
        return m_spec;
    }

    void suspend();
    void resume();

    //! NOTE The quantum asked for, on the stream as it runs: the graph's quantum comes down to the
    //! least asked, and goes back up once no one asks for less
    void setLatency(uint32_t frames);

private:

    // C-style callbacks and their corresponding C++ methods
    static void c_process(void* userdata);
    void process();

    pw_core* m_core = nullptr;

    spa_hook m_hook;  // binds m_events with this
    pw_stream_events m_events;
    struct pw_stream* m_stream = nullptr;

    IAudioDriver::Spec m_spec;
    size_t m_stride;
    PwMonitor* m_monitor = nullptr;
};
}

// Called from main thread
PwStream::PwStream(pw_core* core, const IAudioDriver::Spec& spec, const std::string& deviceId, uint32_t latencyFrames,
                   PwMonitor* monitor)
    : m_core{core}
    , m_events{}
    , m_spec{spec}
    , m_monitor{monitor}
{
    m_events.version = PW_VERSION_STREAM_EVENTS;
    m_events.process = c_process;

    auto props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        nullptr);

    if (m_spec.output.samplesPerChannel == 0) {
        // could be null if upgrading from a previous version
        m_spec.output.samplesPerChannel = MINIMUM_BUFFER_SIZE * 4;
    }

    m_spec.output.samplesPerChannel = std::max(MINIMUM_BUFFER_SIZE, m_spec.output.samplesPerChannel);
    m_spec.output.samplesPerChannel = std::min(MAXIMUM_BUFFER_SIZE, m_spec.output.samplesPerChannel);
    // Request for a specific number of samples.
    // This is done through the "node.latency" property.
    // Note: user system configuration can override this. e.g.:
    //  - PIPEWIRE_QUANTUM or PIPEWIRE_LATENCY environment variable.
    //  - "clock.force-quantum" setting
    // The margin's level, if more than the block
    std::ostringstream lat;
    lat << std::max<uint32_t>(latencyFrames, m_spec.output.samplesPerChannel) << "/" << m_spec.output.sampleRate;
    pw_properties_set(props, PW_KEY_NODE_LATENCY, lat.str().c_str());

    if (deviceId != PW_DEFAULT_DEVICE) {
        // Request for a specific output device.
        // This is done through the "target.object" property.
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, deviceId.c_str());
    }

    spa_audio_info_raw formatInfo {};
    formatInfo.rate = m_spec.output.sampleRate;
    formatInfo.channels = m_spec.output.audioChannelCount;
    formatInfo.format = SPA_AUDIO_FORMAT_F32;
    m_stride = m_spec.output.audioChannelCount * 4;

    char buf[1024];
    auto builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &formatInfo);

    m_stream = pw_stream_new(m_core, "MuseScore", props);

    pw_stream_add_listener(m_stream, &m_hook, &m_events, this);

    pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
                                                   | PW_STREAM_FLAG_MAP_BUFFERS),
                      &param, 1);
}

// Called from main thread
PwStream::~PwStream()
{
    pw_stream_flush(m_stream, false);
    pw_stream_disconnect(m_stream);
    pw_stream_destroy(m_stream);
}

// Called from main thread, with the loop locked
void PwStream::setLatency(uint32_t frames)
{
    std::ostringstream lat;
    lat << std::max<uint32_t>(frames, m_spec.output.samplesPerChannel) << "/" << m_spec.output.sampleRate;
    const std::string value = lat.str();
    const spa_dict_item item = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, value.c_str());
    const spa_dict dict = SPA_DICT_INIT(&item, 1);
    pw_stream_update_properties(m_stream, &dict);
    LOGI() << "node.latency: " << value;
}

// Called from main thread
void PwStream::suspend()
{
    pw_stream_set_active(m_stream, false);
}

// Called from main thread
void PwStream::resume()
{
    pw_stream_set_active(m_stream, true);
}

void PwStream::c_process(void* userdata)
{
    PwStream* stream = static_cast<PwStream*>(userdata);
    stream->process();
}

void PwStream::process()
{
    // Testing the detection: as if called late
    if (const double delayMs = m_monitor ? m_monitor->delayMs.exchange(0.0) : 0.0; delayMs > 0.0) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delayMs * 1000.0)));
    }

    const int64_t start = pw_stream_get_nsec(m_stream);
    pw_time time {};
    pw_stream_get_time_n(m_stream, &time, sizeof(time));

    auto b = pw_stream_dequeue_buffer(m_stream);
    if (!b || !b->buffer) {
        LOG_AUDIO() << "out of buffers";
        return;
    }

    auto buf = b->buffer;
    auto dst = static_cast<uint8_t*>(buf->datas[0].data);
    if (!dst) {
        return;
    }

    const auto maxFrames = buf->datas[0].maxsize / m_stride;
    const auto numFrames = b->requested ? SPA_MIN(b->requested, maxFrames) : maxFrames;
    const auto len = numFrames * m_stride;

    m_spec.callback(dst, len);

    // The loudest sample (MuseScore writes floats)
    float peak = 0.0f;
    const float* samples = reinterpret_cast<const float*>(dst);
    for (size_t i = 0; i < numFrames * m_spec.output.audioChannelCount; ++i) {
        peak = std::max(peak, std::abs(samples[i]));
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = m_stride;
    buf->datas[0].chunk->size = len;
    // In frames: the stream sums them in its time info (pw_time::queued)
    b->size = numFrames;

    pw_stream_queue_buffer(m_stream, b);

    if (m_monitor) {
        m_monitor->observe(time, start, pw_stream_get_nsec(m_stream), numFrames, peak);
    }
}

// Called from "pw-loop" thread
void PwMonitor::observe(const pw_time& time, int64_t start, int64_t end, uint32_t frames, float peak)
{
    if (time.rate.denom == 0 || frames == 0) {
        return;
    }
    const double graphRate = static_cast<double>(time.rate.denom) / time.rate.num;
    const double periodS = frames / graphRate;
    periodMs = periodS * 1000.0;

    // Underruns: the graph's clock (ticks, frames played) goes on, while the frames it took from
    // the stream don't (a cycle that found none queued). PipeWire doesn't count them for a stream
    const int64_t taken = static_cast<int64_t>(framesQueued) - static_cast<int64_t>(time.queued);
    const int64_t starved = static_cast<int64_t>(time.ticks) - taken;
    framesQueued += frames;

    // Startup isn't counted (the first cycles come before anything sounds), nor a stream that
    // wasn't run for a while (moved, suspended): what was missed then isn't an underrun
    const bool gap = lastNow > 0 && time.now - lastNow > 1'000'000'000;
    lastNow = time.now;
    if (restartCounting.exchange(false)) {
        callbacksSinceStart = 0;
    }
    if (++callbacksSinceStart <= 20 || gap) {
        lastStarved = starved;
        return;
    }
    const int64_t starvedFrames = starved - lastStarved;
    lastStarved = starved;

    // This callback's frames are needed by the next cycle: the one after that the time is of
    double headroomMs = (time.now + periodS * 1e9 - end) / 1e6;
    Gathered& g = gathering;
    ++g.callbacks;
    if (starvedFrames > frames / 2) {
        ++g.underruns;
        headroomMs = std::min(headroomMs, -starvedFrames / graphRate * 1000.0);
        LOGW() << "underrun: the graph played " << starvedFrames / graphRate * 1000.0 << " ms without the stream's frames";
    }
    g.headroomSumMs += headroomMs;
    g.headroomMinMs = std::min(g.headroomMinMs, headroomMs);
    const double load = (end - start) / (periodS * 1e9) * 100.0;
    g.loadSum += load;
    g.loadMax = std::max(g.loadMax, load);
    // From this callback to its first frame played: the next cycle, after what is still queued,
    // and the graph's delay to the device
    const double delayS = static_cast<double>(time.delay) * time.rate.num / time.rate.denom;
    const double latencyMs = ((time.now - start) / 1e9 + periodS + (time.queued + time.buffered) / graphRate + delayS)
                             * 1000.0;
    if (latencyMs > 0.0) {
        g.latencySumMs += latencyMs;
        ++g.latencyCount;
    }
    g.peak = std::max(g.peak, peak);

    if (windowMutex.try_lock()) {
        gathered.add(g);
        if (g.latencyCount > 0) {
            lastLatencyMs = g.latencySumMs / g.latencyCount;
        }
        windowMutex.unlock();
        g = {};
    }
}

PwAudioDriver::PwAudioDriver()
    : m_monitor{std::make_unique<PwMonitor>()}
{
    static bool initDone = false;

    if (!initDone) {
        pw_log_set_level(PW_LOG_LEVEL);
        pw_init(nullptr, nullptr);
        initDone = true;
    }

    m_loop = pw_thread_loop_new(PW_THREAD_NAME, nullptr);
    if (!m_loop) {
        return;
    }

    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context) {
        return;
    }

    pw_log_set_level(PW_LOG_LEVEL); // log level is reset by pw_context_new

    m_core = pw_context_connect(m_context, nullptr, 0);

    // m_core is valid if we are connected to PipeWire
    if (m_core) {
        m_registry = std::make_unique<PwRegistry>(m_core);

        // Actually kick-in the device discovery
        // we do it here rather than in init() in order to have
        // the full list available before the stream is opened
        //
        // If not the stream will first open with "default", and reopen
        // with the preferred device (if not "default")
        //
        // The discovery is effectively done in masked time,
        // while all application modules are initialized.
        m_registry->init();
        pw_thread_loop_start(m_loop);
    }
}

PwAudioDriver::~PwAudioDriver()
{
    if (m_core) {
        {
            PwLoopLock lk { m_loop };
            m_registry.reset();
            m_stream.reset();
            pw_core_disconnect(m_core);
        }

        pw_thread_loop_stop(m_loop);
    }
    if (m_context) {
        pw_context_destroy(m_context);
    }
    if (m_loop) {
        pw_thread_loop_destroy(m_loop);
    }
}

void PwAudioDriver::init()
{
    IF_ASSERT_FAILED_X(m_core, "Not connected to PipeWire. Should fallback to ALSA!") {
        return;
    }

    // Nothing to do
    constexpr auto pwVersion = pw_get_headers_version();
    LOGI() << "Initialized Pipewire";
    LOGD() << "Compiled with version " << pwVersion;
    LOGD() << "Running with version " << pw_get_library_version();
}

std::string PwAudioDriver::name() const
{
    return "PipeWire";
}

AudioDeviceID PwAudioDriver::defaultDevice() const
{
    return PW_DEFAULT_DEVICE;
}

bool PwAudioDriver::open(const Spec& spec, Spec* activeSpec)
{
    IF_ASSERT_FAILED_X(m_core, "Not connected to PipeWire. Should fallback to ALSA!") {
        return false;
    }

    PwLoopLock lk { m_loop };

    LOGI() << "Connecting to " << spec.deviceId << " / " << spec.output.sampleRate << "Hz / "
           << spec.output.samplesPerChannel << " samples";

    // The margin's levels: the block, twice, four times (a cycle of 5.3, 10.7, 21.3 ms for 256 frames
    // at 48 kHz). A smaller quantum than the block would bring back the alignment buffer
    {
        std::vector<uint32_t> levels;
        const uint32_t block = std::max<uint32_t>(spec.output.samplesPerChannel, MINIMUM_BUFFER_SIZE);
        for (uint32_t frames = block; frames <= MAXIMUM_BUFFER_SIZE && levels.size() < 3; frames *= 2) {
            levels.push_back(frames);
        }
        if (levels.empty()) {
            levels.push_back(block);
        }
        std::scoped_lock lock { m_monitor->levelsMutex };
        m_monitor->levelFrames = std::move(levels);
        m_monitor->rate = static_cast<double>(spec.output.sampleRate);
    }
    m_monitor->restart();
    m_monitor->periodMs = 0.0;

    m_stream = std::make_unique<PwStream>(m_core, spec, spec.deviceId,
                                          m_monitor->levelFramesFor(m_monitor->requestedLevel), m_monitor.get());

    m_formatSpec = m_stream->spec();
    if (activeSpec) {
        *activeSpec = m_stream->spec();
    }

    m_activeSpecChanged.send(m_formatSpec);

    LOGD() << "Connected to " << spec.deviceId;
    return true;
}

void PwAudioDriver::close()
{
    PwLoopLock lk { m_loop };

    m_stream.reset();
}

bool PwAudioDriver::isOpened() const { return m_stream != nullptr; }

const IAudioDriver::Spec& PwAudioDriver::activeSpec() const { return m_formatSpec; }

async::Channel<IAudioDriver::Spec> PwAudioDriver::activeSpecChanged() const { return m_activeSpecChanged; }

AudioDeviceList PwAudioDriver::availableOutputDevices() const
{
    IF_ASSERT_FAILED_X(m_core, "Not connected to PipeWire. Should fallback to ALSA!") {
        return AudioDeviceList{};
    }

    PwLoopLock lk { m_loop };

    return m_registry->getDevices();
}

async::Notification PwAudioDriver::availableOutputDevicesChanged() const
{
    IF_ASSERT_FAILED_X(m_core, "Not connected to PipeWire. Should fallback to ALSA!") {
        return async::Notification();
    }

    PwLoopLock lk { m_loop };

    return m_registry->availableOutputDevicesChanged();
}

std::vector<samples_t> PwAudioDriver::availableOutputDeviceBufferSizes() const
{
    std::vector<samples_t> result;

    samples_t n = MAXIMUM_BUFFER_SIZE;
    while (n >= MINIMUM_BUFFER_SIZE) {
        result.push_back(n);
        n /= 2;
    }

    std::sort(result.begin(), result.end());

    return result;
}

std::vector<sample_rate_t> PwAudioDriver::availableOutputDeviceSampleRates() const
{
    return {
        44100,
        48000,
        88200,
        96000,
    };
}

IAudioOutputMonitor::Window PwAudioDriver::takeWindow()
{
    PwMonitor::Gathered g;
    double latencyMs = 0.0;
    {
        std::scoped_lock lock { m_monitor->windowMutex };
        g = m_monitor->gathered;
        m_monitor->gathered = {};
        latencyMs = m_monitor->lastLatencyMs;
    }

    Window w;
    w.callbacks = g.callbacks;
    w.underruns = g.underruns;
    if (g.callbacks > 0) {
        w.headroomMeanMs = g.headroomSumMs / g.callbacks;
        w.headroomMinMs = g.headroomMinMs;
        w.loadMeanPercent = g.loadSum / g.callbacks;
        w.loadMaxPercent = g.loadMax;
    }
    w.outputLatencyMs = latencyMs;
    w.outputPeak = g.peak;
    return w;
}

double PwAudioDriver::periodMs() const
{
    return isOpened() ? m_monitor->periodMs.load() : 0.0;
}

std::vector<double> PwAudioDriver::marginLevelsMs() const
{
    std::vector<double> levels;
    if (!isOpened()) {
        return levels;
    }
    std::scoped_lock lock { m_monitor->levelsMutex };
    for (const uint32_t frames : m_monitor->levelFrames) {
        levels.push_back(m_monitor->rate > 0.0 ? frames * 1000.0 / m_monitor->rate : 0.0);
    }
    return levels;
}

size_t PwAudioDriver::marginLevel() const
{
    std::scoped_lock lock { m_monitor->levelsMutex };
    const size_t count = m_monitor->levelFrames.size();
    return count ? std::min(m_monitor->requestedLevel.load(), count - 1) : 0;
}

void PwAudioDriver::setMarginLevel(size_t level)
{
    if (m_monitor->requestedLevel.exchange(level) == level || !m_core) {
        return;
    }
    PwLoopLock lk { m_loop };
    if (m_stream) {
        m_monitor->restartCounting = true;
        m_stream->setLatency(m_monitor->levelFramesFor(level));
    }
}

void PwAudioDriver::delayNextCallback(double ms)
{
    m_monitor->delayMs = ms;
}
