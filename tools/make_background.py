#!/usr/bin/env python3
"""Renders NASBridge's home-screen backgrounds (app/sce_sys/pic0.dds, pic1.dds).

A still frame of the app's own waves backdrop: the UI kit's "waves" shader
(src/gfx/backdrop.cpp, mode 5) redone in numpy with the colours the library
uses, at 3840x2160, written as the single-surface BC7_UNORM DX10 DDS the PS5
home screen wants (the kit's tools/validate-assets.sh profile). The BC7
encoder is a small mode-6 one: every 4x4 block gets two RGBA endpoints on its
main colour axis and 16 levels between them, which suits smooth gradients.

Run (needs numpy and Pillow):
    python3 tools/make_background.py app/sce_sys
It also writes background-preview.png (the decoded DDS, for checking).
"""
import struct
import sys

import numpy as np
from PIL import Image

W, H = 3840, 2160
TIME = 9.0  # the moment of the animation that is frozen


def rgb(hexcode):
    return np.array([(hexcode >> 16) & 255, (hexcode >> 8) & 255, hexcode & 255], np.float64) / 255.0


def mix(a, b, t):
    return a + (b - a) * t


# The library's waves colours (library.cpp, palette(Game{}, slot)).
BLACK = np.zeros(3)
PLAIN_DARK, PLAIN_MID, PRIMARY = rgb(0x150F26), rgb(0x3B2F63), rgb(0x76D6FF)
C0 = mix(PLAIN_DARK, BLACK, 0.45)
C1 = mix(PLAIN_DARK, BLACK, 0.70)
C2 = mix(PLAIN_MID, BLACK, 0.25)
C3 = mix(PRIMARY, BLACK, 0.35)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def render():
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    u = (x + 0.5) / W
    v = (y + 0.5) / H  # 0 at the top, as in the shader (uv.y = 1 - fragY / height)
    col = mix(C0[None, None, :], C1[None, None, :], v[..., None])
    for i in range(5):
        fi = float(i)
        wave = (0.42 + fi * 0.1
                + 0.05 * np.sin(u * (2.0 + fi * 0.7) + TIME * (0.25 + 0.07 * fi) + fi * 1.7)
                + 0.025 * np.sin(u * (5.0 + fi) - TIME * 0.2 + fi))
        ribbon = mix(C2, C3, fi / 4.0)
        edge = smoothstep(wave - 0.002, wave + 0.002, v)[..., None] * 0.16
        col = col + (ribbon[None, None, :] - col) * edge
        col = col + ribbon[None, None, :] * 0.3 * np.exp(-np.abs(v - wave) * 220.0)[..., None]
    # The shader's dither: a little noise hides banding on TVs.
    rng = np.random.default_rng(1)
    col = col + (rng.random((H, W, 1)) - 0.5) / 255.0
    rgb8 = np.clip(np.round(col * 255.0), 0, 255).astype(np.uint8)
    return np.concatenate([rgb8, np.full((H, W, 1), 255, np.uint8)], axis=2)


WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def palette_of(e0, e1):
    """e0, e1: (n, 4) 8-bit endpoints -> (n, 16, 4) palette."""
    w = WEIGHTS[None, :, None]
    return ((64 - w) * e0[:, None, :] + w * e1[:, None, :] + 32) >> 6


