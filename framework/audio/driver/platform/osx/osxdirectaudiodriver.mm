/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
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

#include "osxdirectaudiodriver.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>
#include <QtCore/qsemaphore.h>
#include <Security/cssmconfig.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/HostTime.h>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>

#include "common/audiotypes.h"
#include "common/audioworkgroup.h"
#include "translation.h"
#include "log.h"

typedef AudioDeviceID OSXAudioDeviceID;

using namespace muse;
using namespace muse::audio;

struct ChannelBufferDetails {
    int streamNumber;
    unsigned int hardwareChannelNumber;
    unsigned int dataOffsetSamples;
    unsigned int dataStrideSamples;
};

//! NOTE What IAudioOutputMonitor tells: set up at open, before the IOProc starts; gathered on the
//! IOProc's thread and handed over when the lock is free (the IOProc never waits for it)
struct OSXDirectAudioDriver::Monitor {
    struct Gathered {
        uint32_t callbacks = 0;
        uint32_t underruns = 0;
        uint32_t clockStalls = 0;
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
            clockStalls += o.clockStalls;
            headroomSumMs += o.headroomSumMs;
            headroomMinMs = std::min(headroomMinMs, o.headroomMinMs);
            loadSum += o.loadSum;
            loadMax = std::max(loadMax, o.loadMax);
            latencySumMs += o.latencySumMs;
            latencyCount += o.latencyCount;
            peak = std::max(peak, o.peak);
        }
    };

    //! NOTE Set at open, before the IOProc starts
    double rate = 0.0;
    double safetyOffsetS = 0.0; // the HAL's distance ahead of the device's read position
    double presentationLatencyS = 0.0; // from the device's buffer to the sound: device's and stream's latency

    //! NOTE The levels: the device's buffer in frames. Under levelsMutex
    mutable std::mutex levelsMutex;
    std::vector<uint32_t> levelFrames;
    uint32_t bufferFrames = 0; // the device's

    std::atomic<size_t> requestedLevel { std::numeric_limits<size_t>::max() }; // the last, until chosen
    std::atomic<double> delayMs { 0.0 };
    //! NOTE Set when the level changes: the HAL restarts the device's cycles at the new size, the
    //! stream's position may jump, which is no underrun of the app's (and happens while nothing sounds)
    std::atomic<bool> restartCounting { false };

    uint32_t levelFramesFor(size_t level) const // under levelsMutex
    {
        return levelFrames.empty() ? 0 : levelFrames[std::min(level, levelFrames.size() - 1)];
    }

    //! NOTE The IOProc's thread
    Gathered gathering;
    uint64_t callbacksSinceStart = 0;
    double expectedSampleTime = -1.0;

    std::mutex windowMutex;
    Gathered gathered; // under windowMutex
    double lastLatencyMs = 0.0; // under windowMutex

    void th_startOver()
    {
        gathering = {};
        callbacksSinceStart = 0;
        expectedSampleTime = -1.0;
    }

    void th_record(const AudioTimeStamp* outputTime, UInt64 startHost, UInt64 endHost, uint32_t frames, float peak);
};

static double hostToSeconds(UInt64 hostTime)
{
    return AudioConvertHostTimeToNanos(hostTime) * 1e-9;
}

