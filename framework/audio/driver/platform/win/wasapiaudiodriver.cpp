/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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
#include "wasapiaudiodriver.h"

#include <combaseapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

#include "global/defer.h"
#include "global/translation.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;

struct WasapiAudioDriver::DeviceListener : public IMMNotificationClient
{
    DeviceListener(WasapiAudioDriver* driver)
        : m_driver(driver) {}

    STDMETHOD_(ULONG, AddRef)() {
        return 1;
    }
    STDMETHOD_(ULONG, Release)() {
        return 1;
    }
    STDMETHOD(QueryInterface)(REFIID, void**) {
        return S_OK;
    }
    STDMETHOD(OnDeviceStateChanged)(LPCWSTR, DWORD) {
        return S_OK;
    }
    STDMETHOD(OnDeviceAdded)(LPCWSTR) {
        m_driver->updateAudioDeviceList();
        return S_OK;
    }
    STDMETHOD(OnDeviceRemoved)(LPCWSTR) {
        m_driver->updateAudioDeviceList();
        return S_OK;
    }
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow flow, ERole role, LPCWSTR) {
        if ((role != eConsole && role != eCommunications) || (flow != eRender)) {
            return S_OK;
        }

        m_driver->updateAudioDeviceList();

        return S_OK;
    }
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR, const PROPERTYKEY) {
        return S_OK;
    }

    WasapiAudioDriver* m_driver = nullptr;
};

struct WasapiAudioDriver::Data {
    IMMDeviceEnumerator* enumerator = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    HANDLE audioEvent = nullptr;
    IAudioClock* audioClock = nullptr;
    UINT64 clockFrequency = 0;

    //! NOTE The margin kept (see th_audioInitialize): at most so many frames queued after a write,
    //! one level per whole period. Written on the audio thread (opening),
    //! read there, and under the lock by the monitor's reader
    mutable std::mutex levelsMutex;
    std::vector<uint32_t> levelFrames;
    std::atomic<size_t> requestedLevel { std::numeric_limits<size_t>::max() }; // the last, until chosen
    std::atomic<double> periodMs { 0.0 };
    std::atomic<double> delayMs { 0.0 };

    //! NOTE What the monitor's windows are made of: gathered on the audio thread, handed over when
    //! the lock is free (the audio thread never waits for it)
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
    Gathered gathering; // audio thread
    std::mutex windowMutex;
    Gathered gathered; // under windowMutex
    double lastLatencyMs = 0.0; // under windowMutex

    //! NOTE Audio thread: when the device's passes are (each signals the event), on the
    //! performance counter's clock, in seconds; the stream's position at the last callback
    double rate = 0.0;
    uint32_t periodFrames = 0;
    double periodS = 0.0;
    double expectedEvent = 0.0; // 0: not known yet
    double lastWriteEvent = 0.0; // the event of the last callback that wrote
    double queuedAfterLastWrite = 0.0;
    uint64_t callbacksSinceStart = 0;
    UINT64 framesWritten = 0;
    double lastPlaying = -1.0;
    double lastPlayingTime = 0.0;

    void releaseClient()
    {
        if (audioClock) {
            audioClock->Release();
            audioClock = nullptr;
        }

        if (audioEvent) {
            CloseHandle(audioEvent);
            audioEvent = nullptr;
        }

        if (renderClient) {
            renderClient->Release();
            renderClient = nullptr;
        }

        if (audioClient) {
            audioClient->Release();
            audioClient = nullptr;
        }

        if (mixFormat) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }
    }

    void releaseAll()
    {
        releaseClient();
        if (enumerator) {
            enumerator->Release();
            enumerator = nullptr;
        }
    }
};

static REFERENCE_TIME to_reftime(int numSamples, double sampleRate)
{
    return (REFERENCE_TIME)((numSamples * 10000.0 * 1000.0 / sampleRate) + 0.5);
}

static samples_t to_samples(const REFERENCE_TIME& t, double sampleRate)
{
    return sampleRate * ((double)t) * 0.0000001;
}

