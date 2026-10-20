# Third-party notices

NASBridge is Copyright (C) 2026 NASBridge contributors and licensed under
GPL-3.0-or-later (see [LICENSE](LICENSE)). It is built on the components
below, which keep their own copyrights and licences.

## ps5-homebrew-ui (the UI kit)

NASBridge's interface is built with ps5-homebrew-ui, Copyright (C) 2026
BlackBearReloaded, licensed under GPL-3.0-or-later. That's the same licence as
NASBridge, so the two combine without restriction.

- **Source:** [J7mbo/ps5-homebrew-ui](https://github.com/J7mbo/ps5-homebrew-ui).
  The original repository (blackbearreloaded/ps5-homebrew-ui) is no longer
  public, so this copy keeps its source available, with its full history.
- **Version:** commit `0f5742db7d5b767f58a1eb62544f5e43b7c411d2`, pinned in
  the `Dockerfile`: the original's commit `bbe19054`, plus one change that
  fetches the ps5-opengl SDK from the copy below.
- **How it's used:** the app is built from a copy of the kit, with NASBridge's
  screens (`app/screens/`) in place of its demo designs. The kit's renderer,
  components, Acrylic theme, sound mixer and native build pipeline are used as
  they are.
- **Modified kit files:** two files in `app/overrides/` replace the kit's
  versions. Each one says at the top what was changed:
  - `src/app/shell.cpp`: no design switcher for a single-screen app.
  - `src/runtime/app_heap.c`: a 256 MiB heap instead of 128 MiB.

The kit brings in the components below, which end up in NASBridge's app. The
kit's own `THIRD_PARTY_NOTICES.md` covers them in full.

### Fonts

The licence texts ship beside the fonts, in the app's `assets/fonts/`.

| Typeface | Copyright | Licence |
| --- | --- | --- |
| Inter | (c) 2016 The Inter Project Authors | SIL Open Font License 1.1 |
| Montserrat | (c) 2011 The Montserrat Project Authors | SIL Open Font License 1.1 |
| DejaVu Sans Mono | (c) 2003 Bitstream, Inc.; DejaVu changes are public domain | Bitstream Vera licence |
| Press Start 2P | (c) 2012 The Press Start 2P Project Authors | SIL Open Font License 1.1 |
| Patrick Hand | (c) 2010-2012 Patrick Wagesreiter | SIL Open Font License 1.1 |

### Sound effects

The interface sounds (`assets/audio/sfx/glass/`) are ProsperoEden's, Copyright
(C) 2026 BlackBearReloaded, distributed under the kit's licence
(GPL-3.0-or-later).

### OpenGL runtime

The app statically links the
ps5-opengl SDK 1.0.2 (source and compiled SDK:
[J7mbo/ps5-opengl](https://github.com/J7mbo/ps5-opengl), a copy of the no
longer public original), Copyright
(C) 2026 BlackBearReloaded, GPL-3.0-or-later. It contains
[Mesa](https://mesa3d.org/) (MIT) and OpenGNM PSBC components under their own
licences.

### Native app pipeline

From ps5-native-app-boilerplate (Copyright (C) 2026 BlackBearReloaded,
GPL-3.0-or-later), by way of the kit, whose source includes this code:

- The `eboot.bin` converter is derived from
  [SharpProspero](https://github.com/SvenGDK/SharpProspero), Copyright (C)
  2026 SvenGDK, GPL-3.0.
- The app's `sce_module/libc.prx` is the boilerplate's independently written
  runtime shim (GPL-3.0-or-later). It contains no Sony code.

### Other code in the kit

- [stb_vorbis](https://github.com/nothings/stb) decodes music (public domain
  or MIT).
- The shape shader follows the 2D signed-distance functions published by
  [Inigo Quilez](https://iquilezles.org/articles/distfunctions2d/) (MIT).

## Added by NASBridge

- **[stb_image](https://github.com/nothings/stb)** decodes game covers (public
  domain or MIT). Its licence is copied into the build with it.
- **Selection music** (`app/sce_sys/snd0.at9`): an excerpt of "First Light"
  from the ps5-homebrew-ui kit, Copyright (C) 2026 BlackBearReloaded,
  GPL-3.0-or-later. It was converted by `tools/make_music.sh` with
  BlackBearReloaded's ps5-at9-converter (GPL-3.0-or-later), which isn't part
  of what is installed.
- **Home-screen artwork** (`app/sce_sys/icon0.png`, `pic0.dds`, `pic1.dds`) is
  NASBridge's own. The backgrounds are rendered by `tools/make_background.py`.

## PS5 payload SDK

The app, the installer and the copy and delete helper payloads are built with
the public [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). Its
startup code (GPL-3.0) is linked into each of them. Its libc++ headers keep
their Apache-2.0 WITH LLVM-exception licence.

## Build and test tools

The toolchain container builds LLVM's compiler-rt, the payload SDK and the
kit from source, and the kit's build tools use [zlib](https://zlib.net/) 1.3.2
(zlib licence). None of these are part of what is installed on the PS5. The
tests' fake NAS and fake PS5 loader (`tests/`) are NASBridge's own.

## Disclaimer

NASBridge is an independent homebrew project. It isn't affiliated with,
endorsed by or sponsored by Sony Interactive Entertainment. "PlayStation" and
"PS5" are trademarks of Sony Interactive Entertainment Inc. No Sony SDK,
firmware, encryption key or system module is included.
