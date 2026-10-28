#include "audio/WasapiAudioCapture.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <objbase.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <timeapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

namespace LiteCapture::Audio {

WasapiAudioCapture::WasapiAudioCapture() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiAudioCapture::~WasapiAudioCapture() {
    Stop();
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

std::vector<AudioDeviceInfo> WasapiAudioCapture::EnumerateEndpoints(EDataFlow dataFlow) {
    std::vector<AudioDeviceInfo> devices;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    Core::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return devices;
    }

    std::wstring defaultId;
    Core::ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &defaultDevice))) {
        LPWSTR idStr = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&idStr)) && idStr) {
            defaultId = idStr;
            CoTaskMemFree(idStr);
        }
    }

    // Default option first
    std::wstring defaultTitle = (dataFlow == eRender) ?
        L"Default Playback Device (Speakers / Headphones)" :
        L"Default Microphone (System Default)";
    devices.push_back({ L"", defaultTitle, true });

    Core::ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            Core::ComPtr<IMMDevice> dev;
            if (SUCCEEDED(collection->Item(i, &dev)) && dev) {
                LPWSTR idStr = nullptr;
                if (SUCCEEDED(dev->GetId(&idStr)) && idStr) {
                    std::wstring devId = idStr;
                    CoTaskMemFree(idStr);

                    std::wstring friendlyName = devId;
                    Core::ComPtr<IPropertyStore> props;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                            if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                                friendlyName = varName.pwszVal;
                            }
                        }
                        PropVariantClear(&varName);
                    }

                    bool isDef = (!defaultId.empty() && devId == defaultId);
                    if (isDef) {
                        friendlyName += L" (Default)";
                    }
                    devices.push_back({ devId, friendlyName, isDef });
                }
            }
        }
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return devices;
}

Core::Status WasapiAudioCapture::Initialize(const AudioConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    Stop();
    m_config = config;
    m_systemVolume.store(config.systemVolume);
    m_micVolume.store(config.micVolume);

    if (!m_config.IsAudioEnabled()) {
        LC_LOG_INFO(L"Audio capture is disabled in configuration.");
        return Core::Status::Ok();
    }

    // 1. Initialize System Loopback Audio Endpoint (if enabled)
    if (m_config.captureSystemAudio) {
        auto status = SetupEndpoint(eRender, m_config.systemDeviceId, AUDCLNT_STREAMFLAGS_LOOPBACK, m_systemEndpoint);
        if (status.IsError()) {
            LC_LOG_WARN(std::format(L"Failed to initialize System Audio Loopback: {}. Continuing without system audio.", status.Message()));
        } else {
            LC_LOG_INFO(std::format(L"System Audio Loopback initialized: {} Hz, {} channels, {} bit",
                m_systemEndpoint.sampleRate, m_systemEndpoint.channels, m_systemEndpoint.bitsPerSample));
        }
    }

    // 2. Initialize Microphone Capture Endpoint (if enabled)
    if (m_config.captureMicrophone) {
        auto status = SetupEndpoint(eCapture, m_config.micDeviceId, 0, m_micEndpoint);
        if (status.IsError()) {
            LC_LOG_WARN(std::format(L"Failed to initialize Microphone: {}. Continuing without microphone.", status.Message()));
        } else {
            LC_LOG_INFO(std::format(L"Microphone initialized: {} Hz, {} channels, {} bit",
                m_micEndpoint.sampleRate, m_micEndpoint.channels, m_micEndpoint.bitsPerSample));
        }
    }

    if (!m_systemEndpoint.isInitialized && !m_micEndpoint.isInitialized) {
        LC_LOG_WARN(L"No audio capture endpoints available. Recording will proceed without audio.");
    }

    return Core::Status::Ok();
}

