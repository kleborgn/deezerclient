# Deezer Desktop Client

A native Windows desktop client for Deezer built with C++ and Qt, featuring gapless playback via the BASS audio library.

## Features

- **Gapless Playback** — Seamless track transitions using BassMix queue mode
- **Full-Quality Streaming** — FLAC, MP3 320, AAC support with BF_CBC_STRIPE decryption
- **Native Performance** — C++/Qt, not Electron
- **Now Playing View** — Large album art, track list with play indicator, compact player bar with waveform (like SoundCloud)
- **Last.fm Scrobbling** — Track scrobble counts displayed inline
- **Discord Rich Presence** — Show what you're listening to
- **Synced Lyrics** — Real-time lyrics display
- **Favorites** — Heart toggle to add/remove tracks from your Deezer favorites
- **Audio Visualizations** — Spectrum and projectM visualizer

## Prerequisites

1. **CMake** 3.16+
2. **Qt 6** (6.2+) — Widgets, Network, OpenGL, OpenGLWidgets
3. **BASS & BassMix** — https://www.un4seen.com/bass.html
4. **Visual Studio 2019/2022** or MinGW-w64
5. **OpenSSL** (optional) — For email/password login token decryption (`-DDEEZER_OPENSSL=ON`)

## Setup

### 1. Clone the repository

```bash
git clone <repository-url>
cd DeezerClient
```

### 2. Create `src/secrets.h`

Copy the example and fill in your keys:

```bash
cp src/secrets.h.example src/secrets.h
```

Edit `src/secrets.h` with your Deezer API keys:

```cpp
#define DEEZER_MOBILE_API_KEY "your_64_char_api_key"
#define DEEZER_MOBILE_GW_KEY  "your_16_char_gw_key"
#define DEEZER_TRACK_XOR_KEY  "your_16_char_xor_key"
```

These keys can be found by decompiling the Deezer Android APK with a tool like `jadx`. The file is gitignored and will not be committed.

### 3. Install BASS Audio Library

Download BASS and BassMix from https://www.un4seen.com/ and place them in:

```
external/
├── bass/
│   ├── bass.h
│   ├── bass.lib
│   └── bass.dll
└── bassmix/
    ├── bassmix.h
    ├── bassmix.lib
    └── bassmix.dll
```

### 4. Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The executable will be at `build/bin/Release/DeezerClient.exe`.

## Authentication

### ARL Cookie (Recommended)

1. Log in to https://www.deezer.com in your browser
2. Open DevTools (F12) → Application → Cookies → `https://www.deezer.com`
3. Copy the `arl` cookie value
4. Paste it into the app's login dialog

### Email & Password

Requires `DEEZER_MOBILE_API_KEY` and `DEEZER_MOBILE_GW_KEY` in `secrets.h`, and building with `-DDEEZER_OPENSSL=ON`.

## Architecture

```
MainWindow
├── Library Tabs (Playlists, Albums, Search)
├── Now Playing Tab
│   ├── Album Art (AspectRatioLabel)
│   ├── Queue Header (album info, stream format, scrobble count)
│   └── Queue (TrackListWidget in QueueMode)
├── PlayerControls (transport, volume, waveform)
├── AudioEngine (BASS/BassMix, gapless preloading, decryption)
└── DeezerAPI (Mobile Gateway + Web Gateway)
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `bass.dll` not found | Ensure BASS DLLs are in the executable directory |
| `Qt6Core.dll` not found | Run `windeployqt DeezerClient.exe` or add Qt bin to PATH |
| Build can't find Qt | Set `CMAKE_PREFIX_PATH` to your Qt install, e.g. `C:\Qt\6.8.0\msvc2022_64` |
| `secrets.h` not found | Copy `src/secrets.h.example` to `src/secrets.h` and fill in your keys |
| Corrupted audio | Check that `DEEZER_TRACK_XOR_KEY` is correct in `secrets.h` |

## License

- **BASS Audio Library** — Free for non-commercial use (https://www.un4seen.com/)
- **Qt** — LGPL
