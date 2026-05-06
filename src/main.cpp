#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"YouAreIdiotNativeWindow";
constexpr wchar_t kDefaultMediaPath[] = L"media\\youare.mp4";
constexpr wchar_t kDefaultAudioPath[] = L"media\\youare.wav";
constexpr UINT_PTR kFrameTimer = 1;
constexpr int kQuitHotkeyId = 100;
constexpr int kArrowCursorResource = 32512;
constexpr int kWarningIconResource = 32515;
constexpr int kClientWidth = 357;
constexpr int kClientHeight = 330;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        Reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* Get() const {
        return ptr_;
    }

    T** Put() {
        Reset();
        return &ptr_;
    }

    void Attach(T* ptr) {
        Reset();
        ptr_ = ptr;
    }

    void Reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T* operator->() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

private:
    T* ptr_ = nullptr;
};

struct Settings {
    unsigned int maxWindows = -1;
    int fps = 120;
    bool topmost = true;
    bool sound = true;
    bool dodgeCursor = true;
    std::wstring mediaPath;
    std::wstring audioPath;
};

struct WindowState {
    HWND hwnd = nullptr;
    bool root = false;
    int id = 0;
    int width = 0;
    int height = 0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    ULONGLONG lastFrameAt = 0;
    std::uint32_t audioVoiceId = 0;
};

class VideoPlayback {
public:
    VideoPlayback() {
        InitializeCriticalSection(&frameLock_);
    }

    ~VideoPlayback() {
        Stop();
        DeleteCriticalSection(&frameLock_);
    }

    HRESULT Start(const std::wstring& path) {
        workerPath_ = path;
        startupHr_ = E_PENDING;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent_ == nullptr || readyEvent_ == nullptr) {
            Stop();
            return HRESULT_FROM_WIN32(GetLastError());
        }

        workerThread_ = CreateThread(nullptr, 0, &VideoPlayback::ThreadEntry, this, 0, nullptr);
        if (workerThread_ == nullptr) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            Stop();
            return hr;
        }

        WaitForSingleObject(readyEvent_, INFINITE);
        return startupHr_;
    }

    void Stop() {
        if (stopEvent_ != nullptr) {
            SetEvent(stopEvent_);
        }

        if (workerThread_ != nullptr) {
            WaitForSingleObject(workerThread_, INFINITE);
            CloseHandle(workerThread_);
            workerThread_ = nullptr;
        }

        if (readyEvent_ != nullptr) {
            CloseHandle(readyEvent_);
            readyEvent_ = nullptr;
        }

        if (stopEvent_ != nullptr) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    void LockFrame() {
        EnterCriticalSection(&frameLock_);
    }

    void UnlockFrame() {
        LeaveCriticalSection(&frameLock_);
    }

    HRESULT Open(const std::wstring& path) {
        path_ = path;

        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(attributes.Put(), 1);
        if (FAILED(hr)) {
            SetError(L"MFCreateAttributes failed", hr);
            return hr;
        }

        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader_.Put());
        if (FAILED(hr)) {
            SetError(L"MFCreateSourceReaderFromURL failed", hr);
            return hr;
        }

        hr = ConfigureRgbOutput();
        if (FAILED(hr)) {
            return hr;
        }

        hr = ReadNextFrame(false, nullptr);
        if (FAILED(hr)) {
            SetError(L"Could not decode the first video frame", hr);
            return hr;
        }

        if (!hasFrame_) {
            hr = E_FAIL;
            SetError(L"The video did not produce a frame", hr);
            return hr;
        }

        startTick_ = GetTickCount64();
        displayedFrameIndex_ = 0;
        return S_OK;
    }

    void Close() {
        EnterCriticalSection(&frameLock_);
        reader_.Reset();
        pixels_.clear();
        hasFrame_ = false;
        LeaveCriticalSection(&frameLock_);
    }

    void Advance() {
        if (!hasFrame_ || !reader_) {
            return;
        }

        const ULONGLONG elapsedMs = GetTickCount64() - startTick_;
        const auto targetFrame = static_cast<std::uint64_t>(
            (static_cast<unsigned long long>(elapsedMs) * 10000ull) /
            static_cast<unsigned long long>(frameDuration100ns_));

        int decoded = 0;
        while (displayedFrameIndex_ < targetFrame && decoded < 8) {
            bool looped = false;
            const HRESULT hr = ReadNextFrame(true, &looped);
            if (FAILED(hr)) {
                return;
            }

            if (looped) {
                startTick_ = GetTickCount64();
                displayedFrameIndex_ = 0;
                return;
            }

            ++displayedFrameIndex_;
            ++decoded;
        }
    }

    bool HasFrame() const {
        return hasFrame_ && !pixels_.empty();
    }

    const std::uint8_t* Pixels() const {
        return pixels_.data();
    }

    const BITMAPINFO& BitmapInfo() const {
        return bitmapInfo_;
    }

    int Width() const {
        return static_cast<int>(width_);
    }

    int Height() const {
        return static_cast<int>(height_);
    }

    const std::wstring& Error() const {
        return error_;
    }

