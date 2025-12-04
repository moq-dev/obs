# OBS MoQ Plugin

A plugin for OBS Studio that adds support for Media over QUIC (MoQ) streaming.

## Building

### macOS

```bash
cmake --preset macos
cmake --build --preset macos
```

The plugin will be built to `build_macos/RelWithDebInfo/obs-moq.plugin`.

### Development

To build against a local [moq](https://github.com/kixelated/moq) checkout:

```bash
cmake --preset macos -DMOQ_LOCAL=../moq
cmake --build --preset macos
```

## Installation

Copy the plugin to your OBS plugins directory:

```bash
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ~/Library/Application\ Support/obs-studio/plugins/

```

And if you're building against a local obs-studio checkout:
```bash
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/PlugIns/
RUST_LOG=debug RUST_BACKTRACE=1 OBS_LOG_LEVEL=debug ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/MacOS/OBS
```
