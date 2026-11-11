# GX TPL images (RGB5A3, CMPR, RGBA8, I8) decoded with numpy.
import struct
import numpy as np

def untile(blocks, w, h, bw, bh):
    tw, th = (w + bw - 1) // bw, (h + bh - 1) // bh
    a = blocks.reshape(th, tw, bh, bw, -1).transpose(0, 2, 1, 3, 4).reshape(th * bh, tw * bw, -1)
    return a[:h, :w]

def c565(v):
    return np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], -1)

def decode(d, off, w, h, fmt):
    tw8, th8 = (w + 7) // 8 * 8, (h + 7) // 8 * 8
    tw4, th4 = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    if fmt == 5:
        v = np.frombuffer(d, '>u2', tw4 * th4, off).astype(np.int32)
        op = (v & 0x8000) != 0
        r = np.where(op, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
        g = np.where(op, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
        b = np.where(op, (v & 31) * 255 // 31, (v & 15) * 17)
        a = np.where(op, 255, ((v >> 12) & 7) * 255 // 7)
        return untile(np.stack([r, g, b, a], -1), w, h, 4, 4)
    if fmt == 14:
        n = tw8 * th8 // 16
        blk = np.frombuffer(d, np.uint8, n * 8, off).reshape(n, 8)
        c0 = blk[:, 0].astype(np.int32) << 8 | blk[:, 1]
        c1 = blk[:, 2].astype(np.int32) << 8 | blk[:, 3]
        bits = (blk[:, 4].astype(np.uint32) << 24) | (blk[:, 5].astype(np.uint32) << 16) | (blk[:, 6].astype(np.uint32) << 8) | blk[:, 7]
        A, B = c565(c0), c565(c1)
        four = (c0 > c1)[:, None]
        p2 = np.where(four, (2 * A + B) // 3, (A + B) // 2)
        p3 = np.where(four, (A + 2 * B) // 3, 0)
        pal = np.stack([A, B, p2, p3], 1)
        alpha = np.stack([np.full(n, 255), np.full(n, 255), np.full(n, 255), np.where(four[:, 0], 255, 0)], 1)
        pal = np.concatenate([pal, alpha[:, :, None]], -1)  # n,4,4
        idx = np.stack([(bits >> (30 - 2 * k)) & 3 for k in range(16)], 1).astype(np.int64)
        px = np.take_along_axis(pal, idx[:, :, None].repeat(4, 2), 1)  # n,16,4
        # sub-blocks: 2x2 of 4x4 inside 8x8 tiles
        px = px.reshape(th8 // 8, tw8 // 8, 2, 2, 4, 4, 4).transpose(0, 2, 4, 1, 3, 5, 6).reshape(th8, tw8, 4)
        return px[:h, :w]
    if fmt == 6:
        n = tw4 * th4 // 16
        blk = np.frombuffer(d, np.uint8, n * 64, off).reshape(n, 64)
        ar, gb = blk[:, :32].reshape(n, 16, 2), blk[:, 32:].reshape(n, 16, 2)
        px = np.stack([ar[:, :, 1], gb[:, :, 0], gb[:, :, 1], ar[:, :, 0]], -1)
        return untile(px.reshape(-1, 4), w, h, 4, 4)
    if fmt == 1:
        v = np.frombuffer(d, np.uint8, tw8 * (h + 3) // 4 * 4, off)
        th = (h + 3) // 4 * 4
        v = np.frombuffer(d, np.uint8, tw8 * th, off).reshape(th // 4, tw8 // 8, 4, 8).transpose(0, 2, 1, 3).reshape(th, tw8)[:h, :w]
        return np.stack([v, v, v, v], -1)
    return None

def load(path, index):
    """Image `index` of the TPL file at `path`, RGBA uint8 (rows, columns, 4)."""
    d = open(path, 'rb').read()
    n, t = struct.unpack('>II', d[4:12])
    ih, _ = struct.unpack('>II', d[t + 8 * index:t + 8 * index + 8])
    h, w, fmt, off = struct.unpack('>HHII', d[ih:ih + 12])
    return decode(d, off, w, h, fmt).astype(np.uint8)