private:
    static DWORD WINAPI ThreadEntry(void* context) {
        return static_cast<VideoPlayback*>(context)->RunThread();
    }

    DWORD RunThread() {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(coHr);

        HRESULT hr = Open(workerPath_);
        startupHr_ = hr;
        SetEvent(readyEvent_);
        if (FAILED(hr)) {
            Close();
            if (uninitializeCom) {
                CoUninitialize();
            }
            return 1;
        }

        const DWORD waitMs = std::max<DWORD>(1, static_cast<DWORD>(frameDuration100ns_ / 20000));
        while (WaitForSingleObject(stopEvent_, waitMs) == WAIT_TIMEOUT) {
            Advance();
        }

        Close();
        if (uninitializeCom) {
            CoUninitialize();
        }
        return 0;
    }

    HRESULT ConfigureRgbOutput() {
        HRESULT hr = ConfigureRgbOutput(kClientWidth, kClientHeight);
        if (SUCCEEDED(hr)) {
            return S_OK;
        }

        return ConfigureRgbOutput(0, 0);
    }

    HRESULT ConfigureRgbOutput(UINT32 outputWidth, UINT32 outputHeight) {
        ComPtr<IMFMediaType> mediaType;
        HRESULT hr = MFCreateMediaType(mediaType.Put());
        if (FAILED(hr)) {
            SetError(L"MFCreateMediaType failed", hr);
            return hr;
        }

        hr = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) {
            hr = mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        }
        if (FAILED(hr)) {
            SetError(L"Could not configure RGB32 output type", hr);
            return hr;
        }

        if (outputWidth > 0 && outputHeight > 0) {
            hr = MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, outputWidth, outputHeight);
            if (FAILED(hr)) {
                SetError(L"Could not configure scaled RGB32 output size", hr);
                return hr;
            }
        }

        hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType.Get());
        if (FAILED(hr)) {
            SetError(L"SetCurrentMediaType RGB32 failed", hr);
            return hr;
        }

        return RefreshOutputFormat();
    }

    HRESULT RefreshOutputFormat() {
        ComPtr<IMFMediaType> actualType;
        HRESULT hr = reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, actualType.Put());
        if (FAILED(hr)) {
            SetError(L"GetCurrentMediaType failed", hr);
            return hr;
        }

        UINT32 width = 0;
        UINT32 height = 0;
        hr = MFGetAttributeSize(actualType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr) || width == 0 || height == 0) {
            SetError(L"Could not read video frame size", FAILED(hr) ? hr : E_FAIL);
            return FAILED(hr) ? hr : E_FAIL;
        }

        UINT32 numerator = 0;
        UINT32 denominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(actualType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) &&
            numerator > 0 &&
            denominator > 0) {
            frameDuration100ns_ = std::max<LONGLONG>(
                1,
                static_cast<LONGLONG>((10000000ull * static_cast<unsigned long long>(denominator)) /
                                      static_cast<unsigned long long>(numerator)));
        }

        EnterCriticalSection(&frameLock_);
        width_ = width;
        height_ = height;
        pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4u, 0);

        std::memset(&bitmapInfo_, 0, sizeof(bitmapInfo_));
        bitmapInfo_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo_.bmiHeader.biWidth = static_cast<LONG>(width_);
        bitmapInfo_.bmiHeader.biHeight = -static_cast<LONG>(height_);
        bitmapInfo_.bmiHeader.biPlanes = 1;
        bitmapInfo_.bmiHeader.biBitCount = 32;
        bitmapInfo_.bmiHeader.biCompression = BI_RGB;
        bitmapInfo_.bmiHeader.biSizeImage =
            static_cast<DWORD>(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4u);
        LeaveCriticalSection(&frameLock_);

        return S_OK;
    }

    HRESULT SeekToStart() {
        PROPVARIANT position{};
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = 0;
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(hr)) {
            SetError(L"Could not loop video to the beginning", hr);
        }
        return hr;
    }

    HRESULT ReadNextFrame(bool allowLoop, bool* looped) {
        if (looped != nullptr) {
            *looped = false;
        }

        for (int attempts = 0; attempts < 64; ++attempts) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG sampleTime = 0;
            ComPtr<IMFSample> sample;

            HRESULT hr = reader_->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                &streamIndex,
                &flags,
                &sampleTime,
                sample.Put());
            if (FAILED(hr)) {
                SetError(L"ReadSample failed", hr);
                return hr;
            }

            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                hr = RefreshOutputFormat();
                if (FAILED(hr)) {
                    return hr;
                }
            }

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                if (!allowLoop) {
                    return S_FALSE;
                }

                hr = SeekToStart();
                if (FAILED(hr)) {
                    return hr;
                }

                if (looped != nullptr) {
                    *looped = true;
                }
                return ReadNextFrame(false, nullptr);
            }

            if (sample) {
                hr = CopySample(sample.Get());
                if (FAILED(hr)) {
                    SetError(L"Could not copy decoded frame pixels", hr);
                    return hr;
                }
                hasFrame_ = true;
                return S_OK;
            }
        }

        return S_FALSE;
    }

    HRESULT CopySample(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(buffer.Put());
        if (FAILED(hr)) {
            return hr;
        }

        IMF2DBuffer* raw2D = nullptr;
        hr = buffer->QueryInterface(IID_PPV_ARGS(&raw2D));
        if (SUCCEEDED(hr) && raw2D != nullptr) {
            ComPtr<IMF2DBuffer> buffer2D;
            buffer2D.Attach(raw2D);

            BYTE* scanline = nullptr;
            LONG pitch = 0;
            hr = buffer2D->Lock2D(&scanline, &pitch);
            if (FAILED(hr)) {
                return hr;
            }

            CopyRows(scanline, pitch);
            buffer2D->Unlock2D();
            return S_OK;
        }

        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = buffer->Lock(&data, &maxLength, &currentLength);
        if (FAILED(hr)) {
            return hr;
        }

        EnterCriticalSection(&frameLock_);
        const LONG pitch = static_cast<LONG>(width_ * 4u);
        const LONG rows = std::min<LONG>(static_cast<LONG>(height_), static_cast<LONG>(currentLength / pitch));
        const std::size_t rowBytes = static_cast<std::size_t>(pitch);
        for (LONG y = 0; y < rows; ++y) {
            std::memcpy(pixels_.data() + static_cast<std::size_t>(y) * rowBytes, data + static_cast<std::size_t>(y) * rowBytes, rowBytes);
        }
        LeaveCriticalSection(&frameLock_);

        buffer->Unlock();
        return S_OK;
    }

    void CopyRows(const BYTE* source, LONG sourcePitch) {
        EnterCriticalSection(&frameLock_);
        const auto destinationPitch = static_cast<std::size_t>(width_) * 4u;
        const auto sourceRowBytes = static_cast<std::size_t>(std::abs(sourcePitch));
        const auto copyBytes = std::min(destinationPitch, sourceRowBytes);

        for (UINT32 y = 0; y < height_; ++y) {
            const BYTE* sourceRow = source + static_cast<LONGLONG>(y) * sourcePitch;
            std::uint8_t* destinationRow = pixels_.data() + static_cast<std::size_t>(y) * destinationPitch;
            std::memcpy(destinationRow, sourceRow, copyBytes);
        }
        LeaveCriticalSection(&frameLock_);
    }

    void SetError(const wchar_t* message, HRESULT hr) {
        wchar_t buffer[256]{};
        std::swprintf(buffer, 256, L"%ls (HRESULT 0x%08lX)", message, static_cast<unsigned long>(hr));
        error_ = buffer;
    }

    std::wstring path_;
    std::wstring workerPath_;
    ComPtr<IMFSourceReader> reader_;
    CRITICAL_SECTION frameLock_{};
    HANDLE workerThread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HRESULT startupHr_ = E_PENDING;
    UINT32 width_ = 0;
    UINT32 height_ = 0;
    LONGLONG frameDuration100ns_ = 166833;
    std::uint64_t displayedFrameIndex_ = 0;
    ULONGLONG startTick_ = 0;
    bool hasFrame_ = false;
    std::vector<std::uint8_t> pixels_;
    BITMAPINFO bitmapInfo_{};
    std::wstring error_;
};

