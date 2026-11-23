# Native Apple Silicon build

This experimental macOS target uses the SDL host shared with Android. It is
separate from the Windows host and does not provide all of its features.

From the repository root:

```sh
brew install cmake ninja sdl3 ffmpeg
./build_macos.sh
./Launch-PSP2i.command
```

The build script builds the recompiler, generates C from your decrypted
EBOOT.BIN, and builds the
executable at build/macos/psp2i. Four compiler jobs limit peak memory use.
psprecomp (in this repository, not a submodule) already carries the Apple ARM64 fiber fix.

Keep EBOOT.BIN and game.iso in the repository root. The launcher sets PSP2I_DATA
and the working directory to that folder. psp2i.ini contains:

```ini
iso=game.iso
fps=30
touch=off
```

Place your PSP fonts or compatible replacement fonts in GameData/flash/font.
Saves go under
GameData/ms; the runtime log is psp2i_log.txt in the repository root.

Keyboard controls (click the game window first): PPSSPP's default layout, listed in the main README under "Controls".

| Keys | PSP control |
| --- | --- |
| Arrow keys | D-pad |
| I / J / K / L | Analog up / left / down / right |
| X / Z | Circle (confirm) / Cross (cancel) |
| A / S | Square / Triangle |
| Q / W | L / R |
| Space / V (or Enter) | Start / Select |

SDL-compatible controllers are also supported, including right-stick camera.
Close the game window to quit.

Movie video uses the system FFmpeg libraries (libavcodec, libavutil, and libswscale).
The runtime supplies H.264 access units; the host returns decoded YUV420P
frames with their presentation timestamps. Decoder buffers are drained at EOF.
To build without video decoding, configure with `-DPSP2I_LIBAV=OFF`; movies
then retain the previous black-video behavior. Windows continues to use its
existing OpenH264 backend. Android does not enable this optional backend.

Build and run the standalone decoder comparison (no game code executed):

```sh
cmake --build build/macos --target video_decode
python3 port/tests/check_video_decode.py build/macos/video_decode
# Optionally compare a legally obtained PMF too:
python3 port/tests/check_video_decode.py build/macos/video_decode /path/to/movie.pmf
```

The test generates 90 frames with B-frame reordering and compares all decoded
YUV pixels and frame counts against FFmpeg, including EOF drain. The probe
feeds Annex-B input through a parser in 2,048-byte chunks. It also accepts an
optional PMF, extracting its video with FFmpeg before the comparison. This
checks the codec adapter; it does not independently validate the runtime's
PSMF demultiplexer, presentation timing, or audio/video synchronization.

The decoder approach was informed by
[kion/psp2i-recomp](https://psp2i.dev/kion/psp2i-recomp), revision
`e7d140fde6836e2233028b160e2e47792074c179`, especially its FFmpeg decoder and
frame-comparison harness. This implementation uses psprecomp's existing codec
interface and demultiplexer.

This SDL host still lacks the Windows settings menu. Online and ad hoc play are
not provided, and saves are written unencrypted (as on Windows).

Full-range cutscenes (YUVJ420P, including the Continue path) are normalized to
studio-range YUV420P before the runtime's BT.601 color conversion. The decoder
comparison also covers 90 full-range frames with B-frame reordering.
