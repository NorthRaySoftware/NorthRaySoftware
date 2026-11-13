#!/usr/bin/env python3
"""Builds the small EPUB files the import tests read. Run from this folder:
   python3 make_fixtures.py"""
import zipfile, struct, zlib

def png(w=2, h=2):
    raw = b''.join(b'\x00' + b'\xff\x00\x00' * w for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

CONTAINER = '''<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'''

def xhtml(body, title='x', btype=''):
    return f'''<?xml version="1.0" encoding="utf-8"?><!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>{title}</title><link rel="stylesheet" href="../Styles/book.css" type="text/css"/></head><body{(' epub:type="'+btype+'"') if btype else ''}>{body}</body></html>'''

def write(name, files, extra_meta=''):
    with zipfile.ZipFile(name, 'w') as z:
        z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
        z.writestr('META-INF/container.xml', CONTAINER)
        for k, v in files.items():
            z.writestr(k, v, compress_type=zipfile.ZIP_DEFLATED)

# 1. A novel: EPUB 3, nav, two parts, cover, front and back matter, CSS styling, an image.
css = '.ital { font-style: italic } p.noind { text-indent: 0 } .sc { font-variant: small-caps } .ctr { text-align: center } .hidden { display: none }'
nav = xhtml('''<nav epub:type="toc"><h1>Contents</h1><ol>
<li><a href="Text/part1.xhtml">Part One: Arrival</a><ol>
  <li><a href="Text/ch1.xhtml">Chapter 1: The Storm</a></li>
  <li><a href="Text/ch2.xhtml#start">Chapter 2</a><ol><li><a href="Text/ch2.xhtml#deep">Deep entry</a></li></ol></li>
</ol></li>
<li><a href="Text/part2.xhtml">Part Two</a><ol><li><a href="Text/ch3.xhtml">Chapter 3</a></li></ol></li>
<li><a href="Text/ack.xhtml">Acknowledgements</a></li>
</ol></nav>''', 'Contents', 'frontmatter')
opf = '''<?xml version="1.0" encoding="utf-8"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
 <dc:title id="t1">The Glass Harbour</dc:title><meta refines="#t1" property="title-type">main</meta>
 <dc:title id="t2">A Novel</dc:title><meta refines="#t2" property="title-type">subtitle</meta>
 <dc:creator id="c1">Ada Example</dc:creator><dc:language>en-GB</dc:language>
 <dc:identifier id="uid">urn:isbn:978-0-00-000000-2</dc:identifier>
 <dc:description>&lt;p&gt;A &lt;b&gt;quiet&lt;/b&gt; town.&lt;/p&gt;</dc:description>
</metadata>
<manifest>
 <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
 <item id="css" href="Styles/book.css" media-type="text/css"/>
 <item id="cover-img" href="Images/cover.png" media-type="image/png" properties="cover-image"/>
 <item id="pic" href="Images/map%20one.png" media-type="image/png"/>
 <item id="cover" href="Text/cover.xhtml" media-type="application/xhtml+xml"/>
 <item id="title" href="Text/title.xhtml" media-type="application/xhtml+xml"/>
 <item id="copy" href="Text/copyright.xhtml" media-type="application/xhtml+xml"/>
 <item id="p1" href="Text/part1.xhtml" media-type="application/xhtml+xml"/>
 <item id="c1" href="Text/ch1.xhtml" media-type="application/xhtml+xml"/>
 <item id="c1b" href="Text/ch1b.xhtml" media-type="application/xhtml+xml"/>
 <item id="c2" href="Text/ch2.xhtml" media-type="application/xhtml+xml"/>
 <item id="p2" href="Text/part2.xhtml" media-type="application/xhtml+xml"/>
 <item id="c3" href="Text/ch3.xhtml" media-type="application/xhtml+xml"/>
 <item id="ack" href="Text/ack.xhtml" media-type="application/xhtml+xml"/>
 <item id="about" href="Text/about.xhtml" media-type="application/xhtml+xml"/>
</manifest>
<spine><itemref idref="cover"/><itemref idref="title"/><itemref idref="copy"/><itemref idref="nav"/><itemref idref="p1"/>
<itemref idref="c1"/><itemref idref="c1b"/><itemref idref="c2"/><itemref idref="p2"/><itemref idref="c3"/><itemref idref="ack"/><itemref idref="about"/></spine>
</package>'''
files = {
 'OEBPS/content.opf': opf, 'OEBPS/nav.xhtml': nav, 'OEBPS/Styles/book.css': css,
 'OEBPS/Images/cover.png': png(4, 6), 'OEBPS/Images/map one.png': png(3, 3),
 'OEBPS/Text/cover.xhtml': xhtml('<div><img src="../Images/cover.png" alt="Cover"/></div>', 'Cover', 'cover'),
 'OEBPS/Text/title.xhtml': xhtml('<h1>The Glass Harbour</h1><p>Ada Example</p>', 'Title', 'titlepage'),
 'OEBPS/Text/copyright.xhtml': xhtml('<p>Copyright &copy; 2026 Ada Example. All rights reserved.</p>', 'Copyright', 'copyright-page'),
 'OEBPS/Text/part1.xhtml': xhtml('<h1>Part One</h1><h2>Arrival</h2>', 'Part One', 'part'),
 'OEBPS/Text/ch1.xhtml': xhtml('''<section epub:type="chapter"><h1>Chapter 1</h1><h2>The Storm</h2>
<blockquote class="epigraph"><p>The sea remembers.</p><p class="attribution">&#8212; Old saying</p></blockquote>
<p class="noind">The <em>storm</em> came in   at dusk, <span class="ital">loud</span> and <b>cold</b>.</p>
<p>Mara watched from the <span class="sc">harbour</span> wall.<br/>She didn't move.</p>
<p class="hidden">Secret text.</p>
<p class="ctr">* * *</p>
<p>Morning brought quiet. Visit <a href="https://example.com">the site</a> or <a href="ch2.xhtml">chapter two</a>.</p>
<hr/>
<p>Later still.</p><script>alert(1)</script></section>''', 'Chapter 1', 'bodymatter'),
 'OEBPS/Text/ch1b.xhtml': xhtml('<p>The chapter carries on in a second file.</p>', 'Chapter 1 (cont.)'),
 'OEBPS/Text/ch2.xhtml': xhtml('''<p>Before the start anchor.</p><h1 id="start">Chapter 2</h1><p>Two begins.</p>
<h2 id="deep">A subsection</h2><p>Still chapter two.</p><table><tr><td>A</td><td>B</td></tr></table>''', 'Chapter 2'),
 'OEBPS/Text/part2.xhtml': xhtml('<h1>Part Two</h1>', 'Part Two', 'part'),
 'OEBPS/Text/ch3.xhtml': xhtml('<h1>Chapter 3</h1><p>Here is the map.</p><figure><img src="../Images/map%20one.png" alt="A map"/></figure><p>The end.</p>', 'Chapter 3'),
 'OEBPS/Text/ack.xhtml': xhtml('<h1>Acknowledgements</h1><p>Thanks to everyone.</p>', 'Acknowledgements', 'acknowledgments'),
 'OEBPS/Text/about.xhtml': xhtml('<h1>About the Author</h1><p>Ada lives by the sea.</p>', 'About the Author'),
}
write('novel.epub', files)

# 2. EPUB 2 with NCX, everything in one file split by anchors; a font-obfuscation encryption.xml (allowed).
ncx = '''<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap>
<navPoint id="n1"><navLabel><text>One</text></navLabel><content src="book.html#c1"/></navPoint>
<navPoint id="n2"><navLabel><text>Two</text></navLabel><content src="book.html#c2"/></navPoint>
<navPoint id="n3"><navLabel><text>Three</text></navLabel><content src="book.html#c3"/></navPoint></navMap></ncx>'''
opf2 = '''<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:title>Single File</dc:title><dc:creator opf:role="aut" xmlns:opf="http://www.idpf.org/2007/opf">B. Writer</dc:creator></metadata>
<manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/><item id="b" href="book.html" media-type="application/xhtml+xml"/>
<item id="f" href="font.otf" media-type="font/otf"/></manifest><spine toc="ncx"><itemref idref="b"/></spine></package>'''
book = '<html><body><h2 id="c1">One</h2><p>First chapter text.</p><h2 id="c2">Two</h2><p>Second chapter text here.</p><div id="c3"><h2>Three</h2><p>Third.</p></div></body></html>'
enc_font = '''<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container" xmlns:enc="http://www.w3.org/2001/04/xmlenc#">
<enc:EncryptedData><enc:EncryptionMethod Algorithm="http://www.idpf.org/2008/embedding"/><enc:CipherData><enc:CipherReference URI="OEBPS/font.otf"/></enc:CipherData></enc:EncryptedData></encryption>'''
write('single.epub', {'OEBPS/content.opf': opf2, 'OEBPS/toc.ncx': ncx, 'OEBPS/book.html': book, 'OEBPS/font.otf': b'x' * 100,
                      'META-INF/encryption.xml': enc_font})

# 3. DRM: Adobe rights.xml, and real content encryption.
write('drm-adobe.epub', {'OEBPS/content.opf': opf2, 'OEBPS/toc.ncx': ncx, 'OEBPS/book.html': book, 'META-INF/rights.xml': '<rights/>'})
write('drm-encrypted.epub', {'OEBPS/content.opf': opf2, 'OEBPS/toc.ncx': ncx, 'OEBPS/book.html': book,
                             'META-INF/encryption.xml': enc_font.replace('http://www.idpf.org/2008/embedding', 'http://www.w3.org/2001/04/xmlenc#aes128-cbc')})

# 4. A "zip bomb" chapter: tiny when packed, 80 MB unpacked. It must be refused, not unpacked.
opf3 = opf2.replace('<itemref idref="b"/>', '<itemref idref="b"/><itemref idref="bomb"/>').replace(
    '<item id="f"', '<item id="bomb" href="bomb.html" media-type="application/xhtml+xml"/><item id="f"')
write('bomb.epub', {'OEBPS/content.opf': opf3, 'OEBPS/toc.ncx': ncx, 'OEBPS/book.html': book,
                    'OEBPS/bomb.html': '<p>' + 'a' * (80 << 20) + '</p>'})

# 5. Not an EPUB at all.
with zipfile.ZipFile('not-epub.zip', 'w') as z: z.writestr('readme.txt', 'hello')
print('ok')
