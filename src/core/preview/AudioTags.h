#pragma once
// Song details for the audio preview (AIMP-style panel): title, artist, album, year, genre, sample rate,
// bit rate, channels and the embedded cover picture. Read with the Windows property system (the same
// built-in handlers File Explorer uses for MP3 / WMA / M4A / FLAC ...); the cover goes through our own
// bounded image decoder. Every field is optional: a file with no tags just shows its name.
#include <cstdint>
#include <string>
#include "core/preview/PreviewDecode.h"

namespace mzr::preview {

struct AudioTags {
    std::wstring title, artist, album, year, genre;
    uint32_t sampleRate = 0;      // Hz
    uint32_t bitRate = 0;         // bits per second
    uint32_t channels = 0;
    Image cover;                  // empty if none (max 512 px per side)
};

// Reads a local file (the private temp copy). COM must be initialised on the calling thread.
AudioTags ReadAudioTags(const std::wstring& path);

// "MP3  ::  44.1 kHz, 128 kbps, stereo" (only the parts that are known).
std::wstring AudioFormatLine(const AudioTags& t, const std::wstring& fileName);

} // namespace mzr::preview
