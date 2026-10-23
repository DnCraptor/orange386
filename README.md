# orange386

Port of the [murm386](https://github.com/DnCraptor/murm386) x86 PC emulator (RP2350) to ARM64 Linux (Orange Pi Zero 2W): video through legacy DRM/KMS, sound through ALSA, keyboard and mouse through evdev. No SDL, EGL, OpenGL, X11 or Wayland required.

## Dependencies (Debian)

```sh
sudo apt install build-essential cmake pkg-config libdrm-dev libasound2-dev
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

Run from a text VT with no compositor controlling the selected DRM device. The working directory must contain the `386/` data directory (BIOS images, `config.ini`, disk images); the build copies it next to the executable.

```sh
cd build
sudo AUDIODEV=plughw:CARD=HDMI,DEV=0 ./orange386 /dev/dri/card0
```

If the KMS card has another number, pass its actual `/dev/dri/cardN` path. Without `AUDIODEV` the ALSA `default` device is used.

Keyboards and mice are grabbed exclusively while the emulator runs.

| Keys | Action |
|---|---|
| Ctrl+Alt+Del | Reboot the emulated PC (restarts the emulator process) |
| Ctrl+Alt+End | Quit (also Ctrl+C / SIGTERM from a terminal) |
| Win+F12 | Disk Manager: change floppy/ATA images and the BIOS image |
| Win+F11 | Settings: volume, video adapter, CPU, sound devices, mouse |

## Configuration

`386/config.ini` uses the murm386 format: `[pc]` / `[386]` (`bios`, `vga_bios`, `fda`, `fdb`, `hda`..`hdd`, `cda`..`cdd`, `mem_size` in MiB, default 16), `[cpu]` (`gen`, `fpu`) and `[murm-286]` (sound devices, mouse options). Paths are resolved below the working directory, case-insensitively.

`bios=native` (or an empty `bios=`) selects the built-in native BIOS instead of a ROM image: POST, BIOS data area and the INT 10h/13h/16h/1Ah/33h services run as host code. With `native_dos=1` (`[murm-286]`, default; Disk Manager row BOOT) it then starts the built-in FreeDOS kernel and FreeCOM shell, with A: (or C:) as the boot drive. With `native_dos=0` it boots the guest from A:, then C:, then a bootable CD.

Native ARM applications (ELF/EZ executables built for the RP2350) are not supported: they are Cortex-M33 Thumb code, and EXEC refuses them.

## Not ported yet

Game controllers other than mouse-as-joystick and the numeric-keypad mouse.

## Limits

- Uses the legacy KMS API and assumes the device supports dumb buffers and 32-bit XRGB8888 framebuffers.
- Picks the first connected HDMI-A connector and the mode that holds the 640x480 VGA raster at the largest integer scale; does not manage multi-display configurations.
- Restores the original CRTC framebuffer/mode on normal exit, but does not save all connector routing or atomic plane state. SIGKILL/crashes cannot restore the display.
- Needs DRM master access; stop the display manager/compositor before running.
