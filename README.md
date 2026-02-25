# Deezer Desktop Client

A native Windows desktop client for Deezer built with C++ and Qt, featuring gapless playback via the BASS audio library.

## Features

- **Gapless Playback** — Seamless track transitions using BassMix queue mode with preloading
- **Progressive Streaming** — Audio starts playing before the full track downloads
- **Full-Quality Streaming** — FLAC, MP3 320, MP3 128 support with BF_CBC_STRIPE decryption
- **WASAPI Output** — Shared and Exclusive modes for bit-perfect playback
- **Native Performance** — C++/Qt, not Electron
- **Now Playing View** — Large album art, track list with play indicator, compact player bar with waveform
- **Waveform Display** — SoundCloud-style waveform with playback position indicator
- **Last.fm Scrobbling** — Track scrobble counts displayed inline, with offline scrobble cache
- **Discord Rich Presence** — Show what you're listening to
- **Synced Lyrics** — Real-time lyrics display in a dedicated window
- **Favorites** — Heart toggle to add/remove tracks from your Deezer favorites
- **Audio Visualizations** — Spectrum analyzer and projectM visualizer (Milkdrop presets)
- **Recently Played** — Quick access to recently played albums and playlists
- **Windows Media Controls** — System media transport controls integration
- **Audio Settings** — Output device selection (DirectSound, WASAPI Shared, WASAPI Exclusive)

## Prerequisites

1. **CMake** 3.16+
2. **Qt 6** (6.2+) — Core, Gui, Widgets, Network, Concurrent, OpenGLWidgets
3. **BASS, BassMix & BASSWASAPI** — https://www.un4seen.com/bass.html
4. **Visual Studio 2019/2022** (MSVC)
5. **OpenSSL** — Required for mobile API authentication (email/password login, token decryption)
6. **projectM** (optional) — For Milkdrop visualizations (https://github.com/projectM-visualizer/projectm)

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

### 3. Install External Libraries

Download from https://www.un4seen.com/ and place in `external/bass/`:

```
external/
├── bass/
│   ├── bass.h, bass.lib, bass.dll
│   ├── bassmix.h, bassmix.lib, bassmix.dll
│   └── basswasapi.h, basswasapi.lib, basswasapi.dll
└── projectm/ (optional)
    ├── libprojectM-4.lib, libprojectM_eval.lib
    ├── hlslparser.lib, glew32.lib, glew32.dll
    └── include/
```

### 4. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"
cmake --build build --config Release
```

The executable will be at `build/bin/Release/DeezerClient.exe`. Qt DLLs are deployed automatically via `windeployqt`.

## Authentication

### ARL Cookie (Recommended)

1. Log in to https://www.deezer.com in your browser
2. Open DevTools (F12) → Application → Cookies → `https://www.deezer.com`
3. Copy the `arl` cookie value
4. Paste it into the app's login dialog

### Email & Password

Requires `DEEZER_MOBILE_API_KEY` and `DEEZER_MOBILE_GW_KEY` in `secrets.h` and OpenSSL.

## Architecture

```
MainWindow
├── Library Tabs (Playlists, Albums, Search, Recent)
├── Now Playing Tab
│   ├── Album Art
│   ├── Queue Header (album info, stream format, scrobble count)
│   └── Queue (TrackListWidget)
├── PlayerControls (transport, volume, waveform, seek)
├── AudioEngine ─── split across 6 source files:
│   ├── audioengine.cpp           Core (init, shutdown, play/pause/stop/seek)
│   ├── audioengine_stream.cpp    Stream lifecycle, track loading, gapless transitions
│   ├── audioengine_progressive.cpp  Progressive download, decryption, preloading
│   ├── audioengine_queue.cpp     Queue management
│   ├── audioengine_output.cpp    WASAPI/DirectSound output, rate negotiation
│   └── audioengine_visualization.cpp  Position, spectrum, waveform computation
├── DeezerAPI (Mobile Gateway + Web Gateway)
├── Last.fm (scrobbling + scrobble cache)
├── DiscordManager (Rich Presence)
├── LyricsWindow (synced lyrics)
├── SpectrumWindow (spectrum analyzer)
├── ProjectMWindow (Milkdrop visualizer)
└── AudioSettingsDialog (output device selection)
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `bass.dll` not found | Ensure BASS DLLs are in the executable directory |
| `Qt6Core.dll` not found | Run `windeployqt DeezerClient.exe` or add Qt bin to PATH |
| Build can't find Qt | Set `CMAKE_PREFIX_PATH` to your Qt install, e.g. `C:\Qt\6.8.0\msvc2022_64` |
| Build can't find OpenSSL | Set `-DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"` |
| `secrets.h` not found | Copy `src/secrets.h.example` to `src/secrets.h` and fill in your keys |
| Corrupted audio | Check that `DEEZER_TRACK_XOR_KEY` is correct in `secrets.h` |
| Audio speed is wrong (WASAPI) | Try switching to WASAPI Shared or DirectSound in Audio Settings |

## License

- **BASS Audio Library** — Free for non-commercial use (https://www.un4seen.com/)
- **Qt** — LGPL
- **projectM** — LGPL
