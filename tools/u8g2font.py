"""Minimal U8g2 font codec.

decode(): parse a U8g2 font byte string into {encoding: Glyph} + header info
encode(): build a U8g2 font byte string from Glyphs (8-bit encodings only,
          plus an optional unicode block for encodings > 255)

Glyph.bitmap is a list of rows, each a list of 0/1, size h x w.
Glyph.x / Glyph.y are BDF-style: x = left bearing, y = offset of the bitmap's
bottom edge from the baseline (positive = above). d = advance.
"""
from dataclasses import dataclass, field
import re, codecs

HDR = 23

@dataclass
class Glyph:
    enc: int
    w: int
    h: int
    x: int
    y: int
    d: int
    bitmap: list = field(default_factory=list)

    @property
    def top(self):      # rows above baseline
        return self.h + self.y

# ---------------------------------------------------------------- bit I/O
class BitReader:
    def __init__(self, data, pos):
        self.data = data; self.pos = pos; self.bit = 0
    def read(self, n):
        v = 0; got = 0
        while got < n:
            byte = self.data[self.pos]
            take = min(8 - self.bit, n - got)
            v |= ((byte >> self.bit) & ((1 << take) - 1)) << got
            got += take; self.bit += take
            if self.bit == 8:
                self.bit = 0; self.pos += 1
        return v
    def read_signed(self, n):
        return self.read(n) - (1 << (n - 1))

class BitWriter:
    def __init__(self):
        self.out = bytearray(); self.cur = 0; self.bit = 0
    def write(self, v, n):
        for i in range(n):
            if (v >> i) & 1:
                self.cur |= 1 << self.bit
            self.bit += 1
            if self.bit == 8:
                self.out.append(self.cur); self.cur = 0; self.bit = 0
    def write_signed(self, v, n):
        self.write(v + (1 << (n - 1)), n)
    def bytes(self):
        if self.bit:
            self.out.append(self.cur); self.cur = 0; self.bit = 0
        return bytes(self.out)

# ---------------------------------------------------------------- decode
def _word(f, p):
    return (f[p] << 8) | f[p + 1]

def decode_glyph_body(f, pos, hdr):
    """pos -> start of bitfields. Returns Glyph (enc unset) and end pos."""
    br = BitReader(f, pos)
    w = br.read(hdr['bw']); h = br.read(hdr['bh'])
    x = br.read_signed(hdr['bx']); y = br.read_signed(hdr['by']); d = br.read_signed(hdr['bd'])
    pixels = []
    n = w * h
    if n:
        while len(pixels) < n:
            a = br.read(hdr['b0']); b = br.read(hdr['b1'])
            while True:
                pixels.extend([0] * a); pixels.extend([1] * b)
                if br.read(1) == 0:
                    break
    rows = [pixels[r * w:(r + 1) * w] for r in range(h)]
    return Glyph(0, w, h, x, y, d, rows)

def header(f):
    s8 = lambda b: b - 256 if b > 127 else b
    return dict(cnt=f[0], mode=f[1], b0=f[2], b1=f[3], bw=f[4], bh=f[5], bx=f[6], by=f[7], bd=f[8],
                max_w=f[9], max_h=f[10], x_off=s8(f[11]), y_off=s8(f[12]),
                ascent_A=s8(f[13]), descent_g=s8(f[14]), ascent_para=s8(f[15]), descent_para=s8(f[16]),
                start_A=_word(f, 17), start_a=_word(f, 19), start_uni=_word(f, 21))

def decode(f):
    hdr = header(f)
    glyphs = {}
    pos = HDR
    while f[pos + 1] != 0:
        g = decode_glyph_body(f, pos + 2, hdr); g.enc = f[pos]
        glyphs[g.enc] = g
        pos += f[pos + 1]
    if hdr['start_uni']:
        uni = HDR + hdr['start_uni']; tbl = uni; blk = uni
        while tbl + 3 < len(f):
            off = _word(f, tbl); e = _word(f, tbl + 2); tbl += 4; blk += off
            p = blk
            while p + 2 < len(f):
                ce = _word(f, p)
                if ce == 0:
                    break
                g = decode_glyph_body(f, p + 3, hdr); g.enc = ce
                glyphs[ce] = g
                p += f[p + 2]
            if e >= 0xFFFF:
                break
    return hdr, glyphs

def load_from_c(path, name):
    """Extract font `name` from a U8g2 style C source (string literal form)."""
    src = open(path, encoding='utf-8', errors='replace').read()
    i = src.index('const uint8_t ' + name + '['); i = src.index('=', i); j = src.index('";\n', i) + 1
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', src[i:j], re.S)
    return b''.join(codecs.escape_decode(l.encode('latin1'))[0] for l in lits) + b'\0\0'

# ---------------------------------------------------------------- encode
def _bits_unsigned(maxv):
    n = 1
    while (1 << n) <= maxv:
        n += 1
    return n

def _bits_signed(vals):
    n = 1
    while True:
        lo, hi = -(1 << (n - 1)), (1 << (n - 1)) - 1
        if all(lo <= v <= hi for v in vals):
            return n
        n += 1

def _rle_pairs(pixels, max0, max1):
    pairs = []; i = 0; n = len(pixels)
    while i < n:
        a = 0
        while i < n and pixels[i] == 0 and a < max0:
            a += 1; i += 1
        b = 0
        while i < n and pixels[i] == 1 and b < max1:
            b += 1; i += 1
        pairs.append((a, b))
    return pairs