bool FileExists(const std::wstring& path);

class AudioPlayback {
public:
    AudioPlayback() {
        InitializeCriticalSection(&voicesLock_);
    }

    ~AudioPlayback() {
        Stop();
        DeleteCriticalSection(&voicesLock_);
    }

    bool Start(const std::wstring& path) {
        path_ = path;
        if (!FileExists(path_) || !LoadWave(path_)) {
            return false;
        }

        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent_ == nullptr) {
            return false;
        }

        thread_ = CreateThread(nullptr, 0, &AudioPlayback::ThreadEntry, this, 0, nullptr);
        if (thread_ == nullptr) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
            return false;
        }

        return true;
    }

    void Stop() {
        if (stopEvent_ != nullptr) {
            SetEvent(stopEvent_);
        }

        if (thread_ != nullptr) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }

        if (stopEvent_ != nullptr) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    void AddVoice(std::uint32_t id) {
        if (totalFrames_ == 0) {
            return;
        }

        EnterCriticalSection(&voicesLock_);
        const auto offset = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(id) * 11407ull + GetTickCount64()) %
            static_cast<std::uint64_t>(totalFrames_));
        voices_.push_back({id, offset});
        LeaveCriticalSection(&voicesLock_);
    }

    void RemoveVoice(std::uint32_t id) {
        EnterCriticalSection(&voicesLock_);
        voices_.erase(
            std::remove_if(
                voices_.begin(),
                voices_.end(),
                [id](const Voice& voice) {
                    return voice.id == id;
                }),
            voices_.end());
        LeaveCriticalSection(&voicesLock_);
    }

