#!/usr/bin/env python3
"""Regenerate src/font_agave.h from an Agave Nerd Font Mono TTF.

Metrics come from parsing the TTF directly (head/hhea/hmtx/cmap/loca/glyf);
pixels are rasterized by ImageMagick through one MVG script per size, with
each glyph placed on our grid at its true scaled bbox position. A probe
pass calibrates pointsize/baseline so ink lands within a pixel of the
outline-math prediction.

Policy: codepoints 32..255 (contiguous, blanks included so bytes index
directly) are embedded at every size. Everything else assigned in the
cmap -- Latin extensions, arrows, and the full Nerd Font icon ranges --
is embedded only at AGAVE_DEFAULT_PX and reached through a sorted
codepoint->offset table that draw.c binary-searches after UTF-8 decoding.
"""
import struct
import subprocess
import sys
import tempfile

TTF_PATH = "/home/danks/.local/share/fonts/AgaveNerdFontMono-Regular.ttf"
OUT = "src/font_agave.h"
SIZES = [12, 14, 16, 18, 20, 24, 28]
DEFAULT_PX = 16
BASE_LO, BASE_HI = 32, 255
THRESH = 128
SS = 4 # supersample factor: rasterize SSx for coverage
LEVELS = 15 # 4-bit coverage stored per pixel


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def i16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


class TTF:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        num = u16(d, 4)
        tabs = {}
        for i in range(num):
            o = 12 + 16 * i
            tag = d[o:o + 4].decode("latin1")
            tabs[tag] = (u32(d, o + 8), u32(d, o + 12))
        self.tabs = tabs

        ho = tabs["head"][0]
        self.upem = u16(d, ho + 18)
        self.loc_fmt = i16(d, ho + 50)

        hh = tabs["hhea"][0]
        self.ascender = i16(d, hh + 4)
        self.descender = i16(d, hh + 6)
        self.n_hmetrics = u16(d, hh + 34)

        self.num_glyphs = u16(d, tabs["maxp"][0] + 4)
        self.cmap = self._parse_cmap()
        self.adv = self._parse_hmtx()
        self.bbox = self._parse_bboxes()

    def _parse_cmap(self):
        d = self.data
        co = self.tabs["cmap"][0]
        n = u16(d, co + 2)
        best = None
        for i in range(n):
            o = co + 4 + 8 * i
            pid, eid, off = u16(d, o), u16(d, o + 2), u32(d, o + 4)
            fmt = u16(d, co + off)
            if fmt == 12:
                best = co + off
                break
            if fmt == 4 and best is None:
                best = co + off
        if best is None:
            sys.exit("no format 4/12 cmap")
        fmt = u16(d, best)
        m = {}
        if fmt == 4:
            seg = u16(d, best + 6) // 2
            ends = best + 14
            starts = ends + 2 * seg + 2
            deltas = starts + 2 * seg
            ranges = deltas + 2 * seg
            for s in range(seg):
                sc = u16(d, starts + 2 * s)
                ec = u16(d, ends + 2 * s)
                delta = i16(d, deltas + 2 * s)
                ro = u16(d, ranges + 2 * s)
                for c in range(sc, min(ec, 0x10FFFF) + 1):
                    if ro == 0:
                        g = (c + delta) & 0xFFFF
                    else:
                        gi = ranges + 2 * s + ro + 2 * (c - sc)
                        g = u16(d, gi)
                        if g:
                            g = (g + delta) & 0xFFFF
                    if g:
                        m[c] = g
        else:
            ng = u32(d, best + 12)
            for i in range(ng):
                o = best + 16 + 12 * i
                s, e, g = u32(d, o), u32(d, o + 4), u32(d, o + 8)
                for c in range(s, e + 1):
                    m[c] = g + (c - s)
        return m

    def _parse_hmtx(self):
        d = self.data
        o = self.tabs["hmtx"][0]
        n = self.n_hmetrics
        adv = [u16(d, o + 4 * i) for i in range(n)]
        return adv + [adv[-1]] * (self.num_glyphs - n)

    def _parse_bboxes(self):
        d = self.data
        lo, ln = self.tabs["loca"]
        gl, _ = self.tabs["glyf"]
        bb = []
        for i in range(self.num_glyphs):
            if self.loc_fmt == 0:
                a, b = 2 * u16(d, lo + 2 * i), 2 * u16(d, lo + 2 * i + 2)
            else:
                a, b = u32(d, lo + 4 * i), u32(d, lo + 4 * i + 2)
            if b <= a:
                bb.append(None)
                continue
            go = gl + a
            bb.append((i16(d, go + 2), i16(d, go + 4),
                       i16(d, go + 6), i16(d, go + 8)))
        return bb


