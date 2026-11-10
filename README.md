# crate

A terminal music player.

## Dependencies (Arch)

```bash
# needed to build
sudo pacman -S --needed base-devel cmake git pkgconf taglib libpulse

# needed at runtime
sudo pacman -S --needed mpv yt-dlp ffmpeg wl-clipboard mpv-mpris
```

- `mpv` plays the audio.
- `yt-dlp` and `ffmpeg` handle downloads.
- `wl-clipboard` provides `wl-paste`, used for pasting.
- `mpv-mpris` lets the keyboard's play/pause key (and other desktop media controls) control crate. It's optional, and does nothing on Android/Termux.
- `libpulse` lets the visualizer record what's playing (works with PipeWire too). It's optional: without it crate still builds, minus the visualizer.

FTXUI (the TUI library) isn't installed with pacman. CMake downloads it the first time you configure, which needs an internet connection.

## Building from scratch

From the repo root, with no `build/` folder yet:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release   # configure: creates build/ and fetches FTXUI
cmake --build build -j                      # compile: produces build/crate
```

After that, rebuilding after changes only takes the second command:

```bash
cmake --build build -j
```

To start completely fresh (for example, after changing `CMakeLists.txt` causes trouble), delete the folder and run both commands again:

```bash
rm -rf build
```

## Putting it on your PATH

Symlink the built binary into `~/.local/bin` (which is on PATH):

```bash
ln -s ~/Projects/crate/build/crate ~/.local/bin/crate
```

The order is `ln -s TARGET LINK_NAME`: the real file comes first, then the link to create. The link points at `build/crate`, so every rebuild is picked up automatically, with no copying. If you delete `build/`, the link breaks until you build again.

Check it with `ls -l ~/.local/bin/crate`, and remove it with `rm ~/.local/bin/crate`.