def _encode_glyph_bits(g, bw, bh, bx, by, bd, b0, b1):
    bwr = BitWriter()
    bwr.write(g.w, bw); bwr.write(g.h, bh)
    bwr.write_signed(g.x, bx); bwr.write_signed(g.y, by); bwr.write_signed(g.d, bd)
    if g.w and g.h:
        pixels = [p for row in g.bitmap for p in row]
        assert len(pixels) == g.w * g.h
        pairs = _rle_pairs(pixels, (1 << b0) - 1, (1 << b1) - 1)
        i = 0
        while i < len(pairs):
            a, b = pairs[i]
            j = i
            while j + 1 < len(pairs) and pairs[j + 1] == (a, b):
                j += 1
            bwr.write(a, b0); bwr.write(b, b1)
            for _ in range(j - i):
                bwr.write(1, 1)
            bwr.write(0, 1)
            i = j + 1
    return bwr.bytes()

def encode(glyphs, ascent=None, descent=None):
    """glyphs: list of Glyph. Returns bytes of a U8g2 font."""
    glyphs = sorted(glyphs, key=lambda g: g.enc)
    bw = _bits_unsigned(max(g.w for g in glyphs))
    bh = _bits_unsigned(max(g.h for g in glyphs))
    bx = _bits_signed([g.x for g in glyphs])
    by = _bits_signed([g.y for g in glyphs])
    bd = _bits_signed([g.d for g in glyphs])
    best = None
    for b0 in range(2, 9):
        for b1 in range(2, 9):
            total = 0
            for g in glyphs:
                total += len(_encode_glyph_bits(g, bw, bh, bx, by, bd, b0, b1))
            if best is None or total < best[0]:
                best = (total, b0, b1)
    _, b0, b1 = best

    body = bytearray()
    start_A = start_a = None
    uni_glyphs = []
    for g in glyphs:
        if g.enc > 255:
            uni_glyphs.append(g); continue
        if start_A is None and g.enc >= ord('A'):
            start_A = len(body)
        if start_a is None and g.enc >= ord('a'):
            start_a = len(body)
        data = _encode_glyph_bits(g, bw, bh, bx, by, bd, b0, b1)
        size = len(data) + 2
        assert size <= 255, f"glyph {g.enc} too large"
        body += bytes([g.enc, size]) + data
    body += b'\0\0'
    if start_A is None: start_A = len(body) - 2
    if start_a is None: start_a = len(body) - 2
    start_uni = len(body)
    if uni_glyphs:
        # one jump-table entry per block of up to 100 glyphs (like bdfconv)
        blocks = [uni_glyphs[i:i + 100] for i in range(0, len(uni_glyphs), 100)]
        blobs = []
        for blk in blocks:
            bb = bytearray()
            for g in blk:
                data = _encode_glyph_bits(g, bw, bh, bx, by, bd, b0, b1)
                size = len(data) + 3
                assert size <= 255
                bb += bytes([g.enc >> 8, g.enc & 0xFF, size]) + data
            bb += b'\0\0'
            blobs.append(bytes(bb))
        table = bytearray()
        off = 4 * len(blobs)   # first block starts right after the table
        for k, (blk, bb) in enumerate(zip(blocks, blobs)):
            last = 0xFFFF if k == len(blocks) - 1 else blk[-1].enc
            table += bytes([off >> 8, off & 0xFF, last >> 8, last & 0xFF])
            off = len(bb)
        body += table + b''.join(blobs)

    vis = [g for g in glyphs if g.w and g.h]
    max_w = max(g.w for g in vis); max_h = max(g.h for g in vis)
    x_off = min(g.x for g in vis); y_off = min(g.y for g in vis)
    top = max(g.top for g in vis); bot = min(g.y for g in vis)
    if ascent is None: ascent = top
    if descent is None: descent = bot
    u8 = lambda v: v & 0xFF
    hdr = bytes([len(glyphs) & 0xFF, 0, b0, b1, bw, bh, bx, by, bd, max_w, max_h,
                 u8(x_off), u8(y_off), u8(ascent), u8(descent), u8(top), u8(bot),
                 start_A >> 8, start_A & 0xFF, start_a >> 8, start_a & 0xFF, start_uni >> 8, start_uni & 0xFF])
    return hdr + bytes(body)

def to_c(name, data, comment=''):
    lines = [f'/* {comment} */' if comment else '', f'const uint8_t {name}[{len(data)}] U8G2_FONT_SECTION("{name}") = {{']
    for i in range(0, len(data), 16):
        lines.append('  ' + ','.join(f'0x{b:02x}' for b in data[i:i + 16]) + ',')
    lines.append('};')
    return '\n'.join(lines) + '\n'

# ---------------------------------------------------------------- render
class Canvas:
    def __init__(self, w=128, h=64):
        self.w = w; self.h = h
        self.px = [[0] * w for _ in range(h)]
    def set(self, x, y, v=1):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = v
    def box(self, x, y, w, h, v=1):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, v)
    def hline(self, x, y, w, v=1):
        self.box(x, y, w, 1, v)
    def glyph(self, g, x, y, color=1):
        top = y - g.top
        for r, row in enumerate(g.bitmap):
            for c, p in enumerate(row):
                if p:
                    self.set(x + g.x + c, top + r, color)
    def text(self, glyphs, s, x, y, color=1):
        for ch in s:
            g = glyphs.get(ord(ch))
            if g is None:
                raise KeyError(f'no glyph for {ch!r}')
            self.glyph(g, x, y, color); x += g.d
        return x
    def ascii(self):
        return '\n'.join(''.join('#' if p else '.' for p in row) for row in self.px)
    def png(self, path, scale=4, fg=(255, 255, 255), bg=(0, 0, 0)):
        from PIL import Image
        im = Image.new('RGB', (self.w, self.h), bg)
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y][x]:
                    im.putpixel((x, y), fg)
        im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        im.save(path)

def text_width(glyphs, s):
    return sum(glyphs[ord(ch)].d for ch in s)