private:
    struct Voice {
        std::uint32_t id = 0;
        std::uint32_t offsetFrames = 0;
    };

    static DWORD WINAPI ThreadEntry(void* context) {
        return static_cast<AudioPlayback*>(context)->RunThread();
    }

    DWORD RunThread() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = sampleRate_;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HANDLE doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (doneEvent == nullptr) {
            return 1;
        }

        HWAVEOUT waveOut = nullptr;
        MMRESULT result = waveOutOpen(&waveOut, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(doneEvent), 0, CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR) {
            CloseHandle(doneEvent);
            return 1;
        }

        constexpr std::size_t kBufferCount = 4;
        constexpr std::size_t kFramesPerBuffer = 1024;
        std::vector<std::vector<std::int16_t>> buffers(kBufferCount);
        std::vector<WAVEHDR> headers(kBufferCount);

        for (std::size_t i = 0; i < kBufferCount; ++i) {
            buffers[i].resize(kFramesPerBuffer * 2);
            FillBuffer(buffers[i].data(), kFramesPerBuffer);
            std::memset(&headers[i], 0, sizeof(WAVEHDR));
            headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(std::int16_t));
            waveOutPrepareHeader(waveOut, &headers[i], sizeof(WAVEHDR));
            waveOutWrite(waveOut, &headers[i], sizeof(WAVEHDR));
        }

        while (WaitForSingleObject(stopEvent_, 1) == WAIT_TIMEOUT) {
            WaitForSingleObject(doneEvent, 10);
            for (std::size_t i = 0; i < kBufferCount; ++i) {
                if ((headers[i].dwFlags & WHDR_DONE) == 0) {
                    continue;
                }

                waveOutUnprepareHeader(waveOut, &headers[i], sizeof(WAVEHDR));
                FillBuffer(buffers[i].data(), kFramesPerBuffer);
                std::memset(&headers[i], 0, sizeof(WAVEHDR));
                headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
                headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(std::int16_t));
                waveOutPrepareHeader(waveOut, &headers[i], sizeof(WAVEHDR));
                waveOutWrite(waveOut, &headers[i], sizeof(WAVEHDR));
            }
        }

        waveOutReset(waveOut);
        for (auto& header : headers) {
            if ((header.dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(waveOut, &header, sizeof(WAVEHDR));
            }
        }
        waveOutClose(waveOut);
        CloseHandle(doneEvent);
        return 0;
    }

    void FillBuffer(std::int16_t* output, std::size_t frames) {
        std::vector<Voice> voices;
        EnterCriticalSection(&voicesLock_);
        voices = voices_;
        LeaveCriticalSection(&voicesLock_);

        if (samples_.empty() || totalFrames_ == 0 || voices.empty()) {
            std::memset(output, 0, frames * 2 * sizeof(std::int16_t));
            playbackFrame_ = (playbackFrame_ + static_cast<std::uint32_t>(frames)) % std::max<std::uint32_t>(1, totalFrames_);
            return;
        }

        const int divisor = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(voices.size()))));
        for (std::size_t frame = 0; frame < frames; ++frame) {
            int mixedLeft = 0;
            int mixedRight = 0;

            for (const auto& voice : voices) {
                const auto sourceFrame = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(playbackFrame_) + frame + voice.offsetFrames) %
                    static_cast<std::uint64_t>(totalFrames_));
                const auto sampleIndex = static_cast<std::size_t>(sourceFrame) * sourceChannels_;
                const int left = samples_[sampleIndex];
                const int right = sourceChannels_ > 1 ? samples_[sampleIndex + 1] : left;
                mixedLeft += left;
                mixedRight += right;
            }

            mixedLeft = std::clamp(mixedLeft / divisor, -32768, 32767);
            mixedRight = std::clamp(mixedRight / divisor, -32768, 32767);
            output[frame * 2] = static_cast<std::int16_t>(mixedLeft);
            output[frame * 2 + 1] = static_cast<std::int16_t>(mixedRight);
        }

        playbackFrame_ = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(playbackFrame_) + frames) %
            static_cast<std::uint64_t>(totalFrames_));
    }

    bool LoadWave(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 64ll * 1024ll * 1024ll) {
            CloseHandle(file);
            return false;
        }

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize.QuadPart));
        DWORD read = 0;
        const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != bytes.size() || bytes.size() < 44) {
            return false;
        }

        if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
            return false;
        }

        WORD audioFormat = 0;
        WORD channels = 0;
        DWORD sampleRate = 0;
        WORD bitsPerSample = 0;
        std::size_t dataOffset = 0;
        std::size_t dataSize = 0;

        std::size_t offset = 12;
        while (offset + 8 <= bytes.size()) {
            const char* chunkId = reinterpret_cast<const char*>(bytes.data() + offset);
            const DWORD chunkSize =
                static_cast<DWORD>(bytes[offset + 4]) |
                (static_cast<DWORD>(bytes[offset + 5]) << 8) |
                (static_cast<DWORD>(bytes[offset + 6]) << 16) |
                (static_cast<DWORD>(bytes[offset + 7]) << 24);
            const std::size_t chunkData = offset + 8;
            if (chunkData + chunkSize > bytes.size()) {
                break;
            }

            if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
                audioFormat = ReadU16(bytes, chunkData);
                channels = ReadU16(bytes, chunkData + 2);
                sampleRate = ReadU32(bytes, chunkData + 4);
                bitsPerSample = ReadU16(bytes, chunkData + 14);
            } else if (std::memcmp(chunkId, "data", 4) == 0) {
                dataOffset = chunkData;
                dataSize = chunkSize;
            }

            offset = chunkData + chunkSize + (chunkSize & 1u);
        }

        if (audioFormat != WAVE_FORMAT_PCM || (channels != 1 && channels != 2) || sampleRate == 0 ||
            bitsPerSample != 16 || dataOffset == 0 || dataSize < channels * sizeof(std::int16_t)) {
            return false;
        }

        sourceChannels_ = channels;
        sampleRate_ = sampleRate;
        samples_.resize(dataSize / sizeof(std::int16_t));
        std::memcpy(samples_.data(), bytes.data() + dataOffset, samples_.size() * sizeof(std::int16_t));
        totalFrames_ = static_cast<std::uint32_t>(samples_.size() / sourceChannels_);
        playbackFrame_ = 0;
        return totalFrames_ > 0;
    }

    static WORD ReadU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
        return static_cast<WORD>(bytes[offset] | (bytes[offset + 1] << 8));
    }

    static DWORD ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
        return
            static_cast<DWORD>(bytes[offset]) |
            (static_cast<DWORD>(bytes[offset + 1]) << 8) |
            (static_cast<DWORD>(bytes[offset + 2]) << 16) |
            (static_cast<DWORD>(bytes[offset + 3]) << 24);
    }

    std::wstring path_;
    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    CRITICAL_SECTION voicesLock_{};
    std::vector<Voice> voices_;
    std::vector<std::int16_t> samples_;
    WORD sourceChannels_ = 2;
    DWORD sampleRate_ = 48000;
    std::uint32_t totalFrames_ = 0;
    std::uint32_t playbackFrame_ = 0;
};