static std::wstring to_wstring(const std::string& str)
{
    if (str.empty()) {
        return L"";
    }

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (wide_len == 0) {
        return L"";
    }

    std::wstring result(wide_len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], wide_len);

    return result;
}

static std::string to_string(LPWSTR wstr)
{
    if (!wstr || wstr[0] == L'\0') {
        return "";
    }

    int multi_len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (multi_len == 0) {
        return "";
    }

    std::string result(multi_len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], multi_len, nullptr, nullptr);

    return result;
}

static std::string get_deviceId(IMMDevice* device)
{
    LPWSTR devId;
    HRESULT hr = device->GetId(&devId);
    if (FAILED(hr)) {
        LOGE() << "failed get deviceId";
        return std::string();
    }
    std::string str = to_string(devId);
    CoTaskMemFree(devId);
    return str;
}

static std::string hrToString(HRESULT hr)
{
    if (SUCCEEDED(hr)) {
        return "SUCCEEDED";
    }

    switch (hr) {
    // Common WASAPI errors
    case AUDCLNT_E_NOT_INITIALIZED:          return "AUDCLNT_E_NOT_INITIALIZED - Audio client not initialized";
    case AUDCLNT_E_ALREADY_INITIALIZED:      return "AUDCLNT_E_ALREADY_INITIALIZED - Audio client already initialized";
    case AUDCLNT_E_WRONG_ENDPOINT_TYPE:      return "AUDCLNT_E_WRONG_ENDPOINT_TYPE - Wrong endpoint type";
    case AUDCLNT_E_DEVICE_INVALIDATED:       return "AUDCLNT_E_DEVICE_INVALIDATED - Device invalidated";
    case AUDCLNT_E_NOT_STOPPED:              return "AUDCLNT_E_NOT_STOPPED - Audio client not stopped";
    case AUDCLNT_E_BUFFER_TOO_LARGE:         return "AUDCLNT_E_BUFFER_TOO_LARGE - Buffer too large";
    case AUDCLNT_E_OUT_OF_ORDER:             return "AUDCLNT_E_OUT_OF_ORDER - Operation out of order";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:       return "AUDCLNT_E_UNSUPPORTED_FORMAT - Unsupported format";
    case AUDCLNT_E_INVALID_SIZE:             return "AUDCLNT_E_INVALID_SIZE - Invalid size";
    case AUDCLNT_E_DEVICE_IN_USE:            return "AUDCLNT_E_DEVICE_IN_USE - Device in use";
    case AUDCLNT_E_BUFFER_OPERATION_PENDING: return "AUDCLNT_E_BUFFER_OPERATION_PENDING - Buffer operation pending";
    case AUDCLNT_E_THREAD_NOT_REGISTERED:    return "AUDCLNT_E_THREAD_NOT_REGISTERED - Thread not registered";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED - Exclusive mode not allowed";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:   return "AUDCLNT_E_ENDPOINT_CREATE_FAILED - Endpoint create failed";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:      return "AUDCLNT_E_SERVICE_NOT_RUNNING - Audio service not running";
    case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED: return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED - Event handle not expected";
    case AUDCLNT_E_EXCLUSIVE_MODE_ONLY:      return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY - Exclusive mode only";
    case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL: return "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL - Buffer duration/period not equal";
    case AUDCLNT_E_EVENTHANDLE_NOT_SET:      return "AUDCLNT_E_EVENTHANDLE_NOT_SET - Event handle not set";
    case AUDCLNT_E_INCORRECT_BUFFER_SIZE:    return "AUDCLNT_E_INCORRECT_BUFFER_SIZE - Incorrect buffer size";
    case AUDCLNT_E_BUFFER_SIZE_ERROR:        return "AUDCLNT_E_BUFFER_SIZE_ERROR - Buffer size error";
    case AUDCLNT_E_CPUUSAGE_EXCEEDED:        return "AUDCLNT_E_CPUUSAGE_EXCEEDED - CPU usage exceeded";

    // Common COM errors
    case E_INVALIDARG:                       return "E_INVALIDARG - Invalid arguments";
    case E_OUTOFMEMORY:                      return "E_OUTOFMEMORY - Out of memory";
    case E_NOINTERFACE:                      return "E_NOINTERFACE - No interface";
    case E_POINTER:                          return "E_POINTER - Invalid pointer";
    case E_ACCESSDENIED:                     return "E_ACCESSDENIED - Access denied";
    case E_FAIL:                             return "E_FAIL - Unspecified error";

    default:
        return "Unknown error: " + std::to_string(hr);
    }
}