void OSXDirectAudioDriver::Monitor::th_record(const AudioTimeStamp* outputTime, UInt64 startHost, UInt64 endHost, uint32_t frames,
                                              float peak)
{
    if (rate <= 0.0 || frames == 0) {
        return;
    }

    if (restartCounting.exchange(false)) {
        callbacksSinceStart = 0;
        expectedSampleTime = -1.0;
    }

    // Cycles the HAL skipped (it does when a cycle overloads): the stream's position jumps, and the
    // device played silence there, whoever was late
    uint32_t clockStalls = 0;
    const bool sampleTimeValid = outputTime && (outputTime->mFlags & kAudioTimeStampSampleTimeValid);
    if (sampleTimeValid) {
        if (expectedSampleTime >= 0.0 && callbacksSinceStart > 20) {
            const double skippedFrames = outputTime->mSampleTime - expectedSampleTime;
            if (skippedFrames > frames / 2.0) {
                ++clockStalls;
                LOGW() << "the device skipped " << skippedFrames / rate * 1000.0 << " ms of the stream";
            }
        }
        expectedSampleTime = outputTime->mSampleTime + frames;
    }

    // Startup isn't counted: the first callbacks come irregularly, before anything sounds
    if (++callbacksSinceStart <= 20) {
        return;
    }

    const bool hostTimeValid = outputTime && (outputTime->mFlags & kAudioTimeStampHostTimeValid);
    const double start = hostToSeconds(startHost);
    const double end = hostToSeconds(endHost);

    Gathered& g = gathering;
    ++g.callbacks;
    g.clockStalls += clockStalls;
    if (hostTimeValid) {
        // The frames are needed when the device's read position, plus the safety offset, reaches them
        const double outputStart = hostToSeconds(outputTime->mHostTime);
        const double headroomMs = (outputStart - safetyOffsetS - end) * 1000.0;
        g.headroomSumMs += headroomMs;
        g.headroomMinMs = std::min(g.headroomMinMs, headroomMs);
        if (headroomMs < 0.0) {
            ++g.underruns;
            LOGW() << "underrun: written " << -headroomMs << " ms after the device needed it";
        }
        g.latencySumMs += (outputStart - start + presentationLatencyS) * 1000.0;
        ++g.latencyCount;
    }
    const double load = (end - start) / (frames / rate) * 100.0;
    g.loadSum += load;
    g.loadMax = std::max(g.loadMax, load);
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

struct OSXDirectAudioDriver::Data {
    Spec format;
    std::shared_ptr<Monitor> monitor;
    AudioDeviceIOProcID procId{};
    bool canBeDirectlyMapped = false;

    OSXAudioDeviceID deviceId{};

    std::atomic<bool> stopPending{ false };
    std::atomic<bool> stopped{ false };

    std::vector<ChannelBufferDetails> channelBufferOutputDetails;
    std::vector<float> outBuffer;

    Data& operator=(const Data& other)
    {
        format = other.format;
        monitor = other.monitor;
        procId = other.procId;
        canBeDirectlyMapped = other.canBeDirectlyMapped;
        deviceId = other.deviceId;
        stopPending = other.stopPending.load();
        stopped = other.stopped.load();
        channelBufferOutputDetails = other.channelBufferOutputDetails;
        outBuffer = other.outBuffer;
        return *this;
    }

    void clear()
    {
        *this = Data();
    }
};
namespace muse::audio {
AudioWorkGroup makeAudioWorkgroup(void* opaqueHandle);
AudioWorkGroup OSXDirectAudioDriver::getAudioWorkGroup() const
{
    return m_audioWorkGroup;
}

async::Notification OSXDirectAudioDriver::currentWorkgroupChanged() const
{
    return m_currentWorkgroupChanged;
}
} // namespace muse::audio

OSXDirectAudioDriver::OSXDirectAudioDriver()
    : m_data(std::make_unique<Data>()), m_monitor(std::make_shared<Monitor>())
{
    initDeviceMapListener();
    updateDeviceMap();
}

OSXDirectAudioDriver::~OSXDirectAudioDriver()
{
    removeDeviceMapListener();
    doClose();
}

void OSXDirectAudioDriver::init()
{
}

std::string OSXDirectAudioDriver::name() const
{
    return "OSX";
}

muse::audio::AudioDeviceID OSXDirectAudioDriver::defaultDevice() const
{
    return DEFAULT_DEVICE_ID;
}

static uint32_t outputBufferSamplesPerChannel(const AudioBuffer& buffer)
{
    if (buffer.mNumberChannels == 0) {
        return 0;
    }

    return buffer.mDataByteSize / (buffer.mNumberChannels * sizeof(float));
}

static uint32_t callbackSamplesPerChannel(const AudioBufferList* outputData,
                                          const std::vector<ChannelBufferDetails>& details)
{
    std::optional<uint32_t> result;

    for (const ChannelBufferDetails& detail : details) {
        if (detail.streamNumber < 0 || static_cast<UInt32>(detail.streamNumber) >= outputData->mNumberBuffers) {
            continue;
        }

        const AudioBuffer& outputBuffer = outputData->mBuffers[detail.streamNumber];
        const uint32_t frames = outputBufferSamplesPerChannel(outputBuffer);
        if (frames == 0) {
            continue;
        }

        result = result.has_value() ? std::min(result.value(), frames) : frames;
    }

    return result.value_or(0);
}

static std::optional<uint32_t> getSameRequestedSampleCount(const AudioBufferList* outOutputData,
                                                           const std::vector<ChannelBufferDetails>& details)
{
    std::optional<uint32_t> sampleCount;

    for (const ChannelBufferDetails& detail : details) {
        if (detail.streamNumber < 0 || static_cast<UInt32>(detail.streamNumber) >= outOutputData->mNumberBuffers) {
            continue;
        }

        const AudioBuffer& outputBuffer = outOutputData->mBuffers[detail.streamNumber];
        const uint32_t frames = outputBufferSamplesPerChannel(outputBuffer);
        if (frames == 0) {
            continue;
        }

        if (!sampleCount.has_value()) {
            sampleCount = frames;
        } else if (sampleCount.value() != frames) {
            return std::nullopt;
        }
    }

    return sampleCount;
}

//! NOTE The loudest sample of the first two channels, interleaved by stride
static float peakOf(const float* samples, uint32_t frames, uint32_t stride)
{
    float peak = 0.0f;
    if (!samples || stride == 0) {
        return peak;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        peak = std::max(peak, std::abs(samples[i * stride]));
        if (stride > 1) {
            peak = std::max(peak, std::abs(samples[i * stride + 1]));
        }
    }
    return peak;
}

//! NOTE Renders into the device's buffers; the frames rendered and their peak
static uint32_t renderOutput(OSXDirectAudioDriver::Data* data, AudioBufferList* outOutputData, float& peak)
{
    uint32_t samplesPerChannel = callbackSamplesPerChannel(outOutputData, data->channelBufferOutputDetails);
    if (samplesPerChannel == 0) {
        return 0;
    }

    const uint32_t callbackDataStride = data->format.output.audioChannelCount;
    int dataSize = static_cast<int>(samplesPerChannel * callbackDataStride * sizeof(float));
    if (data->canBeDirectlyMapped && getSameRequestedSampleCount(outOutputData,
                                                                 data->channelBufferOutputDetails).value_or(0) == samplesPerChannel) {
        float* samples = static_cast<float*>(outOutputData->mBuffers[data->channelBufferOutputDetails[0].streamNumber].mData);
        data->format.callback((uint8_t*)samples, dataSize);
        peak = peakOf(samples, samplesPerChannel, callbackDataStride);
        return samplesPerChannel;
    }

    if (data->outBuffer.size() < static_cast<size_t>(samplesPerChannel) * callbackDataStride) {
        samplesPerChannel = static_cast<uint32_t>(data->outBuffer.size() / callbackDataStride);
        dataSize = static_cast<int>(samplesPerChannel * callbackDataStride * sizeof(float));
    }

    data->format.callback((uint8_t*)data->outBuffer.data(), dataSize);
    peak = peakOf(data->outBuffer.data(), samplesPerChannel, callbackDataStride);

    for (int ch = 0; ch < (int)data->channelBufferOutputDetails.size(); ++ch) {
        const auto& details = data->channelBufferOutputDetails[ch];
        auto& outputBuffer = outOutputData->mBuffers[details.streamNumber];
        int stride = details.dataStrideSamples;
        if (stride == 0) {
            continue;
        }

        const uint64_t requiredSamples = details.dataOffsetSamples
                                         + static_cast<uint64_t>(samplesPerChannel - 1) * stride
                                         + 1;
        if (outputBuffer.mDataByteSize < (requiredSamples * sizeof(float))) {
            // this is very unexpected. Should always be the same as samplesPerChannel
            continue;
        }

        float* src = data->outBuffer.data() + ch;
        float* dest = static_cast<float*>(outputBuffer.mData)
                      + details.dataOffsetSamples;
        for (int j = samplesPerChannel; --j >= 0;) {
            *dest = *src;
            dest += stride;
            src += callbackDataStride;
        }
    }
    return samplesPerChannel;
}

static int coreAudioIOProc(AudioObjectID /* inDevice*/,
                           const AudioTimeStamp* /* inNow */,
                           const AudioBufferList* /* inInputData */,
                           const AudioTimeStamp* /*  inInputTime */,
                           AudioBufferList* outOutputData,
                           const AudioTimeStamp* inOutputTime,
                           void* __nullable inClientData)
{
    auto* data = reinterpret_cast<OSXDirectAudioDriver::Data*>(inClientData);
    if (data->stopPending) {
        AudioDeviceStop(data->deviceId, data->procId);
        data->stopPending = false;
        data->stopped = true;
        return noErr;
    }
    if (data->channelBufferOutputDetails.empty()) {
        return noErr;
    }

    OSXDirectAudioDriver::Monitor* monitor = data->monitor.get();

    // Testing the detection: as if woken late
    if (monitor) {
        if (const double delayMs = monitor->delayMs.exchange(0.0); delayMs > 0.0) {
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delayMs * 1000.0)));
        }
    }

    const UInt64 start = AudioGetCurrentHostTime();
    float peak = 0.0f;
    const uint32_t frames = renderOutput(data, outOutputData, peak);
    if (monitor) {
        monitor->th_record(inOutputTime, start, AudioGetCurrentHostTime(), frames, peak);
    }
    return noErr;
}