HINSTANCE g_instance = nullptr;
Settings g_settings;
VideoPlayback g_video;
AudioPlayback g_audio;
std::vector<std::unique_ptr<WindowState>> g_windows;
std::mt19937 g_rng{std::random_device{}()};
bool g_helpOnly = false;
int g_nextWindowId = 1;

int RandomInt(int minValue, int maxValue) {
    std::uniform_int_distribution<int> dist(minValue, maxValue);
    return dist(g_rng);
}

double RandomDouble(double minValue, double maxValue) {
    std::uniform_real_distribution<double> dist(minValue, maxValue);
    return dist(g_rng);
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }

    path.resize(length);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring ResolveMediaPath() {
    if (!g_settings.mediaPath.empty()) {
        return g_settings.mediaPath;
    }

    const std::wstring besideExecutable = ModuleDirectory() + L"\\" + kDefaultMediaPath;
    if (FileExists(besideExecutable)) {
        return besideExecutable;
    }

    if (FileExists(kDefaultMediaPath)) {
        return kDefaultMediaPath;
    }

    return besideExecutable;
}

std::wstring ResolveAudioPath() {
    if (!g_settings.audioPath.empty()) {
        return g_settings.audioPath;
    }

    const std::wstring besideExecutable = ModuleDirectory() + L"\\" + kDefaultAudioPath;
    if (FileExists(besideExecutable)) {
        return besideExecutable;
    }

    if (FileExists(kDefaultAudioPath)) {
        return kDefaultAudioPath;
    }

    return besideExecutable;
}