Core::Status WasapiAudioCapture::SetupEndpoint(
    EDataFlow dataFlow,
    const std::wstring& preferredDeviceId,
    DWORD streamFlags,
    AudioEndpoint& endpoint
) {
    ReleaseEndpoint(endpoint);

    Core::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create MMDeviceEnumerator");
        return Core::Status::FromHResult(hr, L"MMDeviceEnumerator");
    }

    bool activated = false;
    // 1. Try preferred device if specified
    if (!preferredDeviceId.empty()) {
        hr = enumerator->GetDevice(preferredDeviceId.c_str(), &endpoint.device);
        if (SUCCEEDED(hr) && endpoint.device) {
            hr = endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &endpoint.client);
            if (SUCCEEDED(hr) && endpoint.client) {
                activated = true;
            } else {
                LC_LOG_WARN(std::format(L"Preferred device '{}' failed to activate (hr=0x{:08X}), falling back to default.", preferredDeviceId, static_cast<uint32_t>(hr)));
                endpoint.client.Reset();
                endpoint.device.Reset();
            }
        }
    }

    // 2. Fall back to system default endpoint
    if (!activated) {
        hr = enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &endpoint.device);
        if (FAILED(hr)) {
            return Core::Status::FromHResult(hr, L"GetDefaultAudioEndpoint");
        }
        hr = endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &endpoint.client);
        if (FAILED(hr)) {
            return Core::Status::FromHResult(hr, L"Activate IAudioClient on default device");
        }
    }

    hr = endpoint.client->GetMixFormat(&endpoint.waveFormat);
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"GetMixFormat");
    }

    WAVEFORMATEX* wfx = endpoint.waveFormat;
    WAVEFORMATEXTENSIBLE* wfext = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ?
        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx) : nullptr;

    endpoint.isFloat = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (wfext && IsEqualGUID(wfext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    endpoint.channels = wfx->nChannels;
    endpoint.sampleRate = wfx->nSamplesPerSec;
    endpoint.bitsPerSample = wfx->wBitsPerSample;

    REFERENCE_TIME hnsBufferDuration = 10000000; // 1 second buffer
    DWORD initFlags = streamFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    hr = endpoint.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        initFlags,
        hnsBufferDuration,
        0,
        wfx,
        nullptr
    );
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"IAudioClient::Initialize failed");
        return Core::Status::FromHResult(hr, L"IAudioClient::Initialize");
    }

    endpoint.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!endpoint.eventHandle) {
        return Core::Status::Error(Core::StatusCode::InternalError, L"Failed to create audio event handle");
    }

    hr = endpoint.client->SetEventHandle(endpoint.eventHandle);
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"SetEventHandle");
    }

    hr = endpoint.client->GetService(IID_PPV_ARGS(&endpoint.captureClient));
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"GetService IAudioCaptureClient");
    }

    endpoint.isInitialized = true;
    return Core::Status::Ok();
}

void WasapiAudioCapture::ReleaseEndpoint(AudioEndpoint& endpoint) {
    if (endpoint.eventHandle) {
        CloseHandle(endpoint.eventHandle);
        endpoint.eventHandle = nullptr;
    }
    if (endpoint.waveFormat) {
        CoTaskMemFree(endpoint.waveFormat);
        endpoint.waveFormat = nullptr;
    }
    endpoint.captureClient.Reset();
    endpoint.client.Reset();
    endpoint.device.Reset();
    endpoint.isInitialized = false;
}

Core::Status WasapiAudioCapture::Start(AudioFrameCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_isCapturing.load()) {
        return Core::Status::Ok();
    }

    if (!m_systemEndpoint.isInitialized && !m_micEndpoint.isInitialized) {
        return Core::Status::Ok();
    }

    m_callback = std::move(callback);
    m_stopRequested.store(false);
    ResetEvent(m_stopEvent);

    m_systemFramesCaptured = 0;
    m_micFramesCaptured = 0;
    m_totalSamplesMixed = 0;

    m_captureThread = std::thread(&WasapiAudioCapture::CaptureThreadProc, this);
    m_isCapturing.store(true);

    LC_LOG_INFO(L"WASAPI Audio Capture thread started.");
    return Core::Status::Ok();
}

void WasapiAudioCapture::Stop() {
    if (!m_isCapturing.load()) {
        return;
    }

    m_stopRequested.store(true);
    if (m_stopEvent) {
        SetEvent(m_stopEvent);
    }

    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    m_isCapturing.store(false);
    LC_LOG_INFO(std::format(L"WASAPI Audio Capture stopped. SystemFrames={}, MicFrames={}, TotalMixed={}",
        m_systemFramesCaptured.load(), m_micFramesCaptured.load(), m_totalSamplesMixed.load()));
}