std::optional<std::array<uint32_t, 2> > getPreferredStereoHardwareChannels(OSXAudioDeviceID deviceId)
{
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster };
    UInt32 stereo[2] = {};
    UInt32 size = sizeof(stereo);

    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, stereo) != noErr) {
        return std::nullopt;
    }
    return std::array<uint32_t, 2> { stereo[0], stereo[1] };
}

bool filterAndReorderStereoHardwareChannels(const OSXAudioDeviceID& deviceId,
                                            std::vector<ChannelBufferDetails>& details)
{
    auto stereoChannels = getPreferredStereoHardwareChannels(deviceId);
    if (!stereoChannels.has_value()) {
        return false;
    }
    details.erase(
        std::remove_if(details.begin(), details.end(),
                       [&stereoChannels](const ChannelBufferDetails& details) {
        return details.hardwareChannelNumber
               != stereoChannels->at(0)
               && details.hardwareChannelNumber
               != stereoChannels->at(1);
    }),
        details.end());
    std::sort(details.begin(), details.end(),
              [&stereoChannels](const ChannelBufferDetails& a,
                                const ChannelBufferDetails& b) {
        auto aIndex = (a.hardwareChannelNumber
                       == stereoChannels->at(0))
                      ? 0
                      : 1;
        auto bIndex = (b.hardwareChannelNumber
                       == stereoChannels->at(0))
                      ? 0
                      : 1;
        return aIndex < bIndex;
    });
    return true;
}

std::vector<ChannelBufferDetails> getChannelBufferDetails(const OSXAudioDeviceID& deviceId,
                                                          const std::function<void(std::string, OSStatus)>& onError)
{
    std::vector<ChannelBufferDetails> result;

    UInt32 size{};
    AudioObjectPropertyAddress address = { kAudioDevicePropertyStreamConfiguration,
                                           kAudioDevicePropertyScopeOutput,
                                           kAudioObjectPropertyElementMaster };
    if (auto status = AudioObjectGetPropertyDataSize(deviceId, &address, 0, nullptr, &size); status != noErr) {
        onError("Failed to get device " + std::to_string(deviceId) + " stream configuration" + ", err: " + std::to_string(status), status);
        return result;
    }

    std::vector<uint8_t> buf(size);

    auto status = AudioObjectGetPropertyData(deviceId, &address, 0, nullptr,
                                             &size, buf.data());
    if (status != noErr) {
        onError("Failed to get device " + std::to_string(deviceId) + " stream configuration" + ", err: " + std::to_string(status), status);
        return result;
    }

    AudioBufferList* bufList = reinterpret_cast<AudioBufferList*>(buf.data());

    const int numStreams = static_cast<int>(bufList->mNumberBuffers);
    unsigned int hardwareChannelNumber = 1;

    for (int i = 0; i < numStreams; ++i) {
        auto& b = bufList->mBuffers[i];
        for (unsigned int j = 0; j < b.mNumberChannels; ++j) {
            ChannelBufferDetails details {
                .streamNumber = i,
                .hardwareChannelNumber = hardwareChannelNumber++,
                .dataOffsetSamples = j,
                .dataStrideSamples
                    =b.mNumberChannels };
            result.push_back(details);
        }
    }

    return result;
}

