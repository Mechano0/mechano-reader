#include "ui/MediaPlayer.h"

#include <mfapi.h>
#include <mfplay.h>

#include <algorithm>
#include <atomic>

#include "utils/Log.h"

namespace mzr::ui {

class MediaPlayer::Callback final : public IMFPMediaPlayerCallback {
public:
    explicit Callback(MediaPlayer* owner) : owner_(owner) {}
    void Detach() { owner_ = nullptr; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFPMediaPlayerCallback)) {
            *out = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* h) override {
        if (owner_ && h) owner_->OnEvent(h);
    }
private:
    std::atomic<ULONG> ref_{1};
    MediaPlayer* owner_;
};

namespace {
// 0 = not tried, 1 = running, -1 = not available (Windows "N" edition without the Media Feature Pack).
int g_engine = 0;

bool EnsureEngine() {
    if (g_engine == 0) {
        // mfplat / mfplay are delay-loaded: check they exist BEFORE the first call,
        // otherwise a missing DLL would end the program instead of just this preview.
        const bool present = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
                             LoadLibraryExW(L"mfplay.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g_engine = present && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)) ? 1 : -1;
        if (g_engine < 0) log::Info(L"Media preview: Media Foundation is not available on this Windows.");
    }
    return g_engine == 1;
}
} // namespace

void MediaPlayer::ShutdownEngine() {
    if (g_engine == 1) MFShutdown();
    g_engine = 0;
}

MediaPlayer::MediaPlayer(HWND notifyWnd, UINT notifyMsg) : notify_(notifyWnd), msg_(notifyMsg) {}
MediaPlayer::~MediaPlayer() { Close(); }

bool MediaPlayer::Open(const std::wstring& file, HWND videoWnd) {
    Close();
    error_.clear();
    if (!EnsureEngine()) {
        error_ = L"Video and audio preview needs the Windows Media Feature Pack (missing on this Windows edition).";
        return false;
    }
    callback_ = new Callback(this);
    HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_NONE, callback_, videoWnd, &player_);
    if (SUCCEEDED(hr)) hr = player_->CreateMediaItemFromURL(file.c_str(), FALSE, 0, nullptr);   // async
    if (FAILED(hr)) {
        wchar_t b[64];
        swprintf_s(b, L"MFPlay error 0x%08X", static_cast<unsigned>(hr));
        log::Info(std::wstring(L"Media preview: ") + b);
        error_ = L"Windows cannot play this file (the codec may be missing).";
        Close();
        return false;
    }
    return true;
}

void MediaPlayer::Close() {
    if (player_) {
        player_->Shutdown();     // stops playback and releases the file
        player_->Release();
        player_ = nullptr;
    }
    if (callback_) { callback_->Detach(); callback_->Release(); callback_ = nullptr; }
    playing_ = false;
    ended_ = false;
    muted_ = true;
}

void MediaPlayer::TogglePlay() {
    if (!player_) return;
    if (playing_) {
        player_->Pause();
    } else {
        if (ended_) {
            PROPVARIANT start;
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = 0;
            player_->SetPosition(MFP_POSITIONTYPE_100NS, &start);
            ended_ = false;
        }
        player_->Play();
    }
}

void MediaPlayer::ToggleMute() {
    if (!player_) return;
    if (SUCCEEDED(player_->SetMute(muted_ ? FALSE : TRUE))) muted_ = !muted_;
    PostMessageW(notify_, msg_, 0, 0);
}

void MediaPlayer::RepaintVideo() {
    if (player_) player_->UpdateVideo();
}

double MediaPlayer::Duration() const {
    if (!player_) return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(player_->GetDuration(MFP_POSITIONTYPE_100NS, &v))) {
        if (v.vt == VT_UI8) s = static_cast<double>(v.uhVal.QuadPart) / 1e7;
        else if (v.vt == VT_I8) s = static_cast<double>(v.hVal.QuadPart) / 1e7;
    }
    PropVariantClear(&v);
    return s > 0 ? s : 0;
}

