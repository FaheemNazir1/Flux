"""Generates the small test documents in this folder (tests/fixtures).

The C++ tests only need the generated files; this script is kept so they can be regenerated and so that
every unique test word is documented in one place. Python 3 standard library only. Deterministic output.

    python tests/fixtures/make_fixtures.py
"""
import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIXED_TIME = (2020, 1, 1, 0, 0, 0)


def write(name, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(os.path.join(HERE, name), mode, **({} if mode == "wb" else {"encoding": "utf-8", "newline": ""})) as f:
        f.write(data)


def make_zip(name, parts, compress=True):
    with zipfile.ZipFile(os.path.join(HERE, name), "w") as z:
        for part_name, content in parts:
            info = zipfile.ZipInfo(part_name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
            z.writestr(info, content if isinstance(content, bytes) else content.encode("utf-8"))


# ----------------------------------------------------------------------------------------------- PDF
def pdf_escape(text):
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def build_pdf(objects, trailer_extra=""):
    """objects: {number: bytes body}. Classic cross-reference table."""
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for number in sorted(objects):
        offsets[number] = len(out)
        out += b"%d 0 obj\n" % number + objects[number] + b"\nendobj\n"
    xref = len(out)
    size = max(objects) + 1
    out += b"xref\n0 %d\n" % size + b"0000000000 65535 f \n"
    for number in range(1, size):
        if number in offsets:
            out += b"%010d 00000 n \n" % offsets[number]
        else:
            out += b"0000000000 65535 f \n"
    out += b"trailer\n<< /Size %d /Root 1 0 R %s>>\nstartxref\n%d\n%%%%EOF\n" % (size, trailer_extra.encode(), xref)
    return bytes(out)


def stream_obj(data, compress=True, extra=""):
    if compress:
        data = zlib.compress(data, 9)
        return b"<< /Length %d /Filter /FlateDecode %s>>\nstream\n" % (len(data), extra.encode()) + data + b"\nendstream"
    return b"<< /Length %d %s>>\nstream\n" % (len(data), extra.encode()) + data + b"\nendstream"


def classic_pdf(pages, compress=True, trailer_extra="", info=None, extra_objects=None):
    objects = {1: b"<< /Type /Catalog /Pages 2 0 R >>", 3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"}
    kids = []
    for index, lines in enumerate(pages):
        page_no, content_no = 4 + 2 * index, 5 + 2 * index
        kids.append("%d 0 R" % page_no)
        text = "BT /F1 12 Tf 72 720 Td 14 TL\n" + "".join("(%s) Tj T*\n" % pdf_escape(line) for line in lines) + "ET"
        objects[page_no] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>" % content_no)
        objects[content_no] = stream_obj(text.encode("latin-1"), compress)
    objects[2] = ("<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join(kids), len(pages))).encode()
    if extra_objects:
        objects.update(extra_objects)
    if info:
        number = max(objects) + 1
        objects[number] = ("<< /Title (%s) >>" % pdf_escape(info)).encode()
        trailer_extra += "/Info %d 0 R " % number
    return build_pdf(objects, trailer_extra)


def cid_pdf():
    """PDF 1.5: object stream + cross-reference stream + Type0 / Identity-H font with a ToUnicode map."""
    alphabet = "abcdefghijklmnopqrstuvwxyz "
    code = {ch: i + 1 for i, ch in enumerate(alphabet)}

    def hexstr(text):
        return "<" + "".join("%04X" % code[c] for c in text) + ">"

    bf = "".join("<%04X> <%04X>\n" % (code[c], ord(c)) for c in alphabet)
    cmap = ("/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n"
            "1 begincodespacerange <0000> <FFFF> endcodespacerange\n%d beginbfchar\n%sendbfchar\nendcmap end end" % (len(alphabet), bf))
    pages_text = ["flxcidone hello world", "flxcidword second page"]
    contents = ["BT /F1 14 Tf 72 700 Td %s Tj ET" % hexstr(t) for t in pages_text]

    inner = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 8 0 R >>",
        4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 9 0 R >>",
        5: b"<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 7 0 R >>",
        6: b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /DW 600 /CIDToGIDMap /Identity >>",
    }
    header, body, pos = "", b"", 0
    for number in sorted(inner):
        header += "%d %d " % (number, pos)
        chunk = inner[number] + b"\n"
        body += chunk
        pos += len(chunk)
    header_bytes = header.encode()
    objstm_data = header_bytes + body
    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}

    def add(number, content):
        offsets[number] = len(out)
        out.extend(b"%d 0 obj\n" % number + content + b"\nendobj\n")

    add(7, stream_obj(cmap.encode()))
    add(8, stream_obj(contents[0].encode()))
    add(9, stream_obj(contents[1].encode()))
    add(10, stream_obj(objstm_data, extra="/Type /ObjStm /N %d /First %d " % (len(inner), len(header_bytes))))
    xref_offset = len(out)
    entries = bytearray()
    for number in range(0, 12):
        if number == 0:
            entries += struct.pack(">BHB", 0, 0, 255)[:0] + bytes([0, 0, 0, 0, 255])
        elif number in inner:
            entries += bytes([2, 0, 0, 10, sorted(inner).index(number)])
        elif number in offsets:
            entries += bytes([1]) + offsets[number].to_bytes(3, "big") + bytes([0])
        else:  # 11: the xref stream itself
            entries += bytes([1]) + xref_offset.to_bytes(3, "big") + bytes([0])
    xref_stream = zlib.compress(bytes(entries))
    out += (b"11 0 obj\n<< /Type /XRef /Size 12 /W [1 3 1] /Root 1 0 R /Length %d /Filter /FlateDecode >>\nstream\n" % len(xref_stream)
            + xref_stream + b"\nendstream\nendobj\nstartxref\n%d\n%%%%EOF\n" % xref_offset)
    return bytes(out)


def type3_pdf():
    """Each glyph is placed by its own move and the Type 3 font's widths are in 1/2048 units (FontMatrix): a reader that
    took the widths as 1/1000 em would see gaps inside the word. Browser-made PDFs look like this."""
    word = "flxtypethreeword"
    letters = sorted(set(word))
    differences = " ".join("%d /%s" % (ord(c), c) for c in letters)
    widths = " ".join("1100" if chr(c) in letters else "0" for c in range(97, 123))
    procs = {i: stream_obj(b"1100 0 d0\n", compress=False) for i in range(10, 10 + len(letters))}
    char_procs = " ".join("/%s %d 0 R" % (c, 10 + i) for i, c in enumerate(letters))
    advance = 1100 * 0.00048828125 * 12  # true advance in points
    text = "BT /F1 12 Tf 72 700 Td\n" + "".join("(%s) Tj %.4f 0 Td\n" % (c, advance) for c in word) + "ET"
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [4 0 R] /Count 1 >>",
        3: ("<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.00048828125 0 0 0.00048828125 0 0] /CharProcs << %s >> "
            "/Encoding << /Type /Encoding /Differences [%s] >> /FirstChar 97 /LastChar 122 /Widths [%s] >>" % (char_procs, differences, widths)).encode(),
        4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents 5 0 R >>",
        5: stream_obj(text.encode("latin-1"), compress=False),
    }
    objects.update(procs)
    return build_pdf(objects)


def scanned_pdf():
    image = zlib.compress(bytes(range(256)) * 4)
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>",
        4: b"<< /Type /XObject /Subtype /Image /Width 32 /Height 32 /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length %d >>\nstream\n" % len(image) + image + b"\nendstream",
        5: stream_obj(b"q 100 0 0 100 0 0 cm /Im0 Do Q"),
    }
    return build_pdf(objects)


