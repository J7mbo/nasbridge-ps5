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
- **Modified kit files:** three files in `app/overrides/` replace the kit's
  versions. Each one says at the top what was changed:
  - `src/app/shell.cpp`: no design switcher for a single-screen app.
  - `src/runtime/app_heap.c`: a 256 MiB heap instead of 128 MiB.
  - `tools/build.sh`: accepts homebrew title IDs (`LAPY`, `FAKE`) as well as
    `PPSA`.

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
- **Reading `.ffpfsc` images** (`app/screens/pfs.cpp`, `ufs2.cpp`,
  `ffpfsc.cpp`) is NASBridge's own code. The PFS and PFSC layouts were learned
  from [MkPFS](https://github.com/PSBrew/MkPFS) (PSBrew) and
  [Dump Forge](https://github.com/quer3q/ps5-dump-forge) (quer3q), both
  GPL-3.0-or-later; UFS2's comes from FreeBSD's public headers. No code was
  copied from them. Its zlib blocks are inflated by stb_image's zlib decoder.
- **Home-screen artwork** (`app/sce_sys/icon0.png`, `pic0.dds`, `pic1.dds`) is
  NASBridge's own. The backgrounds are rendered by `tools/make_background.py`.

## libsmb2 (SMB)

The app's SMB support is built on
[libsmb2](https://github.com/sahlberg/libsmb2), Copyright (C) Ronnie
Sahlberg and contributors, licensed under LGPL-2.1-or-later, which combines
with NASBridge's GPL-3.0-or-later.

- **Version:** tag `libsmb2-6.2`, commit
  `d67e213a5c4e7e4969fd81f0b95e4ca5831fbba1`, pinned in the `Dockerfile`.
- **How it's used:** its `lib/` and `include/` sources are compiled, unchanged,
  into the app and into the copy helper payload (`copier.elf`), with
  NASBridge's own `config.h`, a small IPv4 address lookup and AES-128
  (`app/third_party/libsmb2/`). The AES (`aes_fast.c`, which uses the CPU's
  AES instructions) is built in place of libsmb2's `lib/aes.c`, which isn't
  used. Its `COPYING` and LGPL text are copied into the app with it.
- **MD4:** sign-in uses libsmb2's `lib/md4c.c`, the RSA Data Security, Inc.
  MD4 Message-Digest Algorithm (from RFC 1320), Copyright (C) 1990-2, RSA Data
  Security, Inc. Its licence notice is kept in that file.

## PS5 payload SDK

The app, the installer and the copy and delete helper payloads are built with
the public [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). Its
startup code (GPL-3.0) is linked into each of them. Its libc++ headers keep
their Apache-2.0 WITH LLVM-exception licence.

## Build and test tools

The toolchain container builds LLVM's compiler-rt, the payload SDK and the
kit from source, and the kit's build tools use [zlib](https://zlib.net/) 1.3.2
(zlib licence). None of these are part of what is installed on the PS5. The
tests' fake NAS and fake PS5 loader (`tests/`) are NASBridge's own. The SMB
tests also build libsmb2 for the computer they run on, and run
[Samba](https://www.samba.org/)'s `smbd` (GPL-3.0-or-later) with
[nss_wrapper](https://cwrap.org/nss_wrapper.html) (BSD-3-Clause) from the
toolchain image as their test NAS.
The `.ffpfsc` test images (`tests/fixtures/*.ffpfsc.gz`) were made with
MkPFS 1.1.0 and Dump Forge 0.4.0 by `tests/fixtures/make_ffpfsc.sh`, from a
made-up game that script writes. Neither tool is part of the build.

## Disclaimer

NASBridge is an independent homebrew project. It isn't affiliated with,
endorsed by or sponsored by Sony Interactive Entertainment. "PlayStation" and
"PS5" are trademarks of Sony Interactive Entertainment Inc. No Sony SDK,
firmware, encryption key or system module is included.
