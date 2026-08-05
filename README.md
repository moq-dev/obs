> [!WARNING]
> **This repository is deprecated.** Development has moved to [moq-dev/moq](https://github.com/moq-dev/moq) under [`cpp/obs`](https://github.com/moq-dev/moq/tree/main/cpp/obs). Please use that repository instead.

# obs-moq

**obs-moq** is an [OBS Studio](https://obsproject.com/) plugin that enables streaming via the **Media over QUIC (MoQ)** protocol. This plugin allows you to publish live audio and video streams to a MoQ relay or server directly from OBS, and to consume MoQ broadcasts as an OBS source.

## Features

- **Protocol:** Media over QUIC (MoQ)
- **Video Codecs:** H.264, HEVC (H.265), AV1
- **Audio Codecs:** AAC, Opus
- **Low Latency:** Leverages QUIC for efficient and low-latency media transport.

## Where it lives now

The plugin is developed in the [moq-dev/moq](https://github.com/moq-dev/moq) monorepo under [`cpp/obs`](https://github.com/moq-dev/moq/tree/main/cpp/obs). It now loads into a **stock** OBS Studio install — the fork that this repository's build instructions required is no longer needed.

See the [OBS plugin documentation](https://doc.moq.dev/bin/obs) for current build, install, and usage instructions, including prebuilt releases.

## License

See the [LICENSE](LICENSE) file for details.