def main():
    pdf = classic_pdf([["flxpdfone first page", "Introduction text"], ["flxpdftwo second page", "Faheem Nazir appears here"], ["flxpdfthree third page"]],
                      info="flxpdftitle")
    write("sample.pdf", pdf)
    write("sample_uncompressed.pdf", classic_pdf([["flxplainpdf page one"]], compress=False))
    write("sample_objstm.pdf", cid_pdf())
    write("scanned.pdf", scanned_pdf())
    write("type3.pdf", type3_pdf())
    write("empty.pdf", classic_pdf([[]]))
    write("encrypted.pdf", classic_pdf([["flxsecretpdf"]], trailer_extra="/Encrypt 20 0 R ").replace(b"\nxref\n", b"\n<< /Filter /Standard /V 1 /R 2 >>\nxref\n", 1))
    write("truncated.pdf", pdf[: len(pdf) * 6 // 10])
    write("garbage.pdf", b"%PDF-1.4\nthis is not really a pdf \x00\x01\x02" * 4)

    # ---- DOCX: heading, paragraphs, table, header, footer, rendered page break, core properties
    w = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
    docx_body = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document %s><w:body>'
        '<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Chapter flxdocxheading</w:t></w:r></w:p>'
        '<w:p><w:r><w:t xml:space="preserve">Plain paragraph with flxdocxbody and </w:t></w:r><w:r><w:t>Faheem</w:t></w:r></w:p>'
        '<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Cell flxdocxtable</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>second cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>'
        '<w:p><w:r><w:lastRenderedPageBreak/><w:t>flxdocxpagetwo starts the second page</w:t></w:r></w:p>'
        '<w:p><w:r><w:delText>flxdocxdeleted</w:delText></w:r></w:p>'
        '</w:body></w:document>' % w)
    docx_parts = [
        ("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="xml" ContentType="application/xml"/></Types>'),
        ("_rels/.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"/>'),
        ("word/document.xml", docx_body),
        ("word/header1.xml", '<w:hdr %s><w:p><w:r><w:t>flxdocxheader</w:t></w:r></w:p></w:hdr>' % w),
        ("word/footnotes.xml", '<w:footnotes %s><w:footnote><w:p><w:r><w:t>flxdocxfootnote</w:t></w:r></w:p></w:footnote></w:footnotes>' % w),
        ("docProps/core.xml", '<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>flxdocxtitle</dc:title><dc:creator>Test Author</dc:creator></cp:coreProperties>'),
    ]
    make_zip("sample.docx", docx_parts)
    make_zip("empty.docx", [docx_parts[0], docx_parts[1], ("word/document.xml", '<w:document %s><w:body><w:p/></w:body></w:document>' % w)])
    write("locked.docx", bytes([0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1]) + b"EncryptedPackage" + bytes(600))
    write("corrupt.docx", bytes((i * 37 + 11) % 251 for i in range(400)))
    write("truncated.docx", open(os.path.join(HERE, "sample.docx"), "rb").read()[:300])

    # ---- PPTX: three slides stored in a different order than their file names, with notes on one
    a = 'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'

    def slide(text, table=None):
        shapes = '<p:sp><p:txBody><a:p><a:r><a:t>%s</a:t></a:r></a:p></p:txBody></p:sp>' % text
        if table:
            shapes += '<p:graphicFrame><a:graphic><a:graphicData><a:tbl><a:tr><a:tc><a:txBody><a:p><a:r><a:t>%s</a:t></a:r></a:p></a:txBody></a:tc></a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>' % table
        return '<p:sld %s><p:cSld><p:spTree>%s</p:spTree></p:cSld></p:sld>' % (a, shapes)

    rels = lambda items: '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">%s</Relationships>' % "".join(items)
    make_zip("sample.pptx", [
        ("[Content_Types].xml", docx_parts[0][1]),
        ("ppt/presentation.xml", '<p:presentation %s><p:sldIdLst><p:sldId id="256" r:id="rId1"/><p:sldId id="257" r:id="rId3"/><p:sldId id="258" r:id="rId2"/></p:sldIdLst></p:presentation>' % a),
        ("ppt/_rels/presentation.xml.rels", rels(['<Relationship Id="rId1" Target="slides/slide1.xml"/>', '<Relationship Id="rId2" Target="slides/slide2.xml"/>', '<Relationship Id="rId3" Target="slides/slide3.xml"/>'])),
        ("ppt/slides/slide1.xml", slide("flxslideone title")),
        ("ppt/slides/slide2.xml", slide("flxslidetwo body", table="flxslidetable")),
        ("ppt/slides/slide3.xml", slide("flxslidethree body")),
        ("ppt/slides/_rels/slide2.xml.rels", rels(['<Relationship Id="rId1" Target="../notesSlides/notesSlide1.xml"/>'])),
        ("ppt/notesSlides/notesSlide1.xml", '<p:notes %s><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>flxslidenotes speaker text</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:notes>' % a),
    ])

    # ---- XLSX: shared strings, an inline string, numbers, a formula with and without a cached value, two sheets
    x = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
    make_zip("sample.xlsx", [
        ("[Content_Types].xml", docx_parts[0][1]),
        ("xl/workbook.xml", '<workbook %s><sheets><sheet name="Students" sheetId="1" r:id="rId1"/><sheet name="Totals" sheetId="2" r:id="rId2"/></sheets></workbook>' % x),
        ("xl/_rels/workbook.xml.rels", rels(['<Relationship Id="rId1" Target="worksheets/sheet1.xml"/>', '<Relationship Id="rId2" Target="worksheets/sheet2.xml"/>'])),
        ("xl/sharedStrings.xml", '<sst %s><si><t>Name</t></si><si><t>flxxlsxname</t></si><si><r><t>Faheem </t></r><r><t>Nazir</t></r></si></sst>' % x),
        ("xl/worksheets/sheet1.xml", '<worksheet %s><sheetData>'
         '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="inlineStr"><is><t>Score</t></is></c></row>'
         '<row r="2"><c r="A2" t="s"><v>1</v></c><c r="B2"><v>91</v></c><c r="C2"><f>SUM(B2:B3)</f></c></row>'
         '<row r="3"><c r="A3" t="s"><v>2</v></c><c r="B3" t="inlineStr"><is><t>flxxlsxinline</t></is></c><c r="C3"><f>B2*2</f><v>182</v></c></row>'
         '</sheetData></worksheet>' % x),
        ("xl/worksheets/sheet2.xml", '<worksheet %s><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>flxxlsxtotals</t></is></c></row></sheetData></worksheet>' % x),
    ])

    # ---- OpenDocument
    o = 'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"'
    manifest = '<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"/>'
    make_zip("sample.odt", [("mimetype", "application/vnd.oasis.opendocument.text"), ("META-INF/manifest.xml", manifest),
                            ("content.xml", '<office:document-content %s><office:body><office:text><text:h>Title flxodtheading</text:h><text:p>Body flxodtbody <text:s/>text</text:p></office:text></office:body></office:document-content>' % o),
                            ("meta.xml", '<office:document-meta xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:dc="http://purl.org/dc/elements/1.1/"><office:meta><dc:title>flxodttitle</dc:title></office:meta></office:document-meta>')])
    make_zip("sample.ods", [("META-INF/manifest.xml", manifest),
                            ("content.xml", '<office:document-content %s><office:body><office:spreadsheet><table:table table:name="Budget"><table:table-row><table:table-cell><text:p>flxodscell</text:p></table:table-cell><table:table-cell><text:p>42</text:p></table:table-cell></table:table-row></table:table></office:spreadsheet></office:body></office:document-content>' % o)])
    make_zip("sample.odp", [("META-INF/manifest.xml", manifest),
                            ("content.xml", '<office:document-content %s><office:body><office:presentation><draw:page draw:name="a"><text:p>flxodpslideone</text:p></draw:page><draw:page draw:name="b"><text:p>flxodpslidetwo</text:p></draw:page></office:presentation></office:body></office:document-content>' % o)])

    # ---- text-like formats
    write("sample.rtf", "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f0 fontnamejunk;}}{\\colortbl;\\red0\\green0\\blue0;}\\f0\\fs24 Hello flxrtfword caf\\'e9 and \\u8364? sign.\\par Second paragraph Faheem.\\par}")
    write("sample.html", "<!DOCTYPE html><html><head><title>flxhtmltitle</title><style>.x{color:red} flxhtmlstyle</style><script>var s='flxhtmlscript';</script></head>"
                         "<body><h1>Heading flxhtmlheading</h1><p>Paragraph&nbsp;with flxhtmlbody &amp; Faheem.</p><!-- flxhtmlcomment --></body></html>")
    write("sample.eml", "From: =?utf-8?B?RmFoZWVtIE5hemly?= <f@example.com>\r\nTo: x@example.com\r\nSubject: =?utf-8?B?Zmx4ZW1sc3ViamVjdA==?=\r\n"
                        "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"BOUND\"\r\n\r\n--BOUND\r\nContent-Type: text/plain; charset=utf-8\r\n"
                        "Content-Transfer-Encoding: quoted-printable\r\n\r\nBody flxemlbody caf=C3=A9 and a very long line that is wrapped with a soft break=\r\n continues here.\r\n"
                        "--BOUND\r\nContent-Type: text/plain; name=\"flxattach.txt\"\r\nContent-Disposition: attachment; filename=\"flxattach.txt\"\r\n\r\nattachment body\r\n--BOUND--\r\n")
    make_zip("sample.epub", [("mimetype", "application/epub+zip"),
                             ("OEBPS/chapter1.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml"><body><h1>One</h1><p>flxepubone text</p></body></html>'),
                             ("OEBPS/chapter2.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>flxepubtwo text</p></body></html>')])
    make_zip("archive.zip", [("inner.docx", open(os.path.join(HERE, "sample.docx"), "rb").read()), ("note.txt", "flxziptext inside an archive"), ("data.bin", bytes(range(256)))])
    write("unicode_utf16.txt", b"\xff\xfe" + "flxutfsixteen café 日本語".encode("utf-16-le"))
    write("unicode_utf8.txt", "flxutfeight Müller 中文".encode("utf-8"))
    write("latin1.txt", b"flxlatin caf\xe9 r\xe9sum\xe9")
    write("source.py", "def flxsourcefunc():\n    return 'Faheem'\n")
    write("data.csv", "name,score\nflxcsvname,10\nFaheem,20\n")


if __name__ == "__main__":
    main()
    print("fixtures written to", HERE)
