"""Regenerate an authored tiny two-master weight fixture (requires fontTools).

No downloaded font, account data or browsing input is used. The generated C
include is committed so ordinary builds do not require Python font tooling.
"""
from pathlib import Path
import tempfile
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.designspaceLib import DesignSpaceDocument, AxisDescriptor, SourceDescriptor
from fontTools.varLib import build

def master(path, weight, optical):
    fb = FontBuilder(1000, isTTF=True)
    names = ['.notdef', 'space', 'A', 'V']
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({32:'space',65:'A',86:'V'})
    glyphs = {}
    for name in names:
        pen = TTGlyphPen(None)
        if name != 'space':
            left, right = 50, 400 + (weight-400)//2 - (optical-14)*3
            pen.moveTo((left,0));pen.lineTo((right,0));pen.lineTo((right,700));pen.lineTo((left,700));pen.closePath()
        glyphs[name] = pen.glyph()
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n:(500+(weight-400)//2-(optical-14)*3,50)for n in names})
    fb.setupHorizontalHeader(ascent=800,descent=-200)
    fb.setupNameTable(dict(familyName='Test Weight',styleName='Regular',uniqueFontIdentifier='AuthoredTestWeight',fullName='Test Weight',psName='TestWeight'))
    fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    fb.setupPost();fb.setupMaxp()
    fb.font['head'].created=fb.font['head'].modified=2082844800
    fb.save(path)

with tempfile.TemporaryDirectory()as directory:
    root=Path(directory);doc=DesignSpaceDocument()
    axis=AxisDescriptor();axis.name='Weight';axis.tag='wght';axis.minimum=400;axis.default=400;axis.maximum=700;doc.addAxis(axis)
    axis=AxisDescriptor();axis.name='Optical';axis.tag='opsz';axis.minimum=14;axis.default=14;axis.maximum=32;doc.addAxis(axis)
    for weight in [400,700]:
        for optical in [14,32]:
            path=root/f'{weight}-{optical}.ttf';master(path,weight,optical)
            source=SourceDescriptor();source.path=str(path);source.name=f'{weight}-{optical}';source.location={'Weight':weight,'Optical':optical};doc.addSource(source)
    font,_,_=build(doc);font.recalcTimestamp=False
    font['head'].created=font['head'].modified=2082844800
    path=root/'variable.ttf';font.save(path);data=path.read_bytes()
    text='/* Authored variable-font fixture; regenerate with tests/tools/generate-variable-font-fixture.py. */\nstatic const unsigned char variable_font_fixture[] = {\n'
    text+=''.join('    '+','.join(f'0x{x:02x}'for x in data[i:i+16])+',\n'for i in range(0,len(data),16))+'};\n'
    (Path(__file__).resolve().parents[1]/'suites/variable_font_fixture.inc').write_text(text)