WasapiAudioDriver::WasapiAudioDriver()
{
    m_data = std::make_shared<Data>();
    m_deviceListener = std::make_shared<DeviceListener>(this);
}

WasapiAudioDriver::~WasapiAudioDriver()
{
    doClose();

    if (m_data->enumerator) {
        m_data->enumerator->UnregisterEndpointNotificationCallback(m_deviceListener.get());
    }

    m_data->releaseAll();

    CoUninitialize();
}

std::string WasapiAudioDriver::name() const
{
    return "WASAPI";
}

void WasapiAudioDriver::init()
{
    LOGI() << "begin driver init";
    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        LOGE() << "failed CoInitialize, error: " << hrToString(hr);
    }

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&m_data->enumerator
        );

    if (FAILED(hr)) {
        LOGE() << "failed create DeviceEnumerator, error: " << hrToString(hr);
        return;
    }

    hr = m_data->enumerator->RegisterEndpointNotificationCallback(m_deviceListener.get());
    if (FAILED(hr)) {
        LOGE() << "failed RegisterEndpointNotificationCallback, error: " << hrToString(hr);
    }

    updateAudioDeviceList();

    LOGI() << "success driver init";
}

static std::pair<std::vector<AudioDevice>, std::string /*defaultId*/> audioDevices(IMMDeviceEnumerator* enumerator)
{
    IF_ASSERT_FAILED(enumerator) {
        return {};
    }

    IMMDeviceCollection* pCollection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        LOGE() << "failed get EnumAudioEndpoints, error: " << hrToString(hr);
        return {};
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    std::pair<std::vector<AudioDevice>, std::string> devices;

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = nullptr;
        hr = pCollection->Item(i, &pDevice);
        if (SUCCEEDED(hr)) {
            AudioDevice deviceInfo;
            deviceInfo.id = get_deviceId(pDevice);

            IPropertyStore* pProps = nullptr;
            hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
            if (SUCCEEDED(hr)) {
                PROPVARIANT varName;
                PropVariantInit(&varName);

                hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
                    deviceInfo.name = to_string(varName.pwszVal);
                }

                PropVariantClear(&varName);
                pProps->Release();
            }

            devices.first.push_back(deviceInfo);
            pDevice->Release();
        }
    }

    pCollection->Release();

    IMMDevice* defaultDevice = nullptr;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    if (defaultDevice) {
        devices.second = get_deviceId(defaultDevice);
        defaultDevice->Release();
    }

    return devices;
}

void WasapiAudioDriver::updateAudioDeviceList()
{
    TRACEFUNC;

    IF_ASSERT_FAILED(m_data->enumerator) {
        return;
    }

    auto devices = audioDevices(m_data->enumerator);
    m_deviceList.clear();
    m_deviceList.push_back({ DEFAULT_DEVICE_ID, muse::trc("audio", "System default") });
    m_deviceList.insert(m_deviceList.end(), devices.first.begin(), devices.first.end());
    m_defaultDeviceId = devices.second;
    m_deviceListChanged.notify();
}

static IAudioClient* audioClientForDevice(IMMDeviceEnumerator* enumerator, const std::string& deviceId)
{
    std::wstring wdeviceId = to_wstring(deviceId);
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDevice(wdeviceId.c_str(), &device);
    if (FAILED(hr)) {
        LOGE() << "failed get device with id: " << deviceId << ", error: " << hrToString(hr);
        return nullptr;
    }

    DEFER {
        device->Release();
    };

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        (void**)&audioClient
        );

    if (FAILED(hr)) {
        LOGE() << "failed get audioClient, error: " << hrToString(hr);
        return nullptr;
    }

    return audioClient;
}

