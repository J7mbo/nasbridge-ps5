#!/usr/bin/env bash
# NASBridge - Makes app/sce_sys/snd0.at9, the music the home screen plays while
# NASBridge is selected.
# Copyright (C) 2026 NASBridge contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The song is "First Light" from the ps5-homebrew-ui kit
# (assets/audio/music/first-light.ogg, Copyright (C) 2026 BlackBearReloaded,
# GPL-3.0-or-later like the kit). The converter is BlackBearReloaded's
# ps5-at9-converter (GPL-3.0-or-later), pinned below. It reads WAV, not OGG,
# so the song is decoded to WAV with ffmpeg first.
#
# snd0.at9 is committed, so a normal build doesn't need any of this. Run it
# again only to change the excerpt:
#
#   tools/make_music.sh path/to/first-light.ogg [start-seconds]
#
# Needs: git, python3 with numpy, ffmpeg.

set -euo pipefail

CONVERTER_REPO=https://github.com/blackbearreloaded/ps5-at9-converter
CONVERTER_COMMIT=65be0a1163ebdfa9399dafa35287afeb71b6afdf

song=${1:?usage: tools/make_music.sh first-light.ogg [start-seconds]}
start=${2:-0}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git clone -q "$CONVERTER_REPO" "$work/converter"
git -C "$work/converter" checkout -q "$CONVERTER_COMMIT"
test "$(git -C "$work/converter" rev-parse HEAD)" = "$CONVERTER_COMMIT"

# 48 kHz stereo 16-bit WAV, the converter's own working format.
ffmpeg -hide_banner -loglevel error -i "$song" -ar 48000 -ac 2 -c:a pcm_s16le "$work/song.wav"

# The longest excerpt that fits the 2 MiB limit, looped over the whole track;
# loudness and "loudnessSnd0" in param.json are set by the converter.
python3 "$work/converter/ps5_at9.py" \
  --input "$work/song.wav" \
  --output "$root/app/sce_sys/snd0.at9" \
  --start "$start" \
  --duration max \
  --param-json "$root/app/sce_sys/param.json"
