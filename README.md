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
| Win+F10 | FTP access to the disk images; Esc restarts the PC |

## Configuration

`386/config.ini` uses the murm386 format: `[pc]` / `[386]` (`bios`, `vga_bios`, `fda`, `fdb`, `hda`..`hdd`, `cda`..`cdd`, `mem_size` in MiB, default 16), `[cpu]` (`gen`, `fpu`) and `[murm-286]` (sound devices, mouse options). Paths are resolved below the working directory, case-insensitively.

`bios=native` (or an empty `bios=`) selects the built-in native BIOS instead of a ROM image: POST, BIOS data area and the INT 10h/13h/16h/1Ah/33h services run as host code. With `native_dos=1` (`[murm-286]`, default; Disk Manager row BOOT) it then starts the built-in FreeDOS kernel and FreeCOM shell, with A: (or C:) as the boot drive. With `native_dos=0` it boots the guest from A:, then C:, then a bootable CD.

Native ARM applications (ELF/EZ executables built for the RP2350) are not supported: they are Cortex-M33 Thumb code, and EXEC refuses them.

`[cpu] gen` (also Settings, Win+F11, "CPU Type") selects the CPU core: 0..2 (8086, 80186, 80286) run the real-mode 286 core of murm386's 286 build, 3..5 the 386 core. A change takes effect after the restart the Settings page offers. SeaBIOS is 32-bit code and does not boot on the 286 core: use `bios=native` (or an 8086/286 BIOS image) there.

The native BIOS can also present a whole disk as a raw BIOS hard disk, as murm386 does with its SD card: `raw_disk=/dev/sdX` in `[pc]` / `[386]` names a block device (typically a USB flash drive with an MBR) or a disk image file, and `raw_sd_hdd` places it after (1, default) or before (2) the ATA disks, or hides it (0); the Disk Manager row RAWDISK sets the same. A block device that is mounted on the host is refused, and the drive is read-only if the device or file is. Like on the RP2350, it is only reachable through the native INT 13h, not through IDE/SeaBIOS.

## FTP access to the disk images

Win+F10 is the counterpart of murm386's USB DEVICE mode. It pauses the PC and starts an FTP server that serves the FAT12/16/32 volumes inside the attached floppy and hard disk images (and the raw disk): each one is a top-level directory named after its Disk Manager row (`/FDD-0`, `/ATA0-0`, `/RAWDISK`, ...). CD-ROM images are not served. Esc stops the server and restarts the emulated PC, so the guest never works on data it cached before the change.

The page shows the address to connect to. Settings in `[murm-286]`: `ftp_port` (default 2121), `ftp_user` and `ftp_password` (both default `dos`). One client at a time; passive (PASV/EPSV) and active (PORT) transfers; file names are UTF-8 (long names, code page 866 for short names); transfers are always binary. The volumes are opened with an unmodified copy of ChaN's FatFs R0.16 (`src/imgfs`).

## Not ported yet

Game controllers other than mouse-as-joystick and the numeric-keypad mouse.

## Limits

- Uses the legacy KMS API and assumes the device supports dumb buffers and 32-bit XRGB8888 framebuffers.
- Picks the first connected HDMI-A connector and the mode that holds the 640x480 VGA raster at the largest integer scale; does not manage multi-display configurations.
- Restores the original CRTC framebuffer/mode on normal exit, but does not save all connector routing or atomic plane state. SIGKILL/crashes cannot restore the display.
- Needs DRM master access; stop the display manager/compositor before running.