def mv_escape(cp):
    ch = chr(cp)
    return ch.replace("\\", "\\\\").replace('"', '\\"')


def render_sheet(font, cps, adv, asc, cell_h, ps, dy, cols=256):
    """Rasterize cps on our grid via one MVG script; return dict cp->rows.

    Rendered at SSx resolution and majority-voted down to one bit per
    pixel so diagonal strokes get coverage-weighted edges instead of
    whatever a single scanline happens to hit."""
    import math
    rows_n = math.ceil(len(cps) / cols)
    W, H = cols * adv * SS, rows_n * cell_h * SS
    lines = ["push graphic-context",
             "font '%s'" % TTF_PATH, "font-size %g" % (ps * SS),
             "fill white", "stroke none"]
    for i, c in enumerate(cps):
        x = (i % cols) * adv * SS
        y = ((i // cols) * cell_h + asc + dy) * SS
        lines.append('text %d,%d "%s"' % (x, y, mv_escape(c)))
    lines.append("pop graphic-context")
    mvg = "\n".join(lines) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".mvg", delete=False) as f:
        f.write(mvg)
        path = f.name
    raw = subprocess.run(
        ["convert", "-size", "%dx%d" % (W, H), "xc:black",
         "-draw", "@" + path,
         "-threshold", "%d%%" % (THRESH * 100 // 256), "-depth", "8",
         "gray:-"], capture_output=True, check=True).stdout
    assert len(raw) >= W * H, "short raster %d < %d" % (len(raw), W * H)
    half = (SS * SS) // 2

    def cell(i):
        cx, cy = (i % cols) * adv * SS, (i // cols) * cell_h * SS
        return [[round(sum(1 for sy in range(SS) for sx in range(SS)
                           if raw[(cy + r * SS + sy) * W + cx + c * SS + sx]
                           >= THRESH) / (SS * SS) * LEVELS)
                 for c in range(adv)] for r in range(cell_h)]

    return {cp: cell(i) for i, cp in enumerate(cps)}, H


def ink_rows(grid):
    rs = [r for r, row in enumerate(grid) if any(row)]
    return (min(rs), max(rs)) if rs else (None, None)


def calibrate(font, k, cell_h_guess, ps):
    """Render 'H' probe; align pointsize + baseline to outline math."""
    cp_h = ord("H")
    gid = font.cmap.get(cp_h)
    if gid is None:
        return 0.0
    xmin, ymin, xmax, ymax = font.bbox[gid]
    cap_target = round(ymax * k)
    ps, dy, asc_g = float(cell_h_guess), 0.0, cell_h_guess
    for _ in range(3):
        g, _ = render_sheet(font, [cp_h], 32, asc_g, cell_h_guess + 24,
                            ps, dy, cols=1)
        top, bot = ink_rows(g[cp_h])
        # baseline sits at row asc_g; want ink top at asc_g - cap_target
        want_top = asc_g - cap_target
        dy += want_top - top
        break
    return dy


def gen_size(font, px, k, ext_ok):
    """Return (metrics, bits bytes, ext list) for one pixel size."""
    adv_ref = font.adv[font.cmap[ord("A")]]
    adv = round(adv_ref * k)
    asc = round(font.ascender * k)
    dsc = round(-font.descender * k)
    cell_h = asc + dsc
    rb = (adv + 7) // 8

    base_cps = list(range(BASE_LO, BASE_HI + 1))
    dy = calibrate(font, k, cell_h, px)
    grids, H = render_sheet(font, base_cps, adv, asc, cell_h, px, dy)
    bits = bytearray()
    base_bits_at = {}
    for cp in base_cps:
        g = grids[cp]
        base_bits_at[cp] = len(bits)
        for row in g:
            for c in range(0, adv, 2):
                hi = row[c]
                lo = row[c + 1] if c + 1 < adv else 0
                bits.append((hi << 4) | lo)

    ext = []
    if ext_ok:
        others = sorted(cp for cp in font.cmap
                        if not (BASE_LO <= cp <= BASE_HI))
        others = [c for c in others if font.bbox[font.cmap[c]]]
        ext_grids, _ = render_sheet(font, others, adv, asc, cell_h, px, dy)
        for cp in others:
            g = ext_grids[cp]
            if not any(any(r) for r in g):
                continue
            off = len(bits)
            for row in g:
                for c in range(0, adv, 2):
                    hi = row[c]
                    lo = row[c + 1] if c + 1 < adv else 0
                    bits.append((hi << 4) | lo)
            ext.append((cp, off))

    return dict(adv=adv, asc=asc, dsc=dsc, cell_h=cell_h,
                rb=(adv + 1) // 2, base_off=base_bits_at), bits, ext, dy


def emit(name, bits, ext, m):
    arr = "bits_%s" % name
    lines = ["static const uint8_t %s[] = {" % arr]
    cur = "   "
    for b in bits:
        tok = "0x%02x," % b
        if len(cur) + len(tok) > 76:
            lines.append(cur)
            cur = "   "
        cur += tok
    if cur.strip():
        lines.append(cur.rstrip(","))
    lines.append("};")
    if ext:
        ea = "ext_%s" % name
        lines.append("static const ext_glyph_t %s[] = {" % ea)
        for cp, off in ext:
            lines.append("    { 0x%xu, %du }," % (cp, off))
        lines.append("};")
    return "\n".join(lines), arr, m, ext


def main():
    font = TTF(TTF_PATH)
    print("upem=%d asc=%d desc=%d glyphs=%d cmap=%d" %
          (font.upem, font.ascender, font.descender, font.num_glyphs,
           len(font.cmap)))
    chunks, tables, meta = [], [], {}
    total = 0
    for px in SIZES:
        k = px / font.upem
        bits, extl, dy = [], [], 0
        m, bits, extl, dy = gen_size(font, px, k, px == DEFAULT_PX)
        name = "agave%d" % px
        txt, arr, m, extl = emit(name, bits, extl, m)
        chunks.append(txt)
        meta[name] = (arr, m, extl)
        total += len(bits)
        print("%s: adv=%d cell=%dx%d rb=%d ext=%d bytes=%d dy=%.0f" %
              (name, m["adv"], m["adv"], m["cell_h"], m["rb"], len(extl),
               len(bits), dy))

    hdr = []
    hdr.append("/* Agave Nerd Font Mono, rasterized at build time.")
    hdr.append("   Generated by scripts/genfont.py -- do not edit. */")
    hdr.append("#ifndef AUSTERE_FONT_AGAVE_H")
    hdr.append("#define AUSTERE_FONT_AGAVE_H")
    hdr.append("#include <stdint.h>")
    hdr.append("")
    hdr.append("typedef struct {")
    hdr.append("    uint32_t cp;")
    hdr.append("    uint32_t off;")
    hdr.append("} ext_glyph_t;")
    hdr.append("")
    hdr.append("typedef struct {")
    hdr.append('    const char *name; /* conf font value: size digits */')
    hdr.append("    uint8_t cell_w, cell_h, ascent, descent, advance;")
    hdr.append("    uint16_t nglyph; /* contiguous block starts at 32 */")
    hdr.append("    const uint8_t *bits; /* 4-bit coverage, 2 px/byte, */")
    hdr.append("    uint8_t rowbytes; /* MSN = even column */")
    hdr.append("    const ext_glyph_t *ext; /* sorted by cp, may be NULL */")
    hdr.append("    uint16_t ext_n;")
    hdr.append("} builtin_font_t;")
    hdr.append("")
    hdr.extend(c for c in chunks)
    hdr.append("#define N_BUILTIN_FONTS %d" % len(SIZES))
    hdr.append("static const builtin_font_t *const builtin_fonts[] = {")
    for px in SIZES:
        name = "agave%d" % px
        arr, m, extl = meta[name]
        ea = "ext_%s" % name if extl else "NULL"
        en = "%du" % len(extl) if extl else "0"
        hdr.append('    &(%s){ "%s", %d, %d, %d, %d, %d, %d, %s, %d, %s, %s },'
                   % ("builtin_font_t", name, m["adv"], m["cell_h"],
                      m["asc"], m["dsc"], m["adv"], BASE_HI - BASE_LO + 1,
                      arr, m["rb"], ea, en))
    hdr.append("};")
    hdr.append("")
    hdr.append("#endif")
    open(OUT, "w").write("\n".join(hdr) + "\n")
    print("total raster bytes: %d (%.0f KB)" % (total, total / 1024))


if __name__ == "__main__":
    main()