AudioDeviceID WasapiAudioDriver::defaultDevice() const
{
    return m_defaultDeviceId;
}

bool WasapiAudioDriver::open(const Spec& spec, Spec* activeSpec)
{
    TRACEFUNC;

    IF_ASSERT_FAILED(!spec.deviceId.empty()) {
        return false;
    }

    IF_ASSERT_FAILED(m_data->enumerator) {
        return false;
    }

    AudioDeviceID deviceId = spec.deviceId;
    if (deviceId == DEFAULT_DEVICE_ID) {
        deviceId = defaultDevice();
    }

    LOGI() << "try open driver, device: " << deviceId;

    m_data->audioClient = audioClientForDevice(m_data->enumerator, deviceId);
    if (!m_data->audioClient) {
        LOGE() << "failed get audio client for device: " << deviceId;
        return false;
    }

    HRESULT hr = m_data->audioClient->GetMixFormat(&m_data->mixFormat);
    if (FAILED(hr)) {
        LOGE() << "failed get mixFormat, error: " << hrToString(hr);
        return false;
    }

    m_activeSpec = spec;
    m_activeSpec.output.sampleRate = m_data->mixFormat->nSamplesPerSec;

    if (activeSpec) {
        *activeSpec = m_activeSpec;
    }

    m_audioThread = std::thread(&WasapiAudioDriver::th_audioThread, this);

    return true;
}

void WasapiAudioDriver::th_audioThread()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOGE() << "failed CoInitializeEx, error: " << hrToString(hr);
        return;
    }

    //! NOTE Pro Audio MMCSS class: the audio thread is scheduled ahead of the others,
    //! as Windows' own audio engine is; without it, a busy system makes it late (underruns)
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (!mmcssTask) {
        LOGW() << "failed AvSetMmThreadCharacteristics, error: " << GetLastError();
    }

    m_opened = th_audioInitialize();
    m_activeSpecChanged.send(m_activeSpec);

    while (m_opened) {
        DWORD waitResult = WaitForSingleObject(m_data->audioEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            th_processAudioData();
        } else if (waitResult == WAIT_TIMEOUT) {
            continue;
        } else {
            // error
            break;
        }
    }

    if (mmcssTask) {
        AvRevertMmThreadCharacteristics(mmcssTask);
    }

    CoUninitialize();
}