static bool isSupportedCallbackFormat(const AudioStreamBasicDescription& format)
{
    const bool isNativeEndian = (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian;
    const bool isNativeFloatPacked = (format.mFormatFlags & kAudioFormatFlagsNativeFloatPacked) == kAudioFormatFlagsNativeFloatPacked;

    return format.mFormatID == kAudioFormatLinearPCM
           && isNativeEndian
           && isNativeFloatPacked
           && format.mBitsPerChannel == sizeof(float) * 8
           && format.mFramesPerPacket == 1;
}

static bool validateOutputStreamFormats(OSXAudioDeviceID deviceId,
                                        const std::vector<ChannelBufferDetails>& details,
                                        const std::function<void(std::string, OSStatus)>& logError)
{
    AudioObjectPropertyAddress streamsAddress {
        kAudioDevicePropertyStreams,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMaster
    };

    UInt32 streamsSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(deviceId, &streamsAddress, 0, nullptr, &streamsSize);
    if (status != noErr) {
        logError("Failed to get device " + std::to_string(deviceId) + " output streams size, err: ", status);
        return false;
    }

    std::vector<AudioStreamID> streamIds(streamsSize / sizeof(AudioStreamID));
    status = AudioObjectGetPropertyData(deviceId, &streamsAddress, 0, nullptr, &streamsSize, streamIds.data());
    if (status != noErr) {
        logError("Failed to get device " + std::to_string(deviceId) + " output streams, err: ", status);
        return false;
    }

    std::vector<int> checkedStreams;
    for (const ChannelBufferDetails& detail : details) {
        if (std::find(checkedStreams.cbegin(), checkedStreams.cend(), detail.streamNumber) != checkedStreams.cend()) {
            continue;
        }

        if (detail.streamNumber < 0 || static_cast<size_t>(detail.streamNumber) >= streamIds.size()) {
            LOGE() << "CoreAudio output stream index " << detail.streamNumber
                   << " is outside stream list for device " << deviceId;
            return false;
        }

        AudioStreamBasicDescription format {};
        UInt32 formatSize = sizeof(format);
        AudioObjectPropertyAddress formatAddress {
            kAudioStreamPropertyVirtualFormat,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMaster
        };

        status = AudioObjectGetPropertyData(streamIds[detail.streamNumber], &formatAddress, 0, nullptr, &formatSize, &format);
        if (status != noErr) {
            logError("Failed to get device " + std::to_string(deviceId) + " output stream format, err: ", status);
            return false;
        }

        if (!isSupportedCallbackFormat(format)) {
            LOGE() << "Unsupported CoreAudio output stream format for device " << deviceId
                   << ", stream " << detail.streamNumber
                   << ": expected native packed 32-bit float PCM";
            return false;
        }

        checkedStreams.push_back(detail.streamNumber);
    }

    return true;
}

inline static bool canBeDirectlyMapped(const std::vector<ChannelBufferDetails>& details,
                                       uint32_t callbackDataStride)
{
    std::optional<int> firstStreamNumber;
    std::optional<uint32_t> expectedOffset;
    for (int channel = 0; channel < (int)details.size(); ++channel) {
        const auto& detail = details[channel];
        if (detail.dataStrideSamples != callbackDataStride) {
            return false;
        }
        if (!firstStreamNumber.has_value()) {
            firstStreamNumber = detail.streamNumber;
        }
        if (firstStreamNumber.value() != detail.streamNumber) {
            return false;
        }
        if (!expectedOffset.has_value()) {
            expectedOffset = detail.dataOffsetSamples;
        }
        if (expectedOffset.value() != detail.dataOffsetSamples) {
            return false;
        }
        *expectedOffset += 1;
    }
    return true;
}

static std::vector<ChannelBufferDetails> getFittingChannelStreamsFromDevice(OSXAudioDeviceID deviceId, int channelCount,
                                                                            const std::function<void(std::string, OSStatus)>& logError)
{
    auto outputStreamInfos = getChannelBufferDetails(deviceId, logError);
    if (outputStreamInfos.size()
        < (unsigned int)channelCount) {
        logError("Not enough channels are available with current device", noErr);
        return {};
    }
    if (channelCount == 2) {
        filterAndReorderStereoHardwareChannels(deviceId, outputStreamInfos);
    }
    if (outputStreamInfos.size() < (unsigned int)channelCount) {
        logError("Not enough channels are available with current device after filtering for stereo preference", noErr);
        return {};
    }
    if (outputStreamInfos.size()
        > (unsigned int)channelCount) {
        // if there are more hardware channels than requested, we just use the first ones. This is a fallback if filterStereoHardwareChannels was not successful
        outputStreamInfos.resize(channelCount);
    }
    return outputStreamInfos;
}

static std::optional<Float64> closestAvailableSampleRate(OSXAudioDeviceID deviceId, Float64 requested,
                                                         const std::function<void(std::string, OSStatus)>& onError)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };

    UInt32 size = 0;
    if (auto status = AudioObjectGetPropertyDataSize(deviceId, &address, 0, nullptr, &size); status != noErr || size == 0) {
        onError("Failed to get device " + std::to_string(deviceId) + " available sample rates" + ", err: " + std::to_string(status),
                status);
        return std::nullopt;
    }

    std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
    if (auto status = AudioObjectGetPropertyData(deviceId, &address, 0, nullptr, &size, ranges.data()); status != noErr) {
        onError("Failed to get device " + std::to_string(deviceId) + " available sample rates" + ", err: " + std::to_string(status),
                status);
        return std::nullopt;
    }

    std::optional<Float64> best;
    Float64 bestDistance = std::numeric_limits<Float64>::max();

    for (const AudioValueRange& range : ranges) {
        Float64 candidate = requested;

        if (requested < range.mMinimum) {
            candidate = range.mMinimum;
        } else if (requested > range.mMaximum) {
            candidate = range.mMaximum;
        }

        Float64 distance = std::abs(candidate - requested);
        if (distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

static std::optional<OutputSpec> prepareDeviceWithOutputSpec(OSXAudioDeviceID deviceId, const OutputSpec& spec,
                                                             const std::function<void(std::string, OSStatus)>& logError)
{
    AudioObjectPropertyAddress requestedSampleRate{
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster };

    Float64 currentSampleRate = 0.0;
    UInt32 sampleRateSize = sizeof(currentSampleRate);

    OSStatus result
        =AudioObjectGetPropertyData(deviceId, &requestedSampleRate, 0, nullptr,
                                    &sampleRateSize, &currentSampleRate);

    if (result != noErr) {
        logError("Failed to get current sample rate for device " + std::to_string(deviceId) + ", err: ", result);
        return std::nullopt;
    }

    Float64 sampleRate = closestAvailableSampleRate(deviceId, spec.sampleRate, logError)
                         .value_or(currentSampleRate);

    if (std::abs(currentSampleRate - sampleRate) > 0.5) {
        result = AudioObjectSetPropertyData(
            deviceId,
            &requestedSampleRate,
            0,
            nullptr,
            sizeof(sampleRate),
            &sampleRate);

        if (result != noErr) {
            sampleRate = currentSampleRate;
        }
    } else {
        sampleRate = currentSampleRate;
    }

    AudioValueRange bufferSizeRange = { 0, 0 };
    UInt32 bufferSizeRangeSize = sizeof(AudioValueRange);
    AudioObjectPropertyAddress bufferSizeRangeAddress = {
        .mSelector = kAudioDevicePropertyBufferFrameSizeRange,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster
    };

    result = AudioObjectGetPropertyData(deviceId, &bufferSizeRangeAddress, 0, 0, &bufferSizeRangeSize, &bufferSizeRange);
    if (result != noErr) {
        logError("Failed to create Audio Queue Output, err: ", result);
        return std::nullopt;
    }

    samples_t minBufferSize = static_cast<samples_t>(bufferSizeRange.mMinimum);
    samples_t maxBufferSize = static_cast<samples_t>(bufferSizeRange.mMaximum);
    UInt32 bufferSizeOut = std::min(maxBufferSize, std::max(minBufferSize, spec.samplesPerChannel));

    AudioObjectPropertyAddress preferredBufferSizeAddress = {
        .mSelector = kAudioDevicePropertyBufferFrameSize,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster
    };

    result = AudioObjectSetPropertyData(deviceId, &preferredBufferSizeAddress, 0, 0, sizeof(bufferSizeOut), (void*)&bufferSizeOut);
    if (result != noErr) {
        logError("Failed to create Audio Queue Output, err: ", result);
        return std::nullopt;
    }

    UInt32 actualBufferSizeOut = bufferSizeOut;
    UInt32 actualBufferSizeOutSize = sizeof(actualBufferSizeOut);
    result = AudioObjectGetPropertyData(deviceId, &preferredBufferSizeAddress, 0, 0, &actualBufferSizeOutSize, &actualBufferSizeOut);
    if (result != noErr) {
        logError("Failed to get Audio Device bufferFrameSize, err: ", result);
        return std::nullopt;
    }
    bufferSizeOut = actualBufferSizeOut;

    return OutputSpec {
        .sampleRate = (uint64_t)sampleRate, .samplesPerChannel = bufferSizeOut, .audioChannelCount = spec.audioChannelCount
    };
}

static std::optional<UInt32> getOutputUInt32(AudioObjectID objectId, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster };
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(objectId, &address, 0, nullptr, &size, &value) != noErr) {
        return std::nullopt;
    }
    return value;
}

