#include "stream_video.h"
#include "stream.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace openshao::stream {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT64 kFrameDuration = 10'000'000 / kVideoFrameRate; // 100 ns units

UINT64 NowHns() {
    return UINT64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100);
}

void SetCodecValue(ICodecAPI* codec, const GUID& key, UINT32 value) {
    if (!codec) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    codec->SetValue(&key, &v);
}

void SetCodecFlag(ICodecAPI* codec, const GUID& key) {
    if (!codec) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    codec->SetValue(&key, &v);
}

ComPtr<IMFSample> MakeSample(const uint8_t* data, size_t size, size_t capacity) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateMemoryBuffer(DWORD(capacity), &buffer)) || FAILED(MFCreateSample(&sample))) return nullptr;
    if (data && size) {
        BYTE* dst = nullptr;
        if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return nullptr;
        std::memcpy(dst, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(size));
    }
    sample->AddBuffer(buffer.Get());
    return sample;
}

bool AppendSample(IMFSample* sample, std::vector<uint8_t>& out) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    out.insert(out.end(), data, data + length);
    buffer->Unlock();
    return true;
}

ComPtr<IMFMediaType> VideoType(const GUID& subtype, uint32_t width, uint32_t height) {
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kVideoFrameRate, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}

void StreamIds(IMFTransform* mft, DWORD& in, DWORD& out) {
    in = out = 0;
    DWORD ins = 0, outs = 0;
    if (FAILED(mft->GetStreamCount(&ins, &outs)) || ins != 1 || outs != 1) return;
    DWORD i = 0, o = 0;
    if (SUCCEEDED(mft->GetStreamIDs(1, &i, 1, &o))) {
        in = i;
        out = o;
    }
}

std::string FriendlyName(IMFActivate* activate) {
    wchar_t* name = nullptr;
    UINT32 length = 0;
    std::string text;
    if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)) && name) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, int(length), nullptr, 0, nullptr, nullptr);
        text.resize(size_t(bytes));
        WideCharToMultiByte(CP_UTF8, 0, name, int(length), text.data(), bytes, nullptr, nullptr);
        CoTaskMemFree(name);
    }
    return text;
}

} // namespace

bool StartMediaFoundation() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!ok) LOGE("stream: Media Foundation is unavailable");
    return ok;
}

// ---------------------------------------------------------------- encoder

struct VideoEncoder::State {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ICodecAPI> codec;
    bool async = false;
    bool provides_samples = false;
    DWORD output_size = 0;
    DWORD in_id = 0, out_id = 0;
    uint32_t width = 0, height = 0;
    uint32_t bitrate = kVideoBitrate;
    int need_input = 0;
    UINT64 start = 0;

    ~State() { Close(); }

    void Close() {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        if (events) {
            // An async transform keeps a reference to its event queue until it is shut down.
            ComPtr<IMFShutdown> shutdown;
            if (SUCCEEDED(mft.As(&shutdown))) shutdown->Shutdown();
        }
        events.Reset();
        codec.Reset();
        mft.Reset();
        need_input = 0;
        width = height = 0;
    }