RECT VirtualScreenBounds() {
    RECT rect{};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect;
}

int TimerIntervalMs() {
    return std::max(1, 1000 / std::max(1, g_settings.fps));
}

void ComputeOuterWindowSize(DWORD style, DWORD exStyle, int& width, int& height) {
    RECT rect{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
}

void SetRandomVelocity(WindowState& state) {
    const double speed = RandomDouble(1400.0, 2600.0);
    const double angle = RandomDouble(0.35, 5.95);
    state.vx = std::cos(angle) * speed;
    state.vy = std::sin(angle) * speed;

    if (std::abs(state.vx) < 650.0) {
        state.vx = std::copysign(650.0, state.vx == 0.0 ? 1.0 : state.vx);
    }
    if (std::abs(state.vy) < 500.0) {
        state.vy = std::copysign(500.0, state.vy == 0.0 ? 1.0 : state.vy);
    }
}

void ClampInsideScreen(WindowState& state, const RECT& bounds) {
    const int maxX = std::max(bounds.left, bounds.right - state.width);
    const int maxY = std::max(bounds.top, bounds.bottom - state.height);
    state.x = std::clamp(state.x, static_cast<double>(bounds.left), static_cast<double>(maxX));
    state.y = std::clamp(state.y, static_cast<double>(bounds.top), static_cast<double>(maxY));
}

void Bounce(WindowState& state, const RECT& bounds) {
    bool bounced = false;

    if (state.x <= bounds.left) {
        state.x = bounds.left;
        state.vx = std::abs(state.vx) + RandomDouble(120.0, 360.0);
        bounced = true;
    } else if (state.x + state.width >= bounds.right) {
        state.x = bounds.right - state.width;
        state.vx = -std::abs(state.vx) - RandomDouble(120.0, 360.0);
        bounced = true;
    }

    if (state.y <= bounds.top) {
        state.y = bounds.top;
        state.vy = std::abs(state.vy) + RandomDouble(120.0, 360.0);
        bounced = true;
    } else if (state.y + state.height >= bounds.bottom) {
        state.y = bounds.bottom - state.height;
        state.vy = -std::abs(state.vy) - RandomDouble(120.0, 360.0);
        bounced = true;
    }

    if (bounced && RandomInt(0, 3) == 0) {
        SetRandomVelocity(state);
        ClampInsideScreen(state, bounds);
    }
}

void DodgeCursor(WindowState& state) {
    if (!g_settings.dodgeCursor) {
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }

    RECT windowRect{};
    if (!GetWindowRect(state.hwnd, &windowRect)) {
        return;
    }

    RECT hotRect = windowRect;
    InflateRect(&hotRect, 28, 28);
    if (!PtInRect(&hotRect, cursor)) {
        return;
    }

    const double centerX = static_cast<double>(windowRect.left + windowRect.right) / 2.0;
    const double centerY = static_cast<double>(windowRect.top + windowRect.bottom) / 2.0;
    double dx = centerX - static_cast<double>(cursor.x);
    double dy = centerY - static_cast<double>(cursor.y);
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1.0) {
        dx = RandomDouble(-1.0, 1.0);
        dy = RandomDouble(-1.0, 1.0);
    } else {
        dx /= length;
        dy /= length;
    }

    const double speed = RandomDouble(1800.0, 3200.0);
    state.vx = dx * speed;
    state.vy = dy * speed;
}

void StepWindow(WindowState& state) {
    const ULONGLONG now = GetTickCount64();
    if (state.lastFrameAt == 0) {
        state.lastFrameAt = now;
    }

    double dt = static_cast<double>(now - state.lastFrameAt) / 1000.0;
    state.lastFrameAt = now;
    dt = std::clamp(dt, 0.001, 0.025);

    DodgeCursor(state);
    state.x += state.vx * dt;
    state.y += state.vy * dt;

    const RECT bounds = VirtualScreenBounds();
    Bounce(state, bounds);

    UINT flags = SWP_NOSIZE | SWP_NOACTIVATE;
    HWND zOrder = HWND_TOPMOST;
    if (!g_settings.topmost) {
        flags |= SWP_NOZORDER;
        zOrder = nullptr;
    }

    SetWindowPos(
        state.hwnd,
        zOrder,
        static_cast<int>(std::lround(state.x)),
        static_cast<int>(std::lround(state.y)),
        0,
        0,
        flags);
}