void WasapiAudioCapture::ConvertToCanonicalFloatStereo(
    const BYTE* srcData,
    uint32_t numFrames,
    const AudioEndpoint& endpoint,
    AudioRingBuffer& outRing,
    bool isSilent
) {
    if (numFrames == 0) return;

    const uint32_t targetRate = 48000;

    // Fast-path 1: Native 48kHz 32-bit float stereo (standard Windows WASAPI loopback/mic)
    if (endpoint.sampleRate == targetRate && endpoint.channels == 2 && endpoint.isFloat && endpoint.bitsPerSample == 32) {
        if (isSilent || !srcData) {
            static const float zeros[960] = {0.0f};
            uint32_t remaining = numFrames;
            while (remaining > 0) {
                uint32_t chunk = (std::min)(remaining, 480u);
                outRing.Push(zeros, chunk * 2);
                remaining -= chunk;
            }
        } else {
            outRing.Push(reinterpret_cast<const float*>(srcData), numFrames * 2);
        }
        return;
    }

    // Fast-path 2: Native 48kHz 16-bit integer PCM stereo
    if (endpoint.sampleRate == targetRate && endpoint.channels == 2 && endpoint.bitsPerSample == 16) {
        if (isSilent || !srcData) {
            static const float zeros[960] = {0.0f};
            uint32_t remaining = numFrames;
            while (remaining > 0) {
                uint32_t chunk = (std::min)(remaining, 480u);
                outRing.Push(zeros, chunk * 2);
                remaining -= chunk;
            }
        } else {
            const int16_t* pcm = reinterpret_cast<const int16_t*>(srcData);
            float temp[960];
            uint32_t remaining = numFrames;
            size_t offset = 0;
            while (remaining > 0) {
                uint32_t chunk = (std::min)(remaining, 480u);
                for (uint32_t i = 0; i < chunk * 2; ++i) {
                    temp[i] = pcm[offset + i] * (1.0f / 32768.0f);
                }
                outRing.Push(temp, chunk * 2);
                offset += chunk * 2;
                remaining -= chunk;
            }
        }
        return;
    }

    // General fallback: arbitrary sample rates, channel configurations, and bit depths
    double resampleRatio = static_cast<double>(endpoint.sampleRate) / static_cast<double>(targetRate);
    uint32_t outFrames = (endpoint.sampleRate == targetRate) ?
        numFrames : static_cast<uint32_t>(std::round(numFrames / resampleRatio));

    if (isSilent || !srcData) {
        static const float zeros[960] = {0.0f};
        uint32_t remaining = outFrames;
        while (remaining > 0) {
            uint32_t chunk = (std::min)(remaining, 480u);
            outRing.Push(zeros, chunk * 2);
            remaining -= chunk;
        }
        return;
    }

    auto getSample = [&](uint32_t frameIdx, uint32_t ch) -> float {
        if (frameIdx >= numFrames || ch >= endpoint.channels) return 0.0f;
        size_t sampleOffset = (frameIdx * endpoint.channels + ch);

        if (endpoint.isFloat && endpoint.bitsPerSample == 32) {
            const float* floatPtr = reinterpret_cast<const float*>(srcData);
            return floatPtr[sampleOffset];
        } else if (endpoint.bitsPerSample == 16) {
            const int16_t* intPtr = reinterpret_cast<const int16_t*>(srcData);
            return intPtr[sampleOffset] / 32768.0f;
        } else if (endpoint.bitsPerSample == 24) {
            const BYTE* bytePtr = srcData + sampleOffset * 3;
            int32_t val = (static_cast<int32_t>(bytePtr[2]) << 24) |
                          (static_cast<int32_t>(bytePtr[1]) << 16) |
                          (static_cast<int32_t>(bytePtr[0]) << 8);
            return (val >> 8) / 8388608.0f;
        } else if (endpoint.bitsPerSample == 32) {
            const int32_t* intPtr = reinterpret_cast<const int32_t*>(srcData);
            return intPtr[sampleOffset] / 2147483648.0f;
        }
        return 0.0f;
    };

    auto getStereoPair = [&](uint32_t frameIdx) -> std::pair<float, float> {
        if (endpoint.channels == 1) {
            float mono = getSample(frameIdx, 0);
            return {mono, mono};
        } else if (endpoint.channels == 2) {
            return {getSample(frameIdx, 0), getSample(frameIdx, 1)};
        } else if (endpoint.channels >= 6) {
            float fl = getSample(frameIdx, 0);
            float fr = getSample(frameIdx, 1);
            float fc = getSample(frameIdx, 2);
            float lfe = getSample(frameIdx, 3);
            float sl = getSample(frameIdx, 4);
            float sr = getSample(frameIdx, 5);
            float left = (fl + 0.707f * fc + 0.5f * lfe + 0.707f * sl) * 0.6f;
            float right = (fr + 0.707f * fc + 0.5f * lfe + 0.707f * sr) * 0.6f;
            return {left, right};
        }
        return {getSample(frameIdx, 0), getSample(frameIdx, 1)};
    };

    std::vector<float> resampled(outFrames * 2);
    for (uint32_t i = 0; i < outFrames; ++i) {
        float left = 0.0f;
        float right = 0.0f;

        if (endpoint.sampleRate == targetRate) {
            auto [l, r] = getStereoPair(i);
            left = l;
            right = r;
        } else {
            double srcPos = i * resampleRatio;
            uint32_t idx0 = static_cast<uint32_t>(srcPos);
            uint32_t idx1 = (std::min)(idx0 + 1, numFrames - 1);
            float frac = static_cast<float>(srcPos - idx0);

            auto [l0, r0] = getStereoPair(idx0);
            auto [l1, r1] = getStereoPair(idx1);
            left = l0 + frac * (l1 - l0);
            right = r0 + frac * (r1 - r0);
        }

        resampled[i * 2 + 0] = left;
        resampled[i * 2 + 1] = right;
    }
    outRing.Push(resampled.data(), resampled.size());
}