    bool Configure(IMFTransform* transform, uint32_t w, uint32_t h) {
        ComPtr<IMFAttributes> attributes;
        UINT32 is_async = 0;
        if (SUCCEEDED(transform->GetAttributes(&attributes))) {
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async)
                attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        ComPtr<ICodecAPI> api;
        transform->QueryInterface(IID_PPV_ARGS(&api));
        // Rate control must be chosen before the media types on the Microsoft software encoder.
        SetCodecFlag(api.Get(), CODECAPI_AVLowLatencyMode);
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, kVideoGopFrames);
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);

        auto out_type = VideoType(MFVideoFormat_H264, w, h);
        auto in_type = VideoType(MFVideoFormat_NV12, w, h);
        if (!out_type || !in_type) return false;
        out_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        DWORD in = 0, out = 0;
        StreamIds(transform, in, out);
        if (FAILED(transform->SetOutputType(out, out_type.Get(), 0)) || FAILED(transform->SetInputType(in, in_type.Get(), 0)))
            return false;
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(transform->GetOutputStreamInfo(out, &info))) return false;
        if (is_async && FAILED(transform->QueryInterface(IID_PPV_ARGS(&events)))) return false;

        mft = transform;
        codec = api;
        async = is_async != 0;
        provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        output_size = info.cbSize ? info.cbSize : DWORD(w * h * 3 / 2);
        in_id = in;
        out_id = out;
        width = w;
        height = h;
        need_input = 0;
        start = NowHns();
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool Open(uint32_t w, uint32_t h) {
        Close();
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                &in, &out, &activates, &count))) {
            for (UINT32 i = 0; i < count; ++i) {
                if (!mft) {
                    ComPtr<IMFTransform> transform;
                    if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform))) &&
                        Configure(transform.Get(), w, h)) {
                        LOGI("stream: H.264 hardware encoder %s, %ux%u", FriendlyName(activates[i]).c_str(), w, h);
                    } else {
                        activates[i]->ShutdownObject();
                    }
                }
                activates[i]->Release();
            }
            CoTaskMemFree(activates);
        }
        if (!mft) {
            ComPtr<IMFTransform> transform;
            if (SUCCEEDED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&transform))) &&
                Configure(transform.Get(), w, h)) {
                LOGI("stream: H.264 software encoder, %ux%u", w, h);
            }
        }
        if (!mft) LOG_FIRST(1, "stream: no H.264 encoder accepted %ux%u", w, h);
        return mft != nullptr;
    }

    // Returns S_OK with output, MF_E_TRANSFORM_NEED_MORE_INPUT when empty, or an error.
    HRESULT Output(std::vector<uint8_t>& out, bool& key) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = out_id;
            ComPtr<IMFSample> own;
            if (!provides_samples) {
                own = MakeSample(nullptr, 0, output_size);
                if (!own) return E_OUTOFMEMORY;
                ob.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> type;
                if (FAILED(mft->GetOutputAvailableType(out_id, 0, &type)) || FAILED(mft->SetOutputType(out_id, type.Get(), 0)))
                    return hr;
                continue;
            }
            if (FAILED(hr)) return hr;
            if (ob.pSample) {
                UINT32 clean = 0;
                if (SUCCEEDED(ob.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) key = true;
                AppendSample(ob.pSample, out);
                if (provides_samples) ob.pSample->Release();
            }
            return S_OK;
        }
        return E_FAIL;
    }

    // Async transforms report readiness through events; poll them with a bounded wait.
    bool WaitEvent(MediaEventType wanted, std::vector<uint8_t>& out, bool& key, bool& got_output, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (wanted == METransformNeedInput && need_input > 0) return true;
            if (wanted == METransformHaveOutput && got_output) return true;
            ComPtr<IMFMediaEvent> event;
            const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) return false;
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == METransformNeedInput) {
                ++need_input;
            } else if (type == METransformHaveOutput) {
                if (FAILED(Output(out, key))) return false;
                got_output = true;
            }
        }
    }
};