void DrawScene(HDC targetDc, const RECT& clientRect) {
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;

    g_video.LockFrame();
    if (!g_video.HasFrame()) {
        g_video.UnlockFrame();
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(targetDc, &clientRect, brush);
        DeleteObject(brush);
        return;
    }

    SetStretchBltMode(targetDc, COLORONCOLOR);
    if (width == g_video.Width() && height == g_video.Height()) {
        SetDIBitsToDevice(
            targetDc,
            0,
            0,
            width,
            height,
            0,
            0,
            0,
            static_cast<UINT>(g_video.Height()),
            g_video.Pixels(),
            &g_video.BitmapInfo(),
            DIB_RGB_COLORS);
        g_video.UnlockFrame();
        return;
    }

    StretchDIBits(
        targetDc,
        0,
        0,
        width,
        height,
        0,
        0,
        g_video.Width(),
        g_video.Height(),
        g_video.Pixels(),
        &g_video.BitmapInfo(),
        DIB_RGB_COLORS,
        SRCCOPY);
    g_video.UnlockFrame();
}

std::wstring OptionsText() {
    return
        L"Native Windows prank options:\n\n"
        L"--fps N        Animation timer rate, 15..240. Default: 120\n"
        L"--media PATH   MP4 file to decode. Default: media\\youare.mp4\n"
        L"--audio PATH   WAV file to loop. Default: media\\youare.wav\n"
        L"--no-topmost   Do not keep windows above other windows.\n"
        L"--no-sound     Disable audio playback.\n"
        L"--no-dodge     Disable cursor dodge behavior.\n"
        L"--calm         Lower-impact profile for testing.\n"
        L"--help         Show this help.\n\n"
        L"Press Esc in any prank window, or Ctrl+Shift+Q from anywhere, to close everything.";
}

bool TryParseInt(const wchar_t* text, int& value) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (end == text || *end != L'\0') {
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
}

void ParseCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto readInt = [&](int& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            ++i;
            return TryParseInt(argv[i], out);
        };
        auto readString = [&]() -> std::wstring {
            if (i + 1 >= argc) {
                return L"";
            }
            ++i;
            return argv[i];
        };

        if (arg == L"--windows") {
            int value = g_settings.maxWindows;
            if (readInt(value)) {
                g_settings.maxWindows = std::clamp(value, 1, 24);
            }
        } else if (arg == L"--fps") {
            int value = g_settings.fps;
            if (readInt(value)) {
                g_settings.fps = std::clamp(value, 15, 240);
            }
        } else if (arg == L"--media") {
            g_settings.mediaPath = readString();
        } else if (arg == L"--audio") {
            g_settings.audioPath = readString();
        } else if (arg == L"--no-topmost") {
            g_settings.topmost = false;
        } else if (arg == L"--no-sound") {
            g_settings.sound = false;
        } else if (arg == L"--no-dodge") {
            g_settings.dodgeCursor = false;
        } else if (arg == L"--calm") {
            g_settings.maxWindows = 3;
            g_settings.fps = 30;
            g_settings.topmost = false;
            g_settings.sound = false;
            g_settings.dodgeCursor = false;
        } else if (arg == L"--help" || arg == L"/?") {
            MessageBoxW(nullptr, OptionsText().c_str(), L"You are an idiot!", MB_OK | MB_ICONINFORMATION);
            g_helpOnly = true;
        }
    }

    LocalFree(argv);
}