//! NOTE The margin is the device's buffer: the IOProc renders a buffer in the time the device plays
//! the one before (a cycle), which is also what the buffer adds to the latency. So a level is a
//! buffer size: the engine's block, twice, four times (5.3, 10.7, 21.3 ms for 256 frames at 48 kHz),
//! as PipeWire's quantum. The engine's block stays the spec's, the least level: a callback asking
//! for more renders it all at once. The buffer changes on the running device. Returns the buffer
//! to open with
uint32_t OSXDirectAudioDriver::setUpMonitor(OSXAudioDeviceID deviceId, const OutputSpec& output)
{
    AudioValueRange range { 0, 0 };
    UInt32 rangeSize = sizeof(range);
    AudioObjectPropertyAddress rangeAddress { kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMaster };
    AudioObjectGetPropertyData(deviceId, &rangeAddress, 0, nullptr, &rangeSize, &range);
    const uint32_t least = std::max(static_cast<uint32_t>(range.mMinimum), static_cast<uint32_t>(MINIMUM_BUFFER_SIZE));
    const uint32_t most = std::min(range.mMaximum > 0 ? static_cast<uint32_t>(range.mMaximum) : static_cast<uint32_t>(MAXIMUM_BUFFER_SIZE),
                                   static_cast<uint32_t>(MAXIMUM_BUFFER_SIZE));

    std::vector<uint32_t> levels;
    const uint32_t block = static_cast<uint32_t>(output.samplesPerChannel);
    for (uint32_t frames = block; least <= most && frames > 0 && levels.size() < 3; frames *= 2) {
        const uint32_t clamped = std::clamp(frames, least, most);
        if (levels.empty() || levels.back() != clamped) {
            levels.push_back(clamped);
        }
    }

    const double rate = static_cast<double>(output.sampleRate);
    const UInt32 safetyOffset = getOutputUInt32(deviceId, kAudioDevicePropertySafetyOffset).value_or(0);
    UInt32 presentationLatency = getOutputUInt32(deviceId, kAudioDevicePropertyLatency).value_or(0);
    if (!m_data->channelBufferOutputDetails.empty()) {
        AudioObjectPropertyAddress streamsAddress { kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput,
                                                    kAudioObjectPropertyElementMaster };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(deviceId, &streamsAddress, 0, nullptr, &size) == noErr && size > 0) {
            std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
            const size_t stream = static_cast<size_t>(m_data->channelBufferOutputDetails[0].streamNumber);
            if (AudioObjectGetPropertyData(deviceId, &streamsAddress, 0, nullptr, &size, streams.data()) == noErr
                && stream < streams.size()) {
                AudioObjectPropertyAddress latencyAddress { kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal,
                                                            kAudioObjectPropertyElementMaster };
                UInt32 streamLatency = 0;
                UInt32 latencySize = sizeof(streamLatency);
                if (AudioObjectGetPropertyData(streams[stream], &latencyAddress, 0, nullptr, &latencySize, &streamLatency) == noErr) {
                    presentationLatency += streamLatency;
                }
            }
        }
    }

    std::string listed;
    for (const uint32_t frames : levels) {
        listed += (listed.empty() ? "" : ", ") + std::to_string(frames);
    }
    LOGI() << "margin levels (buffer frames): " << listed << ", block: " << output.samplesPerChannel
           << ", safety offset: " << safetyOffset << ", device and stream latency: " << presentationLatency << " frames";

    Monitor& m = *m_monitor;
    std::scoped_lock lock { m.levelsMutex };
    m.th_startOver(); // the IOProc isn't running
    m.rate = rate;
    m.safetyOffsetS = rate > 0.0 ? safetyOffset / rate : 0.0;
    m.presentationLatencyS = rate > 0.0 ? presentationLatency / rate : 0.0;
    m.levelFrames = std::move(levels);
    m.bufferFrames = m.levelFramesFor(m.requestedLevel);
    return m.bufferFrames;
}