def encode_bc7(rgba):
    """Mode 6 for every block. rgba: (H, W, 4) uint8 -> bytes."""
    bh, bw = H // 4, W // 4
    blocks = rgba.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.float64)
    out = np.zeros((blocks.shape[0], 16), np.uint8)
    chunk = 16384
    for start in range(0, blocks.shape[0], chunk):
        b = blocks[start:start + chunk]
        n = b.shape[0]
        mean = b[:, :, :3].mean(axis=1)
        centred = b[:, :, :3] - mean[:, None, :]
        # The main colour axis (a few rounds of power iteration on the covariance).
        cov = np.einsum("npi,npj->nij", centred, centred)
        axis = np.ones((n, 3)) / np.sqrt(3.0)
        for _ in range(8):
            axis = np.einsum("nij,nj->ni", cov, axis)
            norm = np.linalg.norm(axis, axis=1, keepdims=True)
            axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), np.ones((n, 3)) / np.sqrt(3.0))
        t = np.einsum("npi,ni->np", centred, axis)
        lo = mean + axis * t.min(axis=1, keepdims=True)
        hi = mean + axis * t.max(axis=1, keepdims=True)
        # 7 bits per channel plus a shared p-bit of 1 (so alpha reaches 255).
        q0 = np.clip(np.round((np.clip(lo, 0, 255) - 1.0) / 2.0), 0, 127).astype(np.int32)
        q1 = np.clip(np.round((np.clip(hi, 0, 255) - 1.0) / 2.0), 0, 127).astype(np.int32)
        q0 = np.concatenate([q0, np.full((n, 1), 127)], axis=1)
        q1 = np.concatenate([q1, np.full((n, 1), 127)], axis=1)
        e0, e1 = q0 * 2 + 1, q1 * 2 + 1
        pal = palette_of(e0, e1).astype(np.float64)
        d = ((b[:, :, None, :3] - pal[:, None, :, :3]) ** 2).sum(axis=3)
        idx = d.argmin(axis=2).astype(np.int32)
        # The first pixel's index must have its top bit clear: swap if not.
        swap = idx[:, 0] >= 8
        q0[swap], q1[swap] = q1[swap].copy(), q0[swap].copy()
        idx[swap] = 15 - idx[swap]
        out[start:start + n] = pack_mode6(q0, q1, idx)
    return out.tobytes()


def pack_mode6(q0, q1, idx):
    n = q0.shape[0]
    bits = np.zeros((n, 128), np.uint8)
    pos = 0

    def put(values, count):
        nonlocal pos
        for k in range(count):
            bits[:, pos + k] = (values >> k) & 1
        pos += count

    put(np.full(n, 1 << 6), 7)  # mode 6
    for c in range(4):          # R0 R1 G0 G1 B0 B1 A0 A1
        put(q0[:, c], 7)
        put(q1[:, c], 7)
    put(np.ones(n, np.int32), 1)  # p0
    put(np.ones(n, np.int32), 1)  # p1
    put(idx[:, 0], 3)             # anchor index: 3 bits
    for p in range(1, 16):
        put(idx[:, p], 4)
    assert pos == 128
    return np.packbits(bits.reshape(n, 16, 8), axis=2, bitorder="little").reshape(n, 16)


def decode_bc7_mode6(data):
    raw = np.frombuffer(data, np.uint8).reshape(-1, 16)
    bits = np.unpackbits(raw, axis=1, bitorder="little").astype(np.int32)
    n = raw.shape[0]
    assert (bits[:, :7] == np.array([0, 0, 0, 0, 0, 0, 1])).all(), "not mode 6"

    def take(start, count):
        return sum(bits[:, start + k] << k for k in range(count))

    e = []
    for c in range(4):
        e.append((take(7 + c * 14, 7), take(14 + c * 14, 7)))
    p0, p1 = bits[:, 63], bits[:, 64]
    e0 = np.stack([e[c][0] * 2 + p0 for c in range(4)], axis=1)
    e1 = np.stack([e[c][1] * 2 + p1 for c in range(4)], axis=1)
    idx = np.zeros((n, 16), np.int32)
    idx[:, 0] = take(65, 3)
    for p in range(1, 16):
        idx[:, p] = take(68 + (p - 1) * 4, 4)
    pal = palette_of(e0, e1)
    px = np.take_along_axis(pal, idx[:, :, None], axis=1).astype(np.uint8)
    return px.reshape(H // 4, W // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(H, W, 4)


def dds(payload):
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000,
                         H, W, len(payload), 0, 1)
    pixel_format = struct.pack("<2I4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<5I", 98, 3, 0, 1, 0)
    data = header + pixel_format + caps + dx10
    assert len(data) == 148
    return data + payload


def main(out_dir):
    image = render()
    payload = encode_bc7(image)
    decoded = decode_bc7_mode6(payload)
    err = (decoded[:, :, :3].astype(np.float64) - image[:, :, :3]) ** 2
    psnr = 10 * np.log10(255.0 ** 2 / max(err.mean(), 1e-9))
    print(f"BC7 round trip: PSNR {psnr:.1f} dB, worst channel error {int(np.sqrt(err.max()))}")
    blob = dds(payload)
    for name in ("pic0.dds", "pic1.dds"):
        with open(f"{out_dir}/{name}", "wb") as f:
            f.write(blob)
    Image.fromarray(decoded[:, :, :3]).save("background-preview.png")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
