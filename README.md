# kms-mattress

Minimal legacy DRM/KMS demonstration in C. Draws eight vertical color bars into a 32-bit dumb buffer and displays it on a connected HDMI-A output. No SDL, EGL, OpenGL, X11 or Wayland required.

## Dependencies (Debian)

```sh
sudo apt install build-essential cmake pkg-config libdrm-dev
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

Run from a text VT with no compositor controlling the selected DRM device:

```sh
sudo ./build/kms-mattress /dev/dri/card0
```

If the KMS card has another number, pass its actual `/dev/dri/cardN` path. Press Enter to exit.

## Limits

- Uses the legacy KMS API and assumes the device supports dumb buffers and 32-bit XRGB8888 framebuffers.
- Picks the first connected HDMI-A connector, its preferred mode and a compatible CRTC; does not manage multi-display configurations.
- Restores the original CRTC framebuffer/mode on normal exit, but does not save all connector routing or atomic plane state. SIGKILL/crashes cannot restore the display.
- Needs DRM master access; stop the display manager/compositor before running.