static std::optional<UInt32> setBufferFrames(OSXAudioDeviceID deviceId, UInt32 frames)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMaster };
    if (OSStatus result = AudioObjectSetPropertyData(deviceId, &address, 0, nullptr, sizeof(frames), &frames); result != noErr) {
        LOGE() << "Failed to set the device's buffer to " << frames << " frames, err: " << result;
        return std::nullopt;
    }
    UInt32 actual = 0;
    UInt32 size = sizeof(actual);
    if (AudioObjectGetPropertyData(deviceId, &address, 0, nullptr, &size, &actual) != noErr) {
        return std::nullopt;
    }
    return actual;
}

AudioWorkGroup createAudioWorkgroup(OSXAudioDeviceID deviceId)
{
    AudioObjectPropertyAddress pa;
    pa.mSelector = kAudioDevicePropertyIOThreadOSWorkgroup;
    pa.mScope = kAudioObjectPropertyScopeWildcard;
    pa.mElement = kAudioObjectPropertyElementMaster;
    os_workgroup_t workgroup;
    uint32_t workgroupSize = sizeof(workgroup);
    if (AudioObjectGetPropertyData(deviceId, &pa, 0, nullptr, &workgroupSize,
                                   &workgroup) != noErr) {
        return {};
    }

    return makeAudioWorkgroup(workgroup);
}

bool OSXDirectAudioDriver::open(const Spec& spec, Spec* activeSpec)
{
    if (isOpened()) {
        return false;
    }

    m_data->clear();

    auto deviceId = getAudioDeviceId(spec.deviceId);
    if (!deviceId) {
        logError("Failed to find device " + spec.deviceId, noErr);
        return false;
    }
    auto bestOutputStreamInfos = getFittingChannelStreamsFromDevice(
        *deviceId, spec.output.audioChannelCount, &logError);
    if (bestOutputStreamInfos.empty()) {
        return false;
    }

    IF_ASSERT_FAILED_X(validateOutputStreamFormats(*deviceId, bestOutputStreamInfos, &logError),
                       "CoreAudio output stream format must be native packed 32-bit float PCM") {
        return false;
    }

    auto actualOutputSpec = prepareDeviceWithOutputSpec(*deviceId, spec.output, &logError);
    if (!actualOutputSpec) {
        return false;
    }

    m_data->canBeDirectlyMapped = canBeDirectlyMapped(bestOutputStreamInfos,
                                                      spec.output.audioChannelCount);
    m_data->format = spec;
    m_data->format.output = actualOutputSpec.value();
    m_data->format.deviceId = QString::number(*deviceId).toStdString();
    m_data->channelBufferOutputDetails = std::move(bestOutputStreamInfos);
    m_data->deviceId = *deviceId;
    m_data->monitor = m_monitor;
    // The device's buffer is the margin's level, the engine's block the spec's (see setUpMonitor)
    if (const uint32_t bufferFrames = setUpMonitor(*deviceId, m_data->format.output);
        bufferFrames > 0 && bufferFrames != m_data->format.output.samplesPerChannel) {
        if (const std::optional<UInt32> actual = setBufferFrames(*deviceId, bufferFrames)) {
            std::scoped_lock lock { m_monitor->levelsMutex };
            m_monitor->bufferFrames = *actual;
        }
    }
    {
        std::scoped_lock lock { m_monitor->levelsMutex };
        const uint32_t largest = m_monitor->levelFrames.empty() ? 0 : m_monitor->levelFrames.back();
        m_data->outBuffer.resize(std::max<size_t>({ m_data->format.output.samplesPerChannel, largest, m_monitor->bufferFrames })
                                 * m_data->format.output.audioChannelCount);
    }

    auto result = AudioDeviceCreateIOProcID(*deviceId, coreAudioIOProc, m_data.get(),
                                            &m_data->procId);
    if (result != noErr) {
        m_data->clear();
        logError("Failed to create Audio Device IO Proc, err: ", result);
        return false;
    }

    result = AudioDeviceStart(*deviceId, m_data->procId);
    if (result != noErr) {
        AudioDeviceDestroyIOProcID(*deviceId, m_data->procId);
        m_data->clear();
        logError("Failed to start Audio Device, err: ", result);
        return false;
    }

    m_audioWorkGroup = createAudioWorkgroup(*deviceId);
    m_currentWorkgroupChanged.notify();

    if (activeSpec) {
        *activeSpec = m_data->format;
    }

    m_activeSpecChanged.send(m_data->format);

    LOGI() << "Connected to " << m_data->format.deviceId
           << " with bufferSize " << m_data->format.output.samplesPerChannel
           << ", sampleRate " << m_data->format.output.sampleRate;

    return true;
}

void OSXDirectAudioDriver::close()
{
    doClose();
}

void OSXDirectAudioDriver::doClose()
{
    if (!isOpened()) {
        return;
    }

    m_data->stopPending = true;
    // we spin while we wait for the callback to stop the device. That way we can be sure that data will no longer be used by the callback
    for (int i = 0; i < 100 && !m_data->stopped; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!m_data->stopped) {
        [[maybe_unused]] auto _leaked = m_data.release(); // we let it leak since it might still be accessed by the callback
        m_data = std::make_unique<Data>();
    } else {
        AudioDeviceDestroyIOProcID(m_data->deviceId, m_data->procId);
        m_data->clear();
    }
    m_audioWorkGroup = {};
    m_currentWorkgroupChanged.notify();
}

