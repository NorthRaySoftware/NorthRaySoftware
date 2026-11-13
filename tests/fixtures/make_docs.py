#!/usr/bin/env python3
"""Builds the Word and OpenDocument files the import tests read (needs python-docx and odfpy):
   python3 make_docs.py"""
import io, struct, zlib, zipfile, re
from docx import Document
from docx.shared import Inches
from docx.enum.text import WD_ALIGN_PARAGRAPH

def png(w=3, h=3):
    raw = b''.join(b'\x00' + b'\x00\x80\xff' * w for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

# 1. A structured manuscript: Title, Heading 1 chapters inside a part, styles, a picture, a list, a quote.
d = Document()
d.core_properties.title = 'The Lighthouse Keeper'
d.core_properties.author = 'Ada Example'
d.add_paragraph('The Lighthouse Keeper', style='Title')
d.add_paragraph('A novel by Ada Example')
d.add_heading('Part One: The Coast', level=1)
d.add_heading('Chapter 1: The Lamp', level=1)
p = d.add_paragraph('Mara climbed the ')
p.add_run('hundred').italic = True
p.add_run(' steps before ')
r = p.add_run('dawn'); r.bold = True
p.add_run(', as she did every ')
r = p.add_run('morning'); r.font.small_caps = True
p.add_run('.')
d.add_paragraph('The sea remembers everything.', style='Quote')
d.add_paragraph('She counted three things:')
d.add_paragraph('the lamp', style='List Bullet')
d.add_paragraph('the stairs', style='List Bullet')
d.add_paragraph('the sea', style='List Bullet')
s = d.add_paragraph('* * *'); s.alignment = WD_ALIGN_PARAGRAPH.CENTER
d.add_paragraph('Later, the fog came in.')
d.add_heading('Chapter 2: Strangers', level=1)
d.add_paragraph('A ship arrived on Tuesday.')
d.add_picture(io.BytesIO(png()), width=Inches(1))
d.add_paragraph('The captain asked for Mara by name.')
d.save('manuscript.docx')

# Add a footnote and a hyperlink by editing the XML (python-docx has no API for them).
with zipfile.ZipFile('manuscript.docx') as z:
    files = {n: z.read(n) for n in z.namelist()}
doc = files['word/document.xml'].decode()
doc = doc.replace('A ship arrived on Tuesday.</w:t></w:r>',
    'A ship arrived on Tuesday.</w:t></w:r><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteReference w:id="1"/></w:r>'
    '<w:r><w:t xml:space="preserve"> See </w:t></w:r><w:hyperlink r:id="rIdLink1"><w:r><w:t>the harbour site</w:t></w:r></w:hyperlink>'
    '<w:del w:id="9" w:author="x"><w:r><w:delText>DELETED WORDS</w:delText></w:r></w:del>')
files['word/document.xml'] = doc.encode()
files['word/footnotes.xml'] = (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:footnotes xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
    b'<w:footnote w:id="-1" w:type="separator"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>'
    b'<w:footnote w:id="1"><w:p><w:r><w:t>Tuesdays were for mending nets.</w:t></w:r></w:p></w:footnote></w:footnotes>')
rels = files['word/_rels/document.xml.rels'].decode()
rels = rels.replace('</Relationships>', '<Relationship Id="rIdLink1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.com/harbour" TargetMode="External"/></Relationships>')
files['word/_rels/document.xml.rels'] = rels.encode()
with zipfile.ZipFile('manuscript.docx', 'w', zipfile.ZIP_DEFLATED) as z:
    for n, b in files.items(): z.writestr(n, b)

# 2. A plain manuscript: everything in Normal, chapters typed as "CHAPTER ONE" + title line.
d = Document()
d.add_paragraph('Notes to self: draft three.')
d.add_paragraph('CHAPTER ONE')
d.add_paragraph('The Storm')
p = d.add_paragraph('It was a dark night. ')
p.add_run('Very').italic = True
p.add_run(' dark.')
d.add_paragraph('***')
d.add_paragraph('Morning came.')
d.add_paragraph('CHAPTER TWO')
d.add_paragraph('Clear Skies')
d.add_paragraph('The sun rose over the harbour.')
d.add_paragraph('Epilogue')
d.add_paragraph('Years later, the lamp still burned.')
d.save('plain.docx')

# 3. OpenDocument: headings, an italic span, a Quotations paragraph, a numbered list, a note.
from odf.opendocument import OpenDocumentText
from odf.style import Style, TextProperties, ParagraphProperties
from odf.text import H, P, Span, List, ListItem, Note, NoteCitation, NoteBody
from odf import meta, dc
o = OpenDocumentText()
o.meta.addElement(dc.Title(text='Harbour Lights'))
o.meta.addElement(meta.InitialCreator(text='B. Writer'))
ital = Style(name='T1', family='text'); ital.addElement(TextProperties(fontstyle='italic')); o.automaticstyles.addElement(ital)
quote = Style(name='Quotations', displayname='Quotations', family='paragraph'); o.styles.addElement(quote)
center = Style(name='P1', family='paragraph'); center.addElement(ParagraphProperties(textalign='center')); o.automaticstyles.addElement(center)
o.text.addElement(H(outlinelevel=1, text='Chapter One'))
p = P(text='The lights came on ')
sp = Span(stylename=ital, text='one by one'); p.addElement(sp); p.addText('.')
n = Note(id='ftn1', noteclass='footnote'); n.addElement(NoteCitation(text='1')); nb = NoteBody(); nb.addElement(P(text='A local custom.')); n.addElement(nb)
p.addElement(n)
o.text.addElement(p)
o.text.addElement(P(stylename=quote, text='Every light is a promise.'))
o.text.addElement(P(stylename=center, text='* * *'))
o.text.addElement(P(text='Night fell.'))
o.text.addElement(H(outlinelevel=1, text='Chapter Two'))
o.text.addElement(P(text='The keeper slept.'))
o.save('harbour.odt')
print('ok')
