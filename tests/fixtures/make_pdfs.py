#!/usr/bin/env python3
"""Builds the PDF files the import tests read (needs reportlab). Run here:
   python3 make_pdfs.py"""
from reportlab.lib.pagesizes import A5
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader
import io, struct, zlib

W, H = A5
M = 50  # margin

def para_lines(c, words, x0, y, width, font, size, indent, first):
    """Writes a paragraph word by word, wrapping by hand and hyphenating long words; returns the new y."""
    lines, line = [], []
    def w(t, f=font): return c.stringWidth(t, f, size)
    avail = width - (0 if first else indent)
    cur = 0.0
    for word, ital in words:
        f = 'Times-Italic' if ital else font
        ww = w(word + ' ', f)
        if cur + ww > avail and line:
            # Hyphenate a long word across the line end.
            if len(word) >= 9 and not ital:
                head = word[:5] + '-'
                line.append((head, f)); lines.append(line); line = [(word[5:] + ' ', f)]
                cur = w(word[5:] + ' ', f); avail = width; continue
            lines.append(line); line = []; cur = 0.0; avail = width
        line.append((word + ' ', f)); cur += ww
    if line: lines.append(line)
    for i, ln in enumerate(lines):
        x = x0 + (indent if (i == 0 and not first) else 0)
        for t, f in ln:
            c.setFont(f, size); c.drawString(x, y, t); x += w(t, f)
        y -= size * 1.35
    return y

def words_of(text):
    out = []
    for tok in text.split(' '):
        if tok.startswith('_') and tok.endswith('_'): out.append((tok.strip('_'), True))
        else: out.append((tok, False))
    return out

CHAPTERS = [
  ("CHAPTER ONE", "The Lighthouse", [
    "Mara climbed the stairs of the old lighthouse every morning before the town woke, counting the steps as her grandmother had taught her.",
    "The lamp had not been lit for _twenty_ years, but she polished the brass anyway. Somebody had to remember how it was done, she thought.",
    "SCENE",
    "By noon the fog had lifted and the harbourmaster came looking for her with news that would change everything in the village.",
  ]),
  ("CHAPTER TWO", "Strangers", [
    "The ship arrived on a Tuesday, which nobody liked, because Tuesdays were for mending nets and arguing about the weather.",
    "Its captain was a tall woman with a voice like gravel, and she asked for Mara by name before she had even stepped ashore.",
  ]),
  ("CHAPTER THREE", "The Map", [
    "Inside the captain's cabin a map lay open across the table, its edges held down by four smooth stones from a distant beach.",
    "Mara recognised the coastline at once. It was her own, drawn with extraordinary care by someone who had walked every mile of it.",
  ]),
]

def book(name, outline=False, encrypt=None, header=True):
    buf = io.BytesIO()
    c = canvas.Canvas(name, pagesize=A5, encrypt=encrypt)
    c.setTitle('The Lighthouse Keeper'); c.setAuthor('Ada Example')
    page = [0]
    def newpage(running=True):
        if page[0]:
            c.showPage()
        page[0] += 1
        if running and header:
            c.setFont('Times-Roman', 8); c.drawCentredString(W / 2, H - 25, 'THE LIGHTHOUSE KEEPER')
            c.drawCentredString(W / 2, 22, str(page[0]))
    # Title page and copyright page.
    newpage(False)
    c.setFont('Times-Bold', 24); c.drawCentredString(W / 2, H * 0.6, 'The Lighthouse Keeper')
    c.setFont('Times-Roman', 14); c.drawCentredString(W / 2, H * 0.6 - 30, 'Ada Example')
    newpage(False)
    c.setFont('Times-Roman', 8); c.drawString(M, M + 40, 'Copyright 2026 Ada Example. All rights reserved.')
    for idx, (num, title, paras) in enumerate(CHAPTERS):
        newpage()
        if outline:
            c.bookmarkPage('ch%d' % idx); c.addOutlineEntry('%s: %s' % (num.title(), title), 'ch%d' % idx, level=0)
        c.setFont('Times-Bold', 18); c.drawCentredString(W / 2, H - 110, num)
        c.setFont('Times-Italic', 15); c.drawCentredString(W / 2, H - 135, title)
        y = H - 190
        first = True
        for p in paras:
            if p == 'SCENE':
                y -= 6; c.setFont('Times-Roman', 10.5); c.drawCentredString(W / 2, y, '* * *'); y -= 10.5 * 1.35 + 6
                first = True; continue
            if first:
                # Drop cap on the chapter's first paragraph.
                if p == paras[0]:
                    c.setFont('Times-Bold', 30); c.drawString(M, y - 16, p[0]); p = p[1:]
            if y < 90:
                newpage(); y = H - 60
            y = para_lines(c, words_of(p), M, y, W - 2 * M, 'Times-Roman', 10.5, 14, first)
            first = False
    c.showPage(); c.save()

book('novel.pdf')
book('novel-outline.pdf', outline=True)

from reportlab.lib import pdfencrypt
book('locked.pdf', encrypt=pdfencrypt.StandardEncryption('secret', ownerPassword='owner'))
book('nocopy.pdf', encrypt=pdfencrypt.StandardEncryption('', ownerPassword='owner', canCopy=0))

# A "scanned" PDF: a picture of a page, no text.
def png(w, h):
    raw = b''.join(b'\x00' + b'\xee\xee\xee' * w for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
c = canvas.Canvas('scanned.pdf', pagesize=A5)
for _ in range(3):
    c.drawImage(ImageReader(io.BytesIO(png(40, 60))), 0, 0, W, H); c.showPage()
c.save()
print('ok')