void WasapiAudioCapture::CaptureThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    timeBeginPeriod(1);

    if (m_systemEndpoint.isInitialized) {
        m_systemEndpoint.client->Start();
    }
    if (m_micEndpoint.isInitialized) {
        m_micEndpoint.client->Start();
    }

    std::vector<HANDLE> waitHandles;
    waitHandles.push_back(m_stopEvent);
    if (m_systemEndpoint.eventHandle) waitHandles.push_back(m_systemEndpoint.eventHandle);
    if (m_micEndpoint.eventHandle) waitHandles.push_back(m_micEndpoint.eventHandle);

    AudioRingBuffer sysQueue;
    sysQueue.Init(48000 * 2); // 1-second capacity
    AudioRingBuffer micQueue;
    micQueue.Init(48000 * 2); // 1-second capacity

    const uint32_t targetSampleRate = 48000;
    const uint32_t CHUNK_FRAMES = 480; // 10ms at 48000 Hz

    float sysChunk[CHUNK_FRAMES * 2];
    float micChunk[CHUNK_FRAMES * 2];
    int16_t pcm16Chunk[CHUNK_FRAMES * 2];

    bool sysActive = m_systemEndpoint.isInitialized;
    bool micActive = m_micEndpoint.isInitialized;

    int64_t captureStartTimeNs = Core::GetCurrentTimeNs();
    uint64_t totalFramesEmitted = 0;

    while (!m_stopRequested.load()) {
        DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()),
            waitHandles.data(),
            FALSE,
            5 // 5ms timeout for tight drift-free polling
        );

        if (waitResult == WAIT_OBJECT_0) {
            break;
        }

        // 1. Drain System Audio packets into sysQueue
        if (sysActive) {
            UINT32 packetLen = 0;
            while (SUCCEEDED(m_systemEndpoint.captureClient->GetNextPacketSize(&packetLen)) && packetLen > 0) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                if (SUCCEEDED(m_systemEndpoint.captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr))) {
                    bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    ConvertToCanonicalFloatStereo(pData, numFrames, m_systemEndpoint, sysQueue, isSilent);
                    m_systemFramesCaptured += numFrames;
                    m_systemEndpoint.captureClient->ReleaseBuffer(numFrames);
                }
            }
        }

        // 2. Drain Microphone packets into micQueue
        if (micActive) {
            UINT32 packetLen = 0;
            while (SUCCEEDED(m_micEndpoint.captureClient->GetNextPacketSize(&packetLen)) && packetLen > 0) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                if (SUCCEEDED(m_micEndpoint.captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr))) {
                    bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    ConvertToCanonicalFloatStereo(pData, numFrames, m_micEndpoint, micQueue, isSilent);
                    m_micFramesCaptured += numFrames;
                    m_micEndpoint.captureClient->ReleaseBuffer(numFrames);
                }
            }
        }

        // 3. Wall-Clock Synchronized Parallel Audio Mixer:
        int64_t nowNs = Core::GetCurrentTimeNs();
        int64_t elapsedNs = nowNs - captureStartTimeNs;
        if (elapsedNs < 0) elapsedNs = 0;

        uint64_t targetTotalFrames = (static_cast<uint64_t>(elapsedNs) * targetSampleRate) / 1000000000ULL;

        while (!m_stopRequested.load()) {
            size_t sysAvailable = sysActive ? (sysQueue.Available() / 2) : 0;
            size_t micAvailable = micActive ? (micQueue.Available() / 2) : 0;

            bool behindTarget = (totalFramesEmitted + CHUNK_FRAMES <= targetTotalFrames);
            bool queuesReady = false;
            if (sysActive && micActive) {
                queuesReady = (sysAvailable >= CHUNK_FRAMES && micAvailable >= CHUNK_FRAMES);
            } else if (sysActive) {
                queuesReady = (sysAvailable >= CHUNK_FRAMES);
            } else if (micActive) {
                queuesReady = (micAvailable >= CHUNK_FRAMES);
            }

            // If we are not behind wall-clock time AND queues don't have enough to emit, wait for next tick
            if (!behindTarget && !queuesReady) {
                break;
            }

            memset(sysChunk, 0, sizeof(sysChunk));
            memset(micChunk, 0, sizeof(micChunk));

            if (sysActive && sysAvailable > 0) {
                size_t popCount = (std::min)(sysAvailable, static_cast<size_t>(CHUNK_FRAMES));
                sysQueue.Pop(sysChunk, popCount * 2);
            }
            if (micActive && micAvailable > 0) {
                size_t popCount = (std::min)(micAvailable, static_cast<size_t>(CHUNK_FRAMES));
                micQueue.Pop(micChunk, popCount * 2);
            }

            const float sysVol = m_systemVolume.load();
            const float micVol = m_micVolume.load();

            for (uint32_t i = 0; i < CHUNK_FRAMES * 2; ++i) {
                float mix = sysChunk[i] * sysVol + micChunk[i] * micVol;
                mix = (std::max)(-1.0f, (std::min)(1.0f, mix));
                pcm16Chunk[i] = static_cast<int16_t>(mix * 32767.0f);
            }

            if (m_callback) {
                m_callback(pcm16Chunk, CHUNK_FRAMES, nowNs);
            }

            totalFramesEmitted += CHUNK_FRAMES;
            m_totalSamplesMixed = totalFramesEmitted;
        }

        // 4. Drift protection: prevent queue accumulation beyond 100ms (4800 samples)
        const size_t MAX_QUEUE_SAMPLES = 4800 * 2;
        if (sysQueue.Available() > MAX_QUEUE_SAMPLES) {
            sysQueue.Discard(sysQueue.Available() - 2400 * 2);
        }
        if (micQueue.Available() > MAX_QUEUE_SAMPLES) {
            micQueue.Discard(micQueue.Available() - 2400 * 2);
        }
    }

    if (m_systemEndpoint.isInitialized) {
        m_systemEndpoint.client->Stop();
    }
    if (m_micEndpoint.isInitialized) {
        m_micEndpoint.client->Stop();
    }

    timeEndPeriod(1);
    CoUninitialize();
}

AudioStats WasapiAudioCapture::GetStats() const {
    AudioStats stats{};
    stats.systemFramesCaptured = m_systemFramesCaptured.load();
    stats.micFramesCaptured = m_micFramesCaptured.load();
    stats.totalSamplesMixed = m_totalSamplesMixed.load();
    stats.isSystemAudioActive = m_systemEndpoint.isInitialized;
    stats.isMicActive = m_micEndpoint.isInitialized;
    return stats;
}

void WasapiAudioCapture::SetVolume(float systemVolume, float micVolume) {
    m_systemVolume.store(systemVolume);
    m_micVolume.store(micVolume);
    m_config.systemVolume = systemVolume;
    m_config.micVolume = micVolume;
}

} // namespace LiteCapture::Audio