bool WasapiAudioDriver::th_audioInitialize()
{
    TRACEFUNC;

    const UINT32 blockFrames = static_cast<UINT32>(m_activeSpec.output.samplesPerChannel);

    //! NOTE The device is called back once per period of Windows' audio engine (10 ms by default):
    //! the engine's block is rendered on the callbacks that bring enough frames for it. Where the
    //! engine's block is shorter than that, ask for a shorter period (low-latency shared mode,
    //! IAudioClient3), the shortest that still holds the block: it is then rendered on every callback
    HRESULT hr = E_NOTIMPL;
    UINT32 streamPeriod = 0;
    IAudioClient3* audioClient3 = nullptr;
    if (SUCCEEDED(m_data->audioClient->QueryInterface(__uuidof(IAudioClient3), (void**)&audioClient3))) {
        UINT32 defaultPeriod = 0;
        UINT32 fundamentalPeriod = 0;
        UINT32 minPeriod = 0;
        UINT32 maxPeriod = 0;
        HRESULT periodHr = audioClient3->GetSharedModeEnginePeriod(m_data->mixFormat, &defaultPeriod, &fundamentalPeriod,
                                                                    &minPeriod, &maxPeriod);
        if (SUCCEEDED(periodHr)) {
            LOGI() << "shared mode engine periods (frames): default " << defaultPeriod << ", fundamental " << fundamentalPeriod
                   << ", min " << minPeriod << ", max " << maxPeriod;
        } else {
            LOGW() << "failed GetSharedModeEnginePeriod, error: " << hrToString(periodHr);
        }

        if (SUCCEEDED(periodHr) && fundamentalPeriod > 0 && blockFrames < defaultPeriod && minPeriod < defaultPeriod) {
            UINT32 period = minPeriod;
            if (blockFrames > minPeriod) {
                period += (blockFrames - minPeriod + fundamentalPeriod - 1) / fundamentalPeriod * fundamentalPeriod;
            }
            period = std::min(period, maxPeriod);

            hr = audioClient3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, m_data->mixFormat, nullptr);
            if (SUCCEEDED(hr)) {
                streamPeriod = period;
                LOGI() << "low-latency shared mode, period: " << period << " frames";
            } else {
                //! NOTE e.g. AUDCLNT_E_ENGINE_PERIODICITY_LOCKED: another stream holds another period
                LOGW() << "failed InitializeSharedAudioStream at " << period << " frames, error: " << hrToString(hr);
            }
        }
        audioClient3->Release();
    }

    if (FAILED(hr)) {
        //! NOTE Room for three periods (the margin's levels, see below), however short the block:
        //! the buffer's size adds no latency, what is kept queued in it does
        REFERENCE_TIME devicePeriod = 0;
        m_data->audioClient->GetDevicePeriod(&devicePeriod, nullptr);
        REFERENCE_TIME bufferDuration = std::max(to_reftime(m_activeSpec.output.samplesPerChannel,
                                                            m_activeSpec.output.sampleRate),
                                                 3 * devicePeriod);

        hr = m_data->audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            bufferDuration,
            0,
            m_data->mixFormat,
            nullptr
            );
    }

    if (FAILED(hr)) {
        LOGE() << "failed audioClient->Initialize, error: " << hrToString(hr);
        return false;
    }

    m_data->audioClient->GetBufferSize(&m_bufferFrames);
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    m_data->audioClient->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    m_defaultPeriod = to_samples(defaultPeriod, m_activeSpec.output.sampleRate);
    m_minimumPeriod = to_samples(minimumPeriod, m_activeSpec.output.sampleRate);

    LOGI() << "initialized, block: " << blockFrames << ", buffer: " << m_bufferFrames << ", device period: default "
           << m_defaultPeriod << ", minimum " << m_minimumPeriod << " (frames)";

    hr = m_data->audioClient->GetService(
        __uuidof(IAudioRenderClient),
        (void**)&m_data->renderClient
        );

    if (FAILED(hr)) {
        LOGE() << "failed get renderClient, error: " << hrToString(hr);
        return false;
    }

    m_data->framesWritten = 0;
    m_data->expectedEvent = 0.0;
    m_data->lastWriteEvent = 0.0;
    m_data->queuedAfterLastWrite = 0.0;
    m_data->callbacksSinceStart = 0;
    m_data->lastPlaying = -1.0;
    hr = m_data->audioClient->GetService(__uuidof(IAudioClock), (void**)&m_data->audioClock);
    if (SUCCEEDED(hr)) {
        hr = m_data->audioClock->GetFrequency(&m_data->clockFrequency);
    }
    if (FAILED(hr) || m_data->clockFrequency == 0) {
        LOGW() << "no audio clock, the output latency isn't measured, error: " << hrToString(hr);
    }

    //! NOTE Shared mode gives a buffer of about two periods or more, whatever is asked (970 frames for
    //! 441-frame periods). Filled whole, what is written at each event waits behind what is left of
    //! the last write (529 frames there, 12 ms). Queued only up to one period, it is played from the
    //! next pass of Windows' audio engine (measured: 31.7 -> 19.7 ms from a callback to its first frame
    //! played), but must be written before that pass, one period after the event, rather than two.
    //! So the margin is chosen (IAudioOutputMonitor): one, two or three whole periods queued, each a
    //! pass more to write in (half a period more gives no more time: a pass takes a whole one).
    //! Never less than the engine's block, which then still renders on every callback it can.
    //! (The period rounded: one frame short would drain the queue by a frame at each pass.)
    const UINT32 period = streamPeriod ? streamPeriod
                          : static_cast<UINT32>(std::llround(defaultPeriod * 1e-7 * m_activeSpec.output.sampleRate));
    {
        std::vector<uint32_t> levels;
        for (UINT32 frames = period; period > 0 && frames <= m_bufferFrames && levels.size() < 3; frames += period) {
            if (frames >= blockFrames) {
                levels.push_back(frames);
            }
        }
        if (levels.empty()) {
            levels.push_back(m_bufferFrames);
        }
        std::string listed;
        for (const uint32_t frames : levels) {
            listed += (listed.empty() ? "" : ", ") + std::to_string(frames);
        }
        LOGI() << "margin levels (frames queued at most): " << listed << ", buffer: " << m_bufferFrames;
        std::scoped_lock lock { m_data->levelsMutex };
        m_data->levelFrames = std::move(levels);
    }
    m_data->rate = m_activeSpec.output.sampleRate;
    m_data->periodFrames = period;
    m_data->periodS = period / m_data->rate;
    m_data->periodMs = period * 1000.0 / m_data->rate;

    m_data->audioEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_data->audioEvent) {
        LOGE() << "failed create audioEvent";
        return false;
    }

    hr = m_data->audioClient->SetEventHandle(m_data->audioEvent);
    if (FAILED(hr)) {
        LOGE() << "failed set audioEvent, error: " << hrToString(hr);
        return false;
    }

    hr = m_data->audioClient->Start();
    if (FAILED(hr)) {
        LOGE() << "failed audioClient->Start(), error: " << hrToString(hr);
        return false;
    }

    return true;
}