double MediaPlayer::Position() const {
    if (!player_) return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(player_->GetPosition(MFP_POSITIONTYPE_100NS, &v))) {
        if (v.vt == VT_I8) s = static_cast<double>(v.hVal.QuadPart) / 1e7;
        else if (v.vt == VT_UI8) s = static_cast<double>(v.uhVal.QuadPart) / 1e7;
    }
    PropVariantClear(&v);
    return s > 0 ? s : 0;
}

void MediaPlayer::Seek(double seconds) {
    if (!player_) return;
    const double d = Duration();
    if (d <= 0) return;
    seconds = std::clamp(seconds, 0.0, d);
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = static_cast<LONGLONG>(seconds * 1e7);
    if (SUCCEEDED(player_->SetPosition(MFP_POSITIONTYPE_100NS, &v)) && seconds < d - 0.05) ended_ = false;
    PostMessageW(notify_, msg_, 0, 0);
}

void MediaPlayer::Stop() {
    if (!player_) return;
    // MFPlay's own Stop: playback ends and the next Play starts from the beginning.
    // (Pause + SetPosition(0) back to back races the async pause and fails.)
    player_->Stop();
    playing_ = false;
    ended_ = false;
    PostMessageW(notify_, msg_, 0, 0);
}

float MediaPlayer::Volume() const {
    float v = 1.0f;
    if (player_ && FAILED(player_->GetVolume(&v))) v = 1.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

void MediaPlayer::SetVolume(float v) {
    if (!player_) return;
    v = std::clamp(v, 0.0f, 1.0f);
    player_->SetVolume(v);
    if (v > 0 && muted_ && SUCCEEDED(player_->SetMute(FALSE))) muted_ = false;
    PostMessageW(notify_, msg_, 0, 0);
}

void MediaPlayer::OnEvent(void* p) {
    auto* h = static_cast<MFP_EVENT_HEADER*>(p);
    if (FAILED(h->hrEvent)) {
        wchar_t b[64];
        swprintf_s(b, L"MFPlay event %d error 0x%08X", static_cast<int>(h->eEventType), static_cast<unsigned>(h->hrEvent));
        log::Info(std::wstring(L"Media preview: ") + b);
        // A refused pause / stop / seek is not a broken file: keep the preview.
        if (h->eEventType == MFP_EVENT_TYPE_PAUSE || h->eEventType == MFP_EVENT_TYPE_STOP ||
            h->eEventType == MFP_EVENT_TYPE_POSITION_SET) {
            PostMessageW(notify_, msg_, 0, 0);
            return;
        }
        error_ = L"Windows cannot play this file (unsupported format, missing codec, or damaged file).";
        playing_ = false;
        PostMessageW(notify_, msg_, 0, 0);
        return;
    }
    switch (h->eEventType) {
    case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
        auto* e = MFP_GET_MEDIAITEM_CREATED_EVENT(h);
        if (player_ && e->pMediaItem) player_->SetMediaItem(e->pMediaItem);
        break;
    }
    case MFP_EVENT_TYPE_MEDIAITEM_SET:
        if (player_) {
            if (startVolume_ > 0) {                  // the user had the sound on (⏮ / ⏭ in the song panel)
                player_->SetVolume(std::clamp(startVolume_, 0.0f, 1.0f));
                muted_ = FAILED(player_->SetMute(FALSE));
            } else {
                player_->SetMute(TRUE);              // a preview never surprises with sound
                muted_ = true;
            }
            startVolume_ = -1;
            player_->Play();
        }
        break;
    case MFP_EVENT_TYPE_PLAY: playing_ = true; ended_ = false; break;
    case MFP_EVENT_TYPE_PAUSE:
    case MFP_EVENT_TYPE_STOP: playing_ = false; break;
    case MFP_EVENT_TYPE_PLAYBACK_ENDED: playing_ = false; ended_ = true; break;
    default: break;
    }
    PostMessageW(notify_, msg_, 0, 0);
}

} // namespace mzr::ui