bool OSXDirectAudioDriver::isOpened() const
{
    return m_data->procId != nullptr;
}

IAudioOutputMonitor::Window OSXDirectAudioDriver::takeWindow()
{
    Monitor::Gathered g;
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
    w.clockStalls = g.clockStalls;
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

double OSXDirectAudioDriver::periodMs() const
{
    if (!isOpened()) {
        return 0.0;
    }
    std::scoped_lock lock { m_monitor->levelsMutex };
    return m_monitor->rate > 0.0 ? m_monitor->bufferFrames * 1000.0 / m_monitor->rate : 0.0;
}

std::vector<double> OSXDirectAudioDriver::marginLevelsMs() const
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

size_t OSXDirectAudioDriver::marginLevel() const
{
    std::scoped_lock lock { m_monitor->levelsMutex };
    const size_t count = m_monitor->levelFrames.size();
    return count ? std::min(m_monitor->requestedLevel.load(), count - 1) : 0;
}

void OSXDirectAudioDriver::setMarginLevel(size_t level)
{
    if (m_monitor->requestedLevel.exchange(level) == level || !isOpened()) {
        return;
    }
    uint32_t frames = 0;
    {
        std::scoped_lock lock { m_monitor->levelsMutex };
        frames = m_monitor->levelFramesFor(level);
    }
    if (frames == 0) {
        return;
    }
    // The IOProc's buffer (outBuffer) holds the largest level
    m_monitor->restartCounting = true;
    if (const std::optional<UInt32> actual = setBufferFrames(m_data->deviceId, frames)) {
        std::scoped_lock lock { m_monitor->levelsMutex };
        m_monitor->bufferFrames = *actual;
        LOGI() << "margin level " << level << ": the device's buffer is " << *actual << " frames";
    }
}

void OSXDirectAudioDriver::delayNextCallback(double ms)
{
    m_monitor->delayMs = ms;
}

const OSXDirectAudioDriver::Spec& OSXDirectAudioDriver::activeSpec() const
{
    return m_data->format;
}

async::Channel<OSXDirectAudioDriver::Spec> OSXDirectAudioDriver::activeSpecChanged() const
{
    return m_activeSpecChanged;
}

AudioDeviceList OSXDirectAudioDriver::availableOutputDevices() const
{
    std::lock_guard lock(m_devicesMutex);

    AudioDeviceList deviceList;
    deviceList.push_back({ DEFAULT_DEVICE_ID, muse::trc("audio", "System default") });

    for (auto& device : m_outputDevices) {
        AudioDevice deviceInfo;
        deviceInfo.id = QString::number(device.first).toStdString();
        deviceInfo.name = device.second;

        deviceList.push_back(deviceInfo);
    }

    return deviceList;
}

async::Notification OSXDirectAudioDriver::availableOutputDevicesChanged() const
{
    return m_availableOutputDevicesChanged;
}

void OSXDirectAudioDriver::updateDeviceMap()
{
    std::lock_guard lock(m_devicesMutex);

    UInt32 propertySize;
    OSStatus result;
    std::vector<AudioObjectID> audioObjects = {};
    m_outputDevices.clear();
    m_inputDevices.clear();

    AudioObjectPropertyAddress devicesPropertyAddress = {
        .mSelector = kAudioHardwarePropertyDevices,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster,
    };

    AudioObjectPropertyAddress namePropertyAddress = {
        .mSelector = kAudioDevicePropertyDeviceNameCFString,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster,
    };

    auto getStreamsCount
        = [](const AudioObjectID& id, const AudioObjectPropertyScope& scope, const std::string& deviceName) -> unsigned int {
        AudioObjectPropertyAddress propertyAddress = {
            .mSelector  = kAudioDevicePropertyStreamConfiguration,
            .mScope     = scope,
            .mElement   = kAudioObjectPropertyElementWildcard
        };
        UInt32 propertySize = 0;
        OSStatus result = AudioObjectGetPropertyDataSize(id, &propertyAddress, 0, NULL, &propertySize);
        if (result != noErr) {
            logError("Failed to get device's (" + deviceName + ") streams size, err: ", result);
            return 0;
        }

        auto freeBufferList = [](AudioBufferList* list) { free(list); };
        std::unique_ptr<AudioBufferList, decltype(freeBufferList)> bufferList(reinterpret_cast<AudioBufferList*>(malloc(propertySize)),
                                                                              freeBufferList);
        result = AudioObjectGetPropertyData(id, &propertyAddress, 0, NULL, &propertySize, bufferList.get());
        if (result != noErr) {
            logError("Failed to get device's (" + deviceName + ") streams, err: ", result);
            return 0;
        }

        return bufferList->mNumberBuffers;
    };

    result = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devicesPropertyAddress, 0, NULL, &propertySize);
    if (result != noErr) {
        logError("Failed to get devices count, err: ", result);
        return;
    }

    audioObjects.resize(propertySize / sizeof(OSXAudioDeviceID));
    result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &devicesPropertyAddress, 0, NULL, &propertySize, audioObjects.data());
    if (result != noErr) {
        logError("Failed to get devices list, err: ", result);
        return;
    }

    for (auto&& deviceId : audioObjects) {
        CFStringRef nameRef;
        propertySize = sizeof(nameRef);

        result = AudioObjectGetPropertyData(deviceId, &namePropertyAddress, 0, NULL, &propertySize, &nameRef);
        if (result != noErr) {
            logError("Failed to get device's name, err: ", result);
            continue;
        }

        NSString* nsString = (NSString*)nameRef;
        std::string deviceName = [nsString UTF8String];

        if (getStreamsCount(deviceId, kAudioObjectPropertyScopeOutput, deviceName) > 0) {
            m_outputDevices[deviceId] = deviceName;
        }

        if (getStreamsCount(deviceId, kAudioObjectPropertyScopeInput, deviceName) > 0) {
            m_inputDevices[deviceId] = deviceName;
        }

        CFRelease(nameRef);
    }
    m_availableOutputDevicesChanged.notify();
}