static double th_now()
{
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / frequency;
}

void WasapiAudioDriver::th_processAudioData()
{
    Data& d = *m_data;

    // Testing the detection: as if woken late
    if (const double delayMs = d.delayMs.exchange(0.0); delayMs > 0.0) {
        Sleep(static_cast<DWORD>(delayMs + 0.5));
    }

    const double start = th_now();

    // Get padding in existing buffer
    UINT32 paddingFrames = 0;
    auto hr = d.audioClient->GetCurrentPadding(&paddingFrames);
    if (FAILED(hr)) {
        LOGE() << "Failed to get current padding: " << hrToString(hr);
        return;
    }

    // The pass whose event this is. Events come one period apart; one that fires while the last
    // is still unanswered is lost (the event resets once); the wait returns after the event,
    // never before, so one sooner than expected moves the expected ones earlier
    double event = start;
    double latenessS = 0.0;
    if (d.expectedEvent > 0.0 && d.periodS > 0.0) {
        latenessS = start - d.expectedEvent;
        if (latenessS < 0.0 || latenessS > 10 * d.periodS) {
            latenessS = 0.0; // re-anchored
        } else {
            event = d.expectedEvent;
        }
    }
    const double missedPasses = d.periodS > 0.0 ? std::floor(latenessS / d.periodS) : 0.0;
    d.expectedEvent = event + (missedPasses + 1.0) * d.periodS;

    // GetCurrentPadding represents the number of queued frames
    // so we can subtract that from the overall number of frames we have
    uint32_t framesAvailable = m_bufferFrames - paddingFrames;
    {
        const std::vector<uint32_t>& levels = d.levelFrames; // only written on this thread
        if (!levels.empty()) {
            const uint32_t maxQueued = levels[std::min(d.requestedLevel.load(), levels.size() - 1)];
            framesAvailable = paddingFrames < maxQueued ? std::min(framesAvailable, maxQueued - paddingFrames) : 0;
        }
    }

    // Only continue if we have buffer to write data
    if (framesAvailable == 0) {
        return;
    }

    uint32_t clockStalls = 0;
    const double latencyMs = th_measureClock(start, clockStalls);

    const uint32_t actualFramesToRead = framesAvailable;

    // WASAPI: "nBlockAlign must be equal to the product of nChannels and wBitsPerSample divided by 8 (bits per byte)"
    const uint32_t clientFrameSize = d.mixFormat->nBlockAlign;

    // MuseScore assumes only 2 audio channels (same calculation as above to determine frame size)
    const uint32_t muFrameSize = 2 * d.mixFormat->wBitsPerSample / 8;

    uint8_t* data = nullptr;
    hr = d.renderClient->GetBuffer(actualFramesToRead, &data);
    if (FAILED(hr)) {
        LOGE() << "Failed to get buffer: " << hrToString(hr);
        return;
    }

    const float* written = nullptr;
    size_t stride = 2;
    if (actualFramesToRead > 0) {
        // Based on the previous calculations, the only way that clientFrameSize will be larger than muFrameSize is
        // if the client specifies more than 2 channels. MuseScore doesn't support this (yet), so we use a workaround
        // where the missing channels are padded with zeroes...
        if (clientFrameSize > muFrameSize) {
            const size_t surroundBufferDesiredSize = actualFramesToRead * muFrameSize;
            if (m_surroundAudioBuffer.size() < surroundBufferDesiredSize) {
                m_surroundAudioBuffer.resize(surroundBufferDesiredSize, 0);
            }

            m_activeSpec.callback(m_surroundAudioBuffer.data(),
                                  (int)surroundBufferDesiredSize);

            for (uint32_t i = 0; i < actualFramesToRead; ++i) {
                uint8_t* frameStartPos = data + i * clientFrameSize;
                std::memcpy(frameStartPos, m_surroundAudioBuffer.data() + i * muFrameSize, muFrameSize);
                std::memset(frameStartPos + muFrameSize, 0, clientFrameSize - muFrameSize);
            }
            written = reinterpret_cast<const float*>(m_surroundAudioBuffer.data());
        } else {
            m_activeSpec.callback(data, actualFramesToRead * clientFrameSize);
            written = reinterpret_cast<const float*>(data);
        }
    }

    // The loudest sample (MuseScore writes stereo floats)
    float peak = 0.0f;
    if (written && d.mixFormat->wBitsPerSample == 32) {
        for (uint32_t i = 0; i < actualFramesToRead; ++i) {
            peak = std::max({ peak, std::abs(written[i * stride]), std::abs(written[i * stride + 1]) });
        }
    }

    d.renderClient->ReleaseBuffer(actualFramesToRead, 0);
    d.framesWritten += actualFramesToRead;

    const double end = th_now();

    // Startup isn't counted: the first callbacks come irregularly, before anything sounds
    if (++d.callbacksSinceStart <= 20 || d.periodS <= 0.0) {
        d.lastWriteEvent = event;
        d.queuedAfterLastWrite = static_cast<double>(paddingFrames) + actualFramesToRead;
        return;
    }

    // The frames were needed by the pass after those that what was queued at the event still
    // covered (whole periods: a pass takes one). What was queued then: what the last write left,
    // less a period for each pass since its event (not the padding now: once late, the queue may
    // have run dry, and the padding doesn't tell by how much)
    const double passesSinceLastWrite = d.lastWriteEvent > 0.0
                                        ? std::max(1.0, std::round((event - d.lastWriteEvent) / d.periodS)) : 1.0;
    const double queuedAtEvent = std::max(0.0, d.queuedAfterLastWrite - passesSinceLastWrite * d.periodFrames);
    const double deadline = event + (1.0 + std::floor(queuedAtEvent / d.periodFrames)) * d.periodS;
    const double headroomMs = (deadline - end) * 1000.0;
    d.lastWriteEvent = event;
    d.queuedAfterLastWrite = static_cast<double>(paddingFrames) + actualFramesToRead;

    Data::Gathered& g = d.gathering;
    ++g.callbacks;
    g.clockStalls += clockStalls;
    g.headroomSumMs += headroomMs;
    g.headroomMinMs = std::min(g.headroomMinMs, headroomMs);
    if (headroomMs < 0.0) {
        ++g.underruns;
        LOGW() << "underrun: written " << -headroomMs << " ms after the device needed it (woken "
               << latenessS * 1000.0 << " ms after the event, " << paddingFrames << " frames queued)";
    }
    const double load = (end - start) / d.periodS * 100.0;
    g.loadSum += load;
    g.loadMax = std::max(g.loadMax, load);
    if (latencyMs > 0.0) {
        g.latencySumMs += latencyMs;
        ++g.latencyCount;
    }
    g.peak = std::max(g.peak, peak);

    if (d.windowMutex.try_lock()) {
        d.gathered.add(g);
        if (g.latencyCount > 0) {
            d.lastLatencyMs = g.latencySumMs / g.latencyCount;
        }
        d.windowMutex.unlock();
        g = {};
    }
}