HWND CreatePrankWindow(bool root);

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<WindowState*>(createStruct->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    case WM_CREATE:
        state->lastFrameAt = GetTickCount64();
        if (state->root) {
            SetTimer(hwnd, kFrameTimer, TimerIntervalMs(), nullptr);
        }
        return 0;

    case WM_TIMER:
        if (state == nullptr) {
            return 0;
        }

        if (wParam == kFrameTimer) {
            for (const auto& window : g_windows) {
                StepWindow(*window);
                InvalidateRect(window->hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) CreatePrankWindow(false);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        DrawScene(dc, clientRect);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        // DestroyWindow(hwnd);
        CreatePrankWindow(false);

        return 0;

    case WM_NCDESTROY:
        if (state != nullptr) {
            KillTimer(hwnd, kFrameTimer);
            g_audio.RemoveVoice(state->audioVoiceId);

            g_windows.erase(
                std::remove_if(
                    g_windows.begin(),
                    g_windows.end(),
                    [state](const std::unique_ptr<WindowState>& item) {
                        return item.get() == state;
                    }),
                g_windows.end());

            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }

        if (g_windows.empty()) {
            PostQuitMessage(0);
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

HWND CreatePrankWindow(bool root) {
    auto state = std::make_unique<WindowState>();
    state->root = root;
    state->id = g_nextWindowId++;
    state->audioVoiceId = static_cast<std::uint32_t>(state->id);

    DWORD exStyle = WS_EX_APPWINDOW;
    if (g_settings.topmost) {
        exStyle |= WS_EX_TOPMOST;
    }

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_BORDER;
    ComputeOuterWindowSize(style, exStyle, state->width, state->height);

    const RECT bounds = VirtualScreenBounds();
    const int maxX = std::max(bounds.left, bounds.right - state->width);
    const int maxY = std::max(bounds.top, bounds.bottom - state->height);
    state->x = RandomDouble(static_cast<double>(bounds.left), static_cast<double>(maxX));
    state->y = RandomDouble(static_cast<double>(bounds.top), static_cast<double>(maxY));
    SetRandomVelocity(*state);

    WindowState* rawState = state.get();
    g_windows.push_back(std::move(state));

    wchar_t title[96]{};
    std::swprintf(title, 96, L"You are an idiot! #%d", rawState->id);

    HWND hwnd = CreateWindowExW(
        exStyle,
        kWindowClass,
        title,
        style,
        static_cast<int>(std::lround(rawState->x)),
        static_cast<int>(std::lround(rawState->y)),
        rawState->width,
        rawState->height,
        nullptr,
        nullptr,
        g_instance,
        rawState);

    if (hwnd == nullptr) {
        g_windows.erase(
            std::remove_if(
                g_windows.begin(),
                g_windows.end(),
                [rawState](const std::unique_ptr<WindowState>& item) {
                    return item.get() == rawState;
                }),
            g_windows.end());
        return nullptr;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    g_audio.AddVoice(rawState->audioVoiceId);
    return hwnd;
}

bool RegisterWindowClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = static_cast<HCURSOR>(
        LoadImageW(nullptr, MAKEINTRESOURCEW(kArrowCursorResource), IMAGE_CURSOR, 0, 0, LR_SHARED));
    windowClass.hIcon = static_cast<HICON>(
        LoadImageW(nullptr, MAKEINTRESOURCEW(kWarningIconResource), IMAGE_ICON, 0, 0, LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(
        LoadImageW(nullptr, MAKEINTRESOURCEW(kWarningIconResource), IMAGE_ICON, 16, 16, LR_SHARED));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;

    return RegisterClassExW(&windowClass) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    ParseCommandLine();
    if (g_helpOnly) {
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Could not initialize COM.", L"You are an idiot!", MB_OK | MB_ICONERROR);
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        CoUninitialize();
        MessageBoxW(nullptr, L"Could not initialize Media Foundation.", L"You are an idiot!", MB_OK | MB_ICONERROR);
        return 1;
    }

    const std::wstring mediaPath = ResolveMediaPath();
    hr = g_video.Start(mediaPath);
    if (FAILED(hr)) {
        std::wstring message = L"Could not decode the prank video:\n";
        message += mediaPath;
        message += L"\n\n";
        message += g_video.Error();
        g_video.Stop();
        MFShutdown();
        CoUninitialize();
        MessageBoxW(nullptr, message.c_str(), L"You are an idiot!", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (g_settings.sound) {
        g_audio.Start(ResolveAudioPath());
    }

    if (!RegisterWindowClass()) {
        g_audio.Stop();
        g_video.Stop();
        MFShutdown();
        CoUninitialize();
        MessageBoxW(nullptr, L"Could not register the prank window class.", L"You are an idiot!", MB_OK | MB_ICONERROR);
        return 1;
    }

    const bool timerResolutionSet = timeBeginPeriod(1) == TIMERR_NOERROR;

    if (CreatePrankWindow(true) == nullptr) {
        if (timerResolutionSet) {
            timeEndPeriod(1);
        }
        g_audio.Stop();
        g_video.Stop();
        MFShutdown();
        CoUninitialize();
        MessageBoxW(nullptr, L"Could not create the prank window.", L"You are an idiot!", MB_OK | MB_ICONERROR);
        return 1;
    }

    const BOOL hotkeyRegistered = RegisterHotKey(nullptr, kQuitHotkeyId, MOD_CONTROL | MOD_SHIFT, 'Q');

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_HOTKEY && message.wParam == kQuitHotkeyId) {
            // DestroyAllWindows();
            CreatePrankWindow(false);
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (hotkeyRegistered) {
        UnregisterHotKey(nullptr, kQuitHotkeyId);
    }
    if (timerResolutionSet) {
        timeEndPeriod(1);
    }
    g_audio.Stop();
    g_video.Stop();
    MFShutdown();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