std::vector<samples_t> OSXDirectAudioDriver::availableOutputDeviceBufferSizes() const
{
    OSXAudioDeviceID osxDeviceId = this->osxDeviceId();
    AudioObjectPropertyAddress bufferFrameSizePropertyAddress = {
        .mSelector = kAudioDevicePropertyBufferFrameSizeRange,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMaster
    };

    AudioValueRange range = { 0, 0 };
    UInt32 dataSize = sizeof(AudioValueRange);
    OSStatus rangeResult = AudioObjectGetPropertyData(osxDeviceId, &bufferFrameSizePropertyAddress, 0, NULL, &dataSize, &range);
    if (rangeResult != noErr) {
        logError("Failed to get device " + m_data->format.deviceId + " bufferFrameSize, err: ", rangeResult);
        return {};
    }

    samples_t minimum = std::max(static_cast<samples_t>(range.mMinimum), MINIMUM_BUFFER_SIZE);
    samples_t maximum = std::min(static_cast<samples_t>(range.mMaximum), MAXIMUM_BUFFER_SIZE);

    std::vector<samples_t> result;
    for (samples_t bufferSize = maximum; bufferSize >= minimum;) {
        result.push_back(bufferSize);
        bufferSize /= 2;
    }

    std::sort(result.begin(), result.end());

    return result;
}

//TODO: replace hardcoded values with truth
std::vector<sample_rate_t> OSXDirectAudioDriver::availableOutputDeviceSampleRates() const
{
    return {
        44100,
        48000,
        88200,
        96000,
    };
}

static std::optional<OSXAudioDeviceID> getDefaultDeviceId(const std::function<void(std::string, OSStatus)>& logError)
{
    OSXAudioDeviceID osxDeviceId = kAudioObjectUnknown;
    UInt32 deviceIdSize = sizeof(osxDeviceId);

    AudioObjectPropertyAddress deviceNamePropertyAddress = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMaster
    };

    OSStatus result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &deviceNamePropertyAddress, 0, 0, &deviceIdSize, &osxDeviceId);
    if (result != noErr) {
        logError("Failed to get default device ID, err: ", result);
        return std::nullopt;
    }

    return osxDeviceId;
}

UInt32 OSXDirectAudioDriver::osxDeviceId() const
{
    AudioDeviceID deviceId = m_data->format.deviceId;
    if (deviceId == DEFAULT_DEVICE_ID) {
        auto defaultDeviceId = getDefaultDeviceId(&logError);
        if (!defaultDeviceId) {
            logError("Failed to get default device ID, err: ", noErr);
            return kAudioObjectUnknown;
        }
        return *defaultDeviceId;
    }

    return QString::fromStdString(deviceId).toInt();
}

void OSXDirectAudioDriver::logError(const std::string message, OSStatus error)
{
    if (error == noErr) {
        return;
    }

    char errorString[5];

    UInt32 errorBigEndian = CFSwapInt32HostToBig(error);
    errorString[0] = errorBigEndian & 0xFF;
    errorString[1] = (errorBigEndian >> 8) & 0xFF;
    errorString[2] = (errorBigEndian >> 16) & 0xFF;
    errorString[3] = (errorBigEndian >> 24) & 0xFF;
    errorString[4] = '\0';
    if (isprint(errorString[0]) && isprint(errorString[1]) && isprint(errorString[2]) && isprint(errorString[3])) {
        LOGE() << message << errorString << "(" << error << ")";
    } else {
        LOGE() << message << error;
    }
}

static OSStatus onDeviceListChanged(AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress* inAddresses,
                                    void* inClientData)
{
    UNUSED(inObjectID);
    UNUSED(inNumberAddresses);
    UNUSED(inAddresses);
    auto driver = reinterpret_cast<OSXDirectAudioDriver*>(inClientData);
    driver->updateDeviceMap();

    return noErr;
}

void OSXDirectAudioDriver::initDeviceMapListener()
{
    AudioObjectPropertyAddress propertyAddress;
    propertyAddress.mSelector = kAudioHardwarePropertyDevices;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    propertyAddress.mElement = kAudioObjectPropertyElementMaster;

    auto result = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &propertyAddress, &onDeviceListChanged, this);
    if (result != noErr) {
        logError("Failed to add devices list listener, err: ", result);
        return;
    }

    m_deviceMapListenerRegistered = true;
}

void OSXDirectAudioDriver::removeDeviceMapListener()
{
    if (!m_deviceMapListenerRegistered) {
        return;
    }

    AudioObjectPropertyAddress propertyAddress;
    propertyAddress.mSelector = kAudioHardwarePropertyDevices;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    propertyAddress.mElement = kAudioObjectPropertyElementMaster;

    auto result = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &propertyAddress, &onDeviceListChanged, this);
    if (result != noErr) {
        logError("Failed to remove devices list listener, err: ", result);
        return;
    }

    m_deviceMapListenerRegistered = false;
}

std::optional<int> muse::audio::OSXDirectAudioDriver::getAudioDeviceId(
    const AudioDeviceID& deviceId) const
{
    if (deviceId.empty() || deviceId == DEFAULT_DEVICE_ID) {
        return getDefaultDeviceId(&logError); //default device used
    }

    std::lock_guard lock(m_devicesMutex);

    uint deviceIdInt = QString::fromStdString(deviceId).toInt();
    auto index = std::find_if(m_outputDevices.begin(), m_outputDevices.end(), [&deviceIdInt](auto& d) {
        return d.first == deviceIdInt;
    });

    if (index == m_outputDevices.end()) {
        return std::nullopt;
    }
    return index->first;
}