VideoEncoder::VideoEncoder() : s_(std::make_unique<State>()) {}
VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::Encode(const uint8_t* nv12, uint32_t width, uint32_t height, bool keyframe,
                          std::vector<uint8_t>& out, bool& is_keyframe) {
    out.clear();
    is_keyframe = false;
    auto& s = *s_;
    if (!s.mft || s.width != width || s.height != height) {
        if (!s.Open(width, height)) return false;
        keyframe = true;
    }
    if (keyframe) SetCodecValue(s.codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    const size_t size = size_t(width) * height * 3 / 2;
    auto sample = MakeSample(nv12, size, size);
    if (!sample) return false;
    sample->SetSampleTime(LONGLONG(NowHns() - s.start));
    sample->SetSampleDuration(LONGLONG(kFrameDuration));

    bool ok = true;
    if (s.async) {
        bool got_output = false;
        ok = s.WaitEvent(METransformNeedInput, out, is_keyframe, got_output, 100) &&
             SUCCEEDED(s.mft->ProcessInput(s.in_id, sample.Get(), 0));
        if (ok) {
            --s.need_input;
            // Low-latency mode returns each frame right away; a short wait avoids a frame of delay.
            if (!got_output) s.WaitEvent(METransformHaveOutput, out, is_keyframe, got_output, 30);
        }
    } else {
        HRESULT hr = s.mft->ProcessInput(s.in_id, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            while (s.Output(out, is_keyframe) == S_OK) {}
            hr = s.mft->ProcessInput(s.in_id, sample.Get(), 0);
        }
        ok = SUCCEEDED(hr);
        while (ok) {
            hr = s.Output(out, is_keyframe);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            ok = SUCCEEDED(hr);
        }
    }
    if (!ok) {
        LOG_FIRST(1, "stream: H.264 encoding failed, reopening the encoder");
        s.Close();
    }
    return ok;
}

void VideoEncoder::SetBitrate(uint32_t bps) {
    if (!s_) return;
    s_->bitrate = bps;
    SetCodecValue(s_->codec.Get(), CODECAPI_AVEncCommonMeanBitRate, bps);
}

// ---------------------------------------------------------------- decoder

struct VideoDecoder::State {
    ComPtr<IMFTransform> mft;
    DWORD output_size = 0;
    bool provides_samples = false;
    uint32_t buffer_w = 0, buffer_h = 0;
    int32_t stride = 0;
    LONGLONG time = 0;
    bool failed_open = false;

    ~State() { Close(); }

    void Close() {
        if (mft) mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft.Reset();
    }

    bool SelectOutput() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(mft->GetOutputAvailableType(0, i, &type))) return false;
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
            if (FAILED(mft->SetOutputType(0, type.Get(), 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            buffer_w = w;
            buffer_h = h;
            UINT32 default_stride = 0;
            stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)) ? int32_t(default_stride) : int32_t(w);
            MFT_OUTPUT_STREAM_INFO info{};
            if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
            provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            output_size = info.cbSize ? info.cbSize : DWORD(w * h * 3 / 2);
            return true;
        }
    }

    bool Open() {
        Close();
        if (failed_open) return false;
        ComPtr<IMFTransform> transform;
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)))) {
            LOGE("stream: H.264 decoder is unavailable");
            failed_open = true;
            return false;
        }
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes))) attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(transform.As(&api))) SetCodecFlag(api.Get(), CODECAPI_AVLowLatencyMode);
        ComPtr<IMFMediaType> in;
        if (FAILED(MFCreateMediaType(&in))) return false;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(transform->SetInputType(0, in.Get(), 0))) {
            LOGE("stream: H.264 decoder rejected the input type");
            failed_open = true;
            return false;
        }
        mft = transform;
        if (!SelectOutput()) {
            mft.Reset();
            return false;
        }
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // Returns S_OK with a converted frame, MF_E_TRANSFORM_NEED_MORE_INPUT when empty, or an error.
    HRESULT Output(uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ComPtr<IMFSample> own;
            if (!provides_samples) {
                own = MakeSample(nullptr, 0, output_size);
                if (!own) return E_OUTOFMEMORY;
                ob.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!SelectOutput()) return hr;
                continue;
            }
            if (FAILED(hr)) return hr;
            HRESULT result = E_FAIL;
            ComPtr<IMFMediaBuffer> buffer;
            if (ob.pSample && SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&buffer))) {
                ComPtr<IMF2DBuffer> buffer2d;
                BYTE* data = nullptr;
                LONG pitch = 0;
                DWORD length = 0;
                if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch))) {
                    if (pitch > 0) {
                        Nv12ToBgra(data, data + size_t(pitch) * buffer_h, uint32_t(pitch),
                                   std::min(width, buffer_w), std::min(height, buffer_h), bgra);
                        result = S_OK;
                    }
                    buffer2d->Unlock2D();
                } else if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    const uint32_t p = uint32_t(stride > 0 ? stride : int32_t(buffer_w));
                    if (size_t(p) * buffer_h * 3 / 2 <= length) {
                        Nv12ToBgra(data, data + size_t(p) * buffer_h, p, std::min(width, buffer_w),
                                   std::min(height, buffer_h), bgra);
                        result = S_OK;
                    }
                    buffer->Unlock();
                }
            }
            if (provides_samples && ob.pSample) ob.pSample->Release();
            return result;
        }
        return E_FAIL;
    }
};

VideoDecoder::VideoDecoder() : s_(std::make_unique<State>()) {}
VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::Reset() {
    if (s_->mft) s_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool VideoDecoder::Decode(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                          std::vector<uint8_t>& bgra, bool& failed) {
    failed = false;
    auto& s = *s_;
    if (!s.mft && !s.Open()) {
        failed = true;
        return false;
    }
    auto sample = MakeSample(data, size, size);
    if (!sample) {
        failed = true;
        return false;
    }
    sample->SetSampleTime(s.time);
    sample->SetSampleDuration(LONGLONG(kFrameDuration));
    s.time += LONGLONG(kFrameDuration);
    bool got = false;
    HRESULT hr = s.mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        while (s.Output(width, height, bgra) == S_OK) got = true;
        hr = s.mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        failed = true;
        return false;
    }
    while (true) {
        hr = s.Output(width, height, bgra);
        if (hr == S_OK) {
            got = true;
            continue;
        }
        if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) failed = true;
        break;
    }
    return got;
}

} // namespace openshao::stream

#else

namespace openshao::stream {

bool StartMediaFoundation() { return false; }

struct VideoEncoder::State {};
VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() = default;
bool VideoEncoder::Encode(const uint8_t*, uint32_t, uint32_t, bool, std::vector<uint8_t>&, bool&) { return false; }
void VideoEncoder::SetBitrate(uint32_t) {}

struct VideoDecoder::State {};
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::Decode(const uint8_t*, size_t, uint32_t, uint32_t, std::vector<uint8_t>&, bool& failed) {
    failed = true;
    return false;
}
void VideoDecoder::Reset() {}

} // namespace openshao::stream

#endif
