# You Are Idiot Native

Native Windows C++ version of the browser prank. It uses one Win32 process, Media Foundation frame decoding, direct GDI painting, timer-driven window motion, capped clone windows, topmost mode, and looped PCM audio.

It does not install persistence, modify the system, disable tools, or write outside the process. Press `Esc` in any prank window, or `Ctrl+Shift+Q` from anywhere, to close everything.

## Build on Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\you_are_idiot.exe
```

## Build with MinGW-w64

```sh
cmake -S . -B build-mingw -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
./build-mingw/you_are_idiot.exe
```

## Runtime options

```text
--windows N    Number of prank windows, clamped to 1..24. Default: 10
--fps N        Animation timer rate, clamped to 15..240. Default: 120
--spawn-ms N   Delay between clone windows, clamped to 250..10000. Default: 700
--media PATH   MP4 file to decode. Default: media\youare.mp4
--audio PATH   WAV file to loop. Default: media\youare.wav
--no-topmost   Do not keep prank windows above other windows.
--no-sound     Disable audio playback.
--no-dodge     Disable cursor dodge behavior.
--calm         Lower-impact profile for testing.
--help         Show option help.
```
