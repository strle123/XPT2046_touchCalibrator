# XPT2046_touchCalibrator

Touchscreen calibration tool for XPT2046 controllers.

This is a learning / hobby project and is still being tested. I tested the basic flows on my setup(s), but you should expect that some configurations may need tweaks (different displays, rotations, kernels, SPI wiring, etc.). PRs and issue reports are welcome.

## Status / expectations

- Tested in a limited way on Raspberry Pi setup(s).
- Calibration is intentionally blocked until you run the installer (see the install marker below).
- The optional uinput daemon is meant to provide system-wide touch events for normal Linux apps, but it may behave differently across kernels/distros.

## Known limitations / might need tweaks

- Display rotation / coordinate mapping (depends on your display stack).
- Different XPT2046 wiring / different SPI device nodes.
- Different kernels/distros (uinput permissions and device naming can vary).

## Quick start (Raspberry Pi)

- Clone/copy this repo to your Pi (any location).
- Run the installer:
	- `bash ./xpt-install.sh`
	- or `cd installation && bash ./install.sh`

The installer creates an install marker at `/etc/xpt2046/installed.marker`. Calibration is only allowed after installation.

Note: the installer does not enable SSH or other remote access features.

## Calibrate

- `cd installation && bash ./xpt-calibrate.sh`

This wrapper will stop your configured GUI service (if configured), stop `xpt-uinputd.service` if present, run the calibration wizard, then restore services.

Basic GUI test binary (SDL2): `xpt_basic_gui_sdl2`

If you exit without saving, it should not modify `/etc/xpt2046/touch_config.txt` (please report if you ever see it change).

## System-wide touch (uinput service)

To provide touch events to normal Linux apps, install the uinput daemon as a systemd service:

- `cd installation && bash ./install_uinput_service.sh`

The service uses `/etc/xpt2046/touch_config.txt` (via `TOUCH_CONFIG_PATH`).

If touch behaves oddly after installing the service, first confirm the config file looks reasonable and try re-running calibration.

## Uninstall

- `cd installation && bash ./uninstall.sh`

Uninstall always removes the install marker so calibration can no longer be used. It can optionally keep `xpt-uinputd.service` and the config file so existing apps keep working.

## What should be tested more

- More Raspberry Pi OS / Debian versions.
- Different display rotations and framebuffer/GUI stacks.
- Long-running stability of `xpt-uinputd.service` (suspend/resume, hotplug, etc.).

## Configuration

Config file is a simple `key=value` format. Common keys include:

- `spi_device=/dev/spidev0.1`
- `invert_x=0|1`, `invert_y=0|1`, `swap_xy=0|1`
- `min_x`, `max_x`, `min_y`, `max_y`
- Advanced keys used by the calibrator/uinput daemon (screen size, deadzones, filters, thresholds)

You can override the config path for the binaries with `TOUCH_CONFIG_PATH=/path/to/touch_config.txt`.
