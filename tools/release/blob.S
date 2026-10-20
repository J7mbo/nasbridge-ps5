/* NASBridge - The packed app (tools/release/pack.py), linked into the release
 * installer as nb_app_blob .. nb_app_blob_end. NB_BLOB is its path, as a
 * quoted string (-DNB_BLOB='"build/ps5/release/app.nbz"').
 * Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

  .section .rodata
  .balign 16
  .globl nb_app_blob
  .globl nb_app_blob_end
nb_app_blob:
  .incbin NB_BLOB
nb_app_blob_end:

  .section .note.GNU-stack,"",@progbits
