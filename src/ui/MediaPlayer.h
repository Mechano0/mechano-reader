#pragma once
// Video / audio preview with Windows Media Foundation (MFPlay): the same
// engine and codecs the Windows "Media Player" app uses. Nothing to ship.
// UI thread only: MFPlay delivers its events on the thread that created it.
#include <windows.h>
#include <string>

struct IMFPMediaPlayer;

namespace mzr::ui {

class MediaPlayer {
public:
    // notifyWnd receives `notifyMsg` whenever the state changes (playing,
    // paused, ended, error) so the UI can refresh its buttons.
    MediaPlayer(HWND notifyWnd, UINT notifyMsg);
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // videoWnd: the child window the picture is drawn into (nullptr for audio).
    // Starts playing, muted (like a preview, it never surprises with sound).
    // Media Foundation is loaded on the first Open (delay-loaded DLLs, faster app start).
    static void ShutdownEngine();
    bool Open(const std::wstring& file, HWND videoWnd);
    void Close();

    void TogglePlay();
    void ToggleMute();
    void RepaintVideo();             // call from the video window's WM_PAINT / WM_SIZE

    // Seconds. 0 until the file is ready.
    double Duration() const;
    double Position() const;
    void Seek(double seconds);       // clamped to 0..Duration(); keeps play / pause state
    void Stop();                     // pause and go back to the start
    float Volume() const;            // 0..1
    void SetVolume(float v);         // also turns sound on when v > 0
    void StartWithSound(float v) { startVolume_ = v; }   // next Open: play with sound at v (⏮ / ⏭ keep the sound on)

    bool IsOpen() const { return player_ != nullptr; }
    bool IsPlaying() const { return playing_; }
    bool IsMuted() const { return muted_; }
    bool HasError() const { return !error_.empty(); }
    const std::wstring& Error() const { return error_; }

    // Called by the MFPlay callback (internal).
    void OnEvent(void* header);

private:
    class Callback;
    HWND notify_;
    UINT msg_;
    IMFPMediaPlayer* player_ = nullptr;
    Callback* callback_ = nullptr;
    bool playing_ = false, muted_ = true, ended_ = false;
    float startVolume_ = -1;
    std::wstring error_;
};

} // namespace mzr::ui