double WasapiAudioDriver::th_measureClock(double now, uint32_t& clockStalls)
{
    Data& d = *m_data;
    if (!d.audioClock || d.clockFrequency == 0) {
        return 0.0;
    }

    UINT64 position = 0;
    UINT64 positionTime = 0; // 100 ns units, on the performance counter's clock
    if (FAILED(d.audioClock->GetPosition(&position, &positionTime))) {
        return 0.0;
    }

    // The frame playing now
    const double playing = static_cast<double>(position) / d.clockFrequency * d.rate + (now - positionTime * 1e-7) * d.rate;

    // Silence played in place of the stream: the position stops while time goes on
    if (d.lastPlaying >= 0.0 && d.callbacksSinceStart > 20) {
        const double behindFrames = (now - d.lastPlayingTime) * d.rate - (playing - d.lastPlaying);
        if (behindFrames > d.periodFrames / 2.0) {
            ++clockStalls;
            LOGW() << "the device's position stalled for " << behindFrames / d.rate * 1000.0 << " ms";
        }
    }
    d.lastPlaying = playing;
    d.lastPlayingTime = now;

    // From now to the first frame of the block about to be written being played
    return (static_cast<double>(d.framesWritten) - playing) / d.rate * 1000.0;
}

IAudioOutputMonitor::Window WasapiAudioDriver::takeWindow()
{
    Data::Gathered g;
    double latencyMs = 0.0;
    {
        std::scoped_lock lock { m_data->windowMutex };
        g = m_data->gathered;
        m_data->gathered = {};
        latencyMs = m_data->lastLatencyMs;
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

double WasapiAudioDriver::periodMs() const
{
    return m_opened ? m_data->periodMs.load() : 0.0;
}

std::vector<double> WasapiAudioDriver::marginLevelsMs() const
{
    std::vector<double> levels;
    if (!m_opened) {
        return levels;
    }
    std::scoped_lock lock { m_data->levelsMutex };
    for (const uint32_t frames : m_data->levelFrames) {
        levels.push_back(frames * 1000.0 / m_activeSpec.output.sampleRate);
    }
    return levels;
}

size_t WasapiAudioDriver::marginLevel() const
{
    std::scoped_lock lock { m_data->levelsMutex };
    const size_t count = m_data->levelFrames.size();
    return count ? std::min(m_data->requestedLevel.load(), count - 1) : 0;
}

void WasapiAudioDriver::setMarginLevel(size_t level)
{
    m_data->requestedLevel = level;
}

void WasapiAudioDriver::delayNextCallback(double ms)
{
    m_data->delayMs = ms;
}

void WasapiAudioDriver::close()
{
    doClose();
}

void WasapiAudioDriver::doClose()
{
    m_opened = false;
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }

    m_data->releaseClient();
}

bool WasapiAudioDriver::isOpened() const
{
    return m_opened;
}

const IAudioDriver::Spec& WasapiAudioDriver::activeSpec() const
{
    return m_activeSpec;
}

async::Channel<IAudioDriver::Spec> WasapiAudioDriver::activeSpecChanged() const
{
    return m_activeSpecChanged;
}

AudioDeviceList WasapiAudioDriver::availableOutputDevices() const
{
    return m_deviceList;
}

async::Notification WasapiAudioDriver::availableOutputDevicesChanged() const
{
    return m_deviceListChanged;
}

std::vector<samples_t> WasapiAudioDriver::availableOutputDeviceBufferSizes() const
{
    std::vector<samples_t> result;

    samples_t n = MAXIMUM_BUFFER_SIZE;
    samples_t min = std::max(m_minimumPeriod, MINIMUM_BUFFER_SIZE);

    while (n >= min) {
        result.push_back(n);
        n /= 2;
    }

    return result;
}

std::vector<sample_rate_t> WasapiAudioDriver::availableOutputDeviceSampleRates() const
{
    return {
        44100,
        48000,
        88200,
        96000,
    };
}
