#!/usr/bin/env python3
"""Build the local illustrated Q2PSX design reference from text and local assets.

Requires reportlab, Pillow and pypdf. No commercial media is embedded in this
source file. The ignored .branding/design directory holds the rendered output.
Use --prepare with the retained source evidence to regenerate image derivatives.
Original scans are never modified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import io
from pathlib import Path
from xml.sax.saxutils import escape

from PIL import Image
from pypdf import PdfReader, PdfWriter
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader
from reportlab.platypus import Paragraph, Table, TableStyle

ROOT = Path(__file__).resolve().parents[1]
W, H = 595.276, 841.89
M, CW = 42.52, 510.236
PAPER, INK, WARM, ORANGE = '#EDF1FA', '#141B2E', '#8C3F1D', '#EF8A3B'
VOID, STEEL, BLUE = '#080B18', '#A7B9D7', '#263D85'
NAVY, VIOLET, CYAN = '#101932', '#3E315E', '#8BD6F2'
CONTRAST_PAIRS = [('white','navy'),('steel','panel'),('orange','navy'),
                  ('ink','orange'),('printOrange','paper'),('orange','paper')]


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def luminance(value):
    rgb = [int(value[i:i+2], 16) / 255 for i in (1, 3, 5)]
    lin = [c / 12.92 if c <= 0.04045 else ((c + .055) / 1.055) ** 2.4 for c in rgb]
    return sum(a*b for a, b in zip(lin, (.2126, .7152, .0722)))


def contrast(a, b):
    la, lb = sorted((luminance(a), luminance(b)))
    return (lb + .05) / (la + .05)


def prepare_assets(output, entries):
    assets = output / 'assets'
    assets.mkdir(parents=True, exist_ok=True)
    register = []
    for entry in entries:
        src = ROOT / entry['source']
        if not src.is_file():
            raise FileNotFoundError(f'Missing {entry["id"]} input: {src}')
        im = Image.open(src).convert('RGB')
        source_size = list(im.size)
        crop = None
        if 'crop' in entry:
            a, b, d, e = entry['crop']
            crop = [round(a*im.width), round(b*im.height), round(d*im.width), round(e*im.height)]
        elif 'cropPixels' in entry:
            crop = entry['cropPixels']
        if crop:
            if not (0 <= crop[0] < crop[2] <= im.width and 0 <= crop[1] < crop[3] <= im.height):
                raise ValueError(f'Crop out of bounds: {entry["id"]}')
            im = im.crop(crop)
        dest = assets / f'{entry["id"]}.png'
        im.save(dest, optimize=True)
        rec = dict(entry)
        rec.update(source_sha256=digest(src), source_dimensions=source_size,
                   output=f'assets/{entry["id"]}.png', output_sha256=digest(dest),
                   output_dimensions=list(im.size), crop_pixels=crop,
                   prepared_date='2026-10-28')
        if entry.get('original'):
            rec['original_sha256'] = digest(ROOT / entry['original'])
        register.append(rec)
    # Extra raw evidence is retained for the pixel-comparison figure.
    raw = output / 'evidence/SECURITY-raw.png'
    Image.open(raw).save(assets / 'SECURITY-raw.png')
    # A recorded crop of actual cool scene evidence gives the new cover its
    # primary atmosphere. The complete image and its HUD remain in atlas A13.
    im = Image.open(assets/'A13.png').convert('RGB')
    im.crop((0,0,1024,600)).save(assets/'cover-art.png')
    im = Image.open(assets/'A17.png')
    im.crop((0,100,240,144)).save(assets/'menu-item-strip.png')
    extras = [
        {'output':'assets/cover-art.png','source':'.branding/design/evidence/SECURITY.png','crop_pixels':[0,0,1024,600]},
        {'output':'assets/menu-item-strip.png','source':'.branding/design/evidence/menu.ppm.frontend.png','crop_pixels':[0,100,240,144]},
        {'output':'assets/SECURITY-raw.png','source':'.branding/design/evidence/SECURITY-raw.png','crop_pixels':None},
    ]
    for e in extras:
        e.update(source_sha256=digest(ROOT/e['source']),output_sha256=digest(output/e['output']),
                 output_dimensions=list(Image.open(output/e['output']).size))
    write_json(output / 'asset-register.json', {'version':'1.0','entries':register,'supporting_images':extras})
    return register


class Book:
    def __init__(self, output, guide, tokens, entries):
        self.output, self.guide, self.tokens = output, guide, tokens
        self.entries = {e['id']:e for e in entries}
        self.pdf_images = {}
        self.c = canvas.Canvas(str(output/'Q2PSX-Design-Guide.pdf'), pagesize=(W,H), pageCompression=1)
        self.c.setTitle('Q2PSX Design Reference')
        self.c.setAuthor('Q2PSX-PC project reference')
        self.c.setSubject('Artwork, supporting documents and an indexed design atlas')
        self.c.setKeywords('Q2PSX-PC, PlayStation, Quake II, artwork, design atlas')
        self.bottoms = []

    def rect(self,x,y,w,h,fill,stroke=None,width=.5):
        c=self.c
        c.setFillColor(colors.HexColor(fill))
        if stroke: c.setStrokeColor(colors.HexColor(stroke)); c.setLineWidth(width)
        c.rect(x,H-y-h,w,h,fill=1,stroke=bool(stroke))

    def line(self,x1,y1,x2,y2,color=None,width=.6):
        color=color or BLUE
        self.c.setStrokeColor(colors.HexColor(color)); self.c.setLineWidth(width)
        self.c.line(x1,H-y1,x2,H-y2)

    def text(self,t,x,y,w,size=10.5,leading=15,font='Body',color=None):
        color=color or INK
        s=ParagraphStyle('p',fontName=font,fontSize=size,leading=leading,textColor=colors.HexColor(color),
                         alignment=TA_LEFT,spaceAfter=0,allowWidows=0,allowOrphans=0)
        p=Paragraph(escape(t).replace('\n','<br/>'),s)
        _,ph=p.wrap(w,H)
        p.drawOn(self.c,x,H-y-ph)
        return y+ph

    def image(self,ident,x,y,w,h,mode='fit',matte=None):
        p=self.output/'assets'/f'{ident}.png'
        if not p.is_file(): raise FileNotFoundError(p)
        iw,ih=Image.open(p).size
        s=min(w/iw,h/ih) if mode=='fit' else max(w/iw,h/ih)
        dw,dh=iw*s,ih*s
        if matte:self.rect(x,y,w,h,matte)
        self.c.saveState()
        clip=self.c.beginPath(); clip.rect(x,H-y-h,w,h); self.c.clipPath(clip,stroke=0,fill=0)
        # Embed compact print-resolution derivatives of scans. The full-resolution
        # prepared PNG and the byte-original archive remain available locally.
        original = self.entries.get(ident,{}).get('evidence') == 'Original'
        image_source = str(p)
        if original:
            target_w = max(1, math.ceil(dw*220/72))
            target_h = max(1, math.ceil(dh*220/72))
            cache_key = (ident,target_w,target_h)
            if cache_key not in self.pdf_images:
                im=Image.open(p).convert('RGB')
                im.thumbnail((target_w,target_h),Image.Resampling.LANCZOS)
                buffer=io.BytesIO(); im.save(buffer,format='JPEG',quality=94,subsampling=0)
                buffer.seek(0); self.pdf_images[cache_key]=ImageReader(buffer)
            image_source=self.pdf_images[cache_key]
        self.c.drawImage(image_source,x+(w-dw)/2,H-y-h+(h-dh)/2,dw,dh,mask='auto')
        self.c.restoreState()

    def label(self,t,x,y,w,color=None):
        color=color or BLUE
        return self.text(t,x,y,w,8.2,11,'Bold',color)

    def pair(self,ids,y,h,labels=None):
        gap=16; cell=(CW-gap)/len(ids)
        for i,ident in enumerate(ids):
            x=M+i*(cell+gap)
            self.image(ident,x,y,cell,h,matte=VOID)
            self.label(labels[i] if labels else ident,x,y+h+7,cell)
        return y+h+28

    def gridlines(self,x,y,w,h,step=18,color='#CAD2E3'):
        for i in range(int(w/step)+1):self.line(x+i*step,y,x+i*step,y+h,color,.35)
        for i in range(int(h/step)+1):self.line(x,y+i*step,x+w,y+i*step,color,.35)

    def table(self,data,y,width=CW):
        heads=data['heads']; rows=data['rows']; n=len(heads)
        if n==2: widths=[width*.29,width*.71]
        else: widths=[width*.24,width*.32,width*.44]
        def p(t,bold=False,white=False):
            return Paragraph(escape(t),ParagraphStyle('cell',fontName='Bold' if bold else 'Body',fontSize=9,
                leading=12,textColor=colors.white if white else colors.HexColor(INK)))
        table=Table([[p(t,True,True) for t in heads]]+[[p(t) for t in r] for r in rows],colWidths=widths)
        table.setStyle(TableStyle([
            ('BACKGROUND',(0,0),(-1,0),colors.HexColor(NAVY)),
            ('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.HexColor('#F8FAFF'),colors.HexColor('#E1E7F2')]),
            ('GRID',(0,0),(-1,-1),.4,colors.HexColor('#D9D9D9')),
            ('LEFTPADDING',(0,0),(-1,-1),9),('RIGHTPADDING',(0,0),(-1,-1),9),
            ('TOPPADDING',(0,0),(-1,-1),8),('BOTTOMPADDING',(0,0),(-1,-1),8),
            ('VALIGN',(0,0),(-1,-1),'MIDDLE')]))
        _,hh=table.wrap(width,H);table.drawOn(self.c,M,H-y-hh)
        return y+hh+20

    def blocks(self,blocks,y):
        gap=19.84; col=(CW-gap)/2
        for row in range(0,len(blocks),2):
            ends=[]
            for j,b in enumerate(blocks[row:row+2]):
                x=M+j*(col+gap)
                yy=self.text(b['h'],x,y,col,11.5,14,'Bold')+6
                yy=self.text(b['t'],x,yy,col,10.5,15)
                ends.append(yy)
            y=max(ends)+18
        return y

    def atlas_card(self,entry,x,y,w,ih,small=False):
        self.image(entry['id'],x,y,w,ih,matte=VOID if entry['evidence']=='Observed' else '#DFE4EE')
        yy=y+ih+7
        yy=self.text(entry['id']+'  '+entry['title'],x,yy,w,10 if not small else 9.3,12.5,'Bold')+3
        yy=self.text(entry['evidence'].upper()+'  /  '+entry.get('credit','Q2PSX-PC renderer'),x,yy,w,6.8,9,'Body',WARM)+5
        yy=self.text(entry['use'],x,yy,w,8.5 if not small else 8,11.5 if not small else 10.6)+4
        yy=self.text(entry['limit'],x,yy,w,7.9 if not small else 7.6,10.5,'Body','#4E5759')
        return yy

    def atlas(self,p,y):
        ids=p['atlas']; cols=3 if p['visual']=='atlas_six' else 2
        gap=18; cell=(CW-gap*(cols-1))/cols
        if p['visual']=='atlas_two': ih=280
        elif cols==3: ih=135
        else: ih=168
        for row in range(0,len(ids),cols):
            ends=[self.atlas_card(self.entries[key],M+j*(cell+gap),y,cell,ih,cols==3) for j,key in enumerate(ids[row:row+cols])]
            y=max(ends)+20
        if p.get('blocks'):y=self.blocks(p['blocks'],y+3)
        return y

    def mini_doc(self,x,y,w,h,title,kind='body'):
        self.rect(x,y,w,h,'#F8FAFF','#BCC8E0')
        self.label('Q2PSX-PC',x+12,y+12,w-24)
        self.text(title,x+12,y+29,w-24,17,18,'Display')
        if kind=='image':
            self.image('A13',x+12,y+62,w-24,h*.33,mode='fill')
            start=y+76+h*.33
        elif kind=='grid':
            self.gridlines(x+12,y+67,w-24,h*.29,12)
            self.image('A20',x+15,y+70,w-30,h*.26)
            start=y+80+h*.29
        else:start=y+68
        for j in range(5):
            self.rect(x+12,start+j*9,(w-24)*(.94 if j%3 else .74),2.6,'#A2A6A2')
        self.text('Layout specimen',x+12,y+h-22,w-24,6.5,8,'Body','#646A69')

    def visual(self,p,y):
        v=p['visual']
        if 'table' in p:
            return self.table(p['table'],y)
        if v=='contents':
            cell=(CW-24)/2
            pages=self.guide['pages'][1:]
            rows=math.ceil(len(pages)/2)
            for i,page in enumerate(pages):
                col=i//rows; row=i%rows; x=M+col*(cell+24); yy=y+row*16
                self.text(f'{i+2:02d}',x,yy,22,8.5,11,'Bold',WARM)
                self.text(page['title'],x+26,yy,cell-26,8.6,11,'Body')
                self.c.linkRect('',f'p{i+2:02d}',(x,H-yy-13,x+cell,H-yy),relative=0,thickness=0)
            return y+rows*16+20
        if v=='modes':
            col=(CW-24)/3
            for j,(key,title) in enumerate([('A29','COOL SCENE'),('A09','ORANGE CAMPAIGN'),('A07','ORDERED DOCUMENT')]):
                self.image(key,M+j*(col+12),y,col,174,matte=VOID)
                self.label(title,M+j*(col+12),y+183,col)
            return y+213
        if v=='heritage':return self.pair(['A01','A02','A05'],y,182,['PAL COVER','US COVER','PAL MANUAL'])
        if v=='identity':
            self.rect(M,y,CW,174,NAVY)
            self.rect(M+CW-85,y,85,174,VIOLET)
            self.text('Q2PSX-PC',M+24,y+25,CW-48,40,46,'Menu','#F4F6FF')
            self.text('A native PC recreation of Quake II for the PlayStation',M+26,y+82,CW-52,10.5,15,'Body',STEEL)
            self.label('PROJECT IDENTIFIER',M+26,y+135,220,CYAN)
            self.text('Artwork reference   /   Version 1.0',M+26,y+111,CW-52,9,12,'Mono',ORANGE)
            self.label('Keep a quiet field around the identifier; historical marks stay distinct.',M,y+184,CW)
            return y+213
        if v=='palette':
            keys=['void','navy','panel','violet','blue','cyan','steel','orange','rust','paper','ink','printOrange']
            gap=9; cell=(CW-3*gap)/4
            for i,key in enumerate(keys):
                token=self.tokens['colors'][key]; xx=M+(i%4)*(cell+gap); yy=y+(i//4)*75
                self.rect(xx,yy,cell,40,token['value'],'#C9C5BA')
                self.text(key.upper(),xx,yy+44,cell,7.8,10,'Bold')
                self.text(token['value'],xx,yy+56,cell,8.3,11,'Mono',WARM)
            return y+237
        if v=='contrast':
            pairs=CONTRAST_PAIRS
            cell=(CW-14)/2
            for i,(fg,bg) in enumerate(pairs):
                xx=M+(i%2)*(cell+14); yy=y+(i//2)*62
                a=self.tokens['colors'][fg]['value']; b=self.tokens['colors'][bg]['value']; r=contrast(a,b)
                self.rect(xx,yy,cell,50,b,'#D9D9D9')
                self.text(f'{fg} on {bg}',xx+10,yy+7,cell-20,9.4,12,'Bold',a)
                self.text(f'{r:.2f}:1  /  '+('body target met' if r>=4.5 else 'accent only'),xx+10,yy+26,cell-20,9,12,'Mono',a)
            return y+204
        if v=='type':
            self.rect(M,y,CW,126,NAVY)
            self.text('Q2PSX-PC',M+17,y+10,CW-34,35,42,'Menu','#F4F6FF')
            self.text('BLUE LIGHT. ORANGE SIGNAL.',M+18,y+66,CW-36,18,22,'Advert',ORANGE)
            self.text('HUD  100  125  46',M+19,y+100,CW-38,14,18,'HUD',CYAN)
            self.text('Reading: Arial / Headings: Arial Narrow Bold',M,y+140,CW,10.5,15)
            self.text('BASE0  |  --headless  |  512 x 248',M,y+164,CW,10,14,'Mono',WARM)
            self.label('LOCAL RECONSTRUCTIONS ABOVE / READING FACES BELOW',M,y+192,CW)
            return y+219
        if v=='grid':
            self.rect(M,y,166,206,'#F8FAFF','#BCC8E0')
            for j in range(2):self.rect(M+13+j*73,y+18,65,169,'#DDE4F2')
            self.line(M+13,y+52,M+153,y+52,BLUE,1)
            self.label('A4 / TWO COLUMNS',M,y+214,180)
            xx=M+193; self.rect(xx,y+30,317,178,NAVY)
            self.gridlines(xx+11,y+41,295,156,24.6,'#304362')
            self.text('Q2PSX-PC',xx+23,y+65,260,25,28,'Menu','#F4F6FF')
            self.text('TITLE FIELD',xx+23,y+110,145,8,10,'Mono',STEEL)
            self.label('16:9 / TWELVE COLUMNS',xx,y+214,317)
            return y+243
        if v=='materials':
            cell=(CW-24)/3
            for i,key in enumerate(['A21','A22','A23','A24','A25','A26']):
                xx=M+(i%3)*(cell+12); yy=y+(i//3)*103
                self.image(key,xx,yy,cell,78,mode='fill',matte=VOID)
                self.label(key+'  '+self.entries[key]['title'],xx,yy+83,cell)
            return y+222
        if v=='subjects':return self.pair(['A01','A19','A20'],y,195,['COVER MASS','MODEL SILHOUETTE','WEAPON CONSTRUCTION'])
        if v=='pixels':
            self.image('SECURITY-raw',M,y,246,122,matte=VOID)
            self.label('RAW 512 x 248',M,y+131,246)
            self.image('A13',M+266,y,244,183,matte=VOID)
            self.label('DISPLAY 1024 x 768',M+266,y+191,244)
            self.text('Source pixels are not square in the intended PAL display presentation.',M,y+158,239,9.3,13)
            return y+221
        if v=='flares':
            self.image('A32',M,y,CW,145,matte=VOID)
            self.label('A32 / NATIVE FLARE DETAIL / BLOOM OFF',M,y+151,CW)
            # A labelled conceptual diagram, separate from the actual image.
            xx=M+24; yy=y+194
            self.line(xx,yy,xx+452,yy,BLUE,.8)
            # Style 1's five ghosts, positioned from its documented 1.12
            # offsets. Radius is schematic; preserve the six-sided vocabulary.
            for offset,r in [(-5120,6),(-3800,10),(-1024,5),(1536,4),(6144,6)]:
                pos=226+(355-226)*offset/4096
                self.c.setStrokeColor(colors.HexColor(BLUE));self.c.setLineWidth(.7)
                path=self.c.beginPath()
                for k in range(6):
                    angle=math.pi/2+k*math.pi/3
                    px,py=xx+pos+math.cos(angle)*r,H-yy+math.sin(angle)*r
                    if k==0:path.moveTo(px,py)
                    else:path.lineTo(px,py)
                path.close();self.c.drawPath(path,stroke=1,fill=0)
            self.line(xx+226,yy-9,xx+226,yy+9,WARM,1)
            for k in range(8):
                angle=k*math.pi/4
                self.line(xx+355,yy,xx+355+math.cos(angle)*14,yy+math.sin(angle)*14,WARM,.8)
            self.label('GHOST AXIS',xx,y+211,150)
            self.label('FRAME CENTER',xx+180,y+211,130)
            self.label('LIGHT CORE',xx+323,y+211,120,WARM)
            self.text('Style-1 axis diagram; effect sizes are schematic.',M,y+232,CW,8,11,'Body',BLUE)
            return y+261
        if v=='interface':return self.pair(['A15','A16'],y,185,['SELECTED ROW','MISSION PANEL'])
        if v=='capture':
            self.rect(M,y,CW,155,INK)
            code='q2psx.exe --disc disc/game.cue\n  --headless --map SECURITY --frames 60\n  --size 1024x768 --aspect 4:3\n  --no-crt --no-bloom\n  --saves <task-profile>\n  --shot <raw.ppm> --shot-display <display.ppm>'
            self.text(code,M+18,y+15,CW-36,10,19,'Mono','#EBE5D7')
            self.label('RECIPE SHOWN WITH OUTPUT PATH PLACEHOLDERS',M,y+164,CW)
            return y+194
        if v=='editorial':
            self.rect(M,y,CW,170,'#F8FAFF','#D9D9D9')
            self.label('CAPTION SPECIMEN',M+18,y+16,CW-36)
            self.text('BASE0 entry passage',M+18,y+40,CW-36,24,27,'Display')
            self.text('Frame 510 / PAL / 4:3 display / CRT off / bloom off',M+18,y+82,CW-36,9,13,'Mono',WARM)
            self.text('Rusted rock and the narrow entry frame the route. The briefing has cleared, leaving the architecture visible.',M+18,y+111,CW-36,11,16)
            return y+192
        if v=='review':
            labels=[('01','IDENTITY','Name, edition, proportions'),('02','EVIDENCE','Source, state, limitations'),('03','READABILITY','Scale, contrast, sequence'),('04','DELIVERY','Files, links, provenance')]
            for i,(n,title,desc) in enumerate(labels):
                yy=y+i*43
                self.text(n,M,yy,35,22,24,'Display',WARM)
                self.text(title,M+49,yy,150,11,14,'Bold')
                self.text(desc,M+211,yy,298,10,14)
                self.line(M+49,yy+30,M+CW,yy+30,'#D1CCBE',.5)
            return y+193
        if v=='release':
            self.mini_doc(M,y,181,230,'Release note','image')
            self.image('A13',M+201,y,309,174,matte=VOID)
            self.rect(M+201,y+184,309,46,ORANGE)
            self.text('THE NEXT BUILD',M+213,y+191,285,17,22,'Advert',INK)
            self.text('Campaign panel / layout specimen',M+213,y+216,285,7,9,'Body',INK)
            return y+249
        if v=='technical':
            steps=['Disc data','Formats','GTE and packets','Rasterizer','Raw frame','Display fit']
            for i,label in enumerate(steps):
                x=M+(i%3)*175; yy=y+(i//3)*80
                self.rect(x,yy,160,50,ORANGE if i==2 else '#DDE4F2')
                self.text(label,x+10,yy+16,140,11,14,'Bold',INK)
                if i%3<2:
                    self.line(x+162,yy+25,x+173,yy+25,BLUE,1)
                    self.line(x+169,yy+21,x+173,yy+25,BLUE,1)
                    self.line(x+169,yy+29,x+173,yy+25,BLUE,1)
            self.line(M+CW-2,y+51,M+CW-2,y+66,BLUE,1)
            self.line(M+CW-2,y+66,M+80,y+66,BLUE,1)
            self.line(M+80,y+66,M+80,y+79,BLUE,1)
            self.line(M+76,y+75,M+80,y+79,BLUE,1)
            self.line(M+84,y+75,M+80,y+79,BLUE,1)
            self.label('SIMPLIFIED IMAGE PATH / GAMEPLAY AND AUDIO OMITTED',M,y+143,CW)
            self.text('Separate raw-frame evidence from optional display presentation.',M,y+166,CW,10.5,15)
            return y+205
        if v=='userguide':
            self.mini_doc(M,y,177,221,'Choose a disc')
            x=M+202
            for i,(a,b) in enumerate([('1','Keep the cue and its tracks together.'),('2','Choose the disc in the local selector.'),('3','Confirm the expected game starts.')]):
                yy=y+15+i*59
                self.text(a,x,yy,26,24,26,'Display',WARM)
                self.text(b,x+38,yy,266,10.5,15)
            self.label('ILLUSTRATIVE TASK SEQUENCE',x,y+205,307)
            return y+242
        if v=='sources':
            links=[('S01','PSX Data Center / US packaging','https://psxdatacenter.com/games/U/Q/SLUS-00757.html'),('S01','PSX Data Center / PAL packaging','https://psxdatacenter.com/games/SLES-01534.html'),('S03','HFS DB / instruction manual','https://db.hfsplay.fr/games/87888-quake-ii?lang=en'),('S02','LaunchBox / promotional archive','https://gamesdb.launchbox-app.com/games/images/11167-quake-ii')]
            for i,(ref,title,url) in enumerate(links):
                yy=y+i*33
                self.text(ref,M,yy,37,11,14,'Mono',WARM)
                self.text(title,M+49,yy,CW-49,11,15,'Bold',BLUE)
                self.c.linkURL(url,(M+49,H-yy-18,M+CW,H-yy),relative=0,thickness=0)
            self.text('Links identify archive hosts. They are not assertions of ownership or blanket permission to republish.',M,y+143,CW,9,13)
            return y+188
        if v=='closing':
            self.rect(M,y,CW,145,NAVY)
            self.text('Q2PSX-PC',M+20,y+20,CW-40,34,38,'Menu','#F4F6FF')
            self.text('COOL LIGHT. SOLID FORM.',M+22,y+76,CW-44,19,24,'Advert',ORANGE)
            self.text('DESIGN REFERENCE 1.0  /  28 OCTOBER 2026',M+22,y+112,CW-44,8,11,'Mono',STEEL)
            return y+168
        raise ValueError('Unknown visual '+v)

    def cover(self,p):
        self.rect(0,0,W,H,VOID)
        self.rect(0,0,W,96,NAVY)
        self.label('Q2PSX-PC  /  PROJECT VISUAL STANDARD',M,35,CW,CYAN)
        self.label('ARTWORK  /  DOCUMENTS  /  DESIGN ATLAS',M,61,CW,STEEL)
        self.image('cover-art',0,96,W,349,mode='fit')
        self.rect(0,445,W,35,VIOLET)
        self.rect(M,445,150,35,ORANGE)
        self.text('REFERENCE 01',M+12,454,130,10,13,'Bold',INK)
        self.text('Q2PSX-PC',M,514,CW,44,48,'Menu','#F4F6FF')
        self.text('Design Reference',M,581,CW,34,38,'Display','#F4F6FF')
        self.text('COOL LIGHT. SOLID FORM.',M,641,CW,20,25,'Advert',ORANGE)
        self.text('Artwork and supporting documents\nwith an indexed design atlas',M,693,CW,13,18,'Body',STEEL)
        self.label('VERSION 1.0  /  28 OCTOBER 2026',M,775,CW,CYAN)
        self.text('Current port scene: SECURITY, A13. Recorded crop; original colors.',M,806,CW,7.5,10,'Body',STEEL)

    def build(self):
        for idx,p in enumerate(self.guide['pages'],1):
            self.c.bookmarkPage(f'p{idx:02d}')
            self.c.addOutlineEntry(p['title'],f'p{idx:02d}',level=0,closed=False)
            if idx==1:self.cover(p);self.c.showPage();continue
            self.rect(0,0,W,H,PAPER)
            self.label('Q2PSX-PC  /  DESIGN REFERENCE',M,25,CW)
            self.text(p['section'].upper(),W-M-150,25,150,7.5,10,'Bold',WARM)
            self.rect(M,48,24,3,ORANGE)
            yy=self.text(p['title'],M,62,CW,28,31,'Display',BLUE)+14
            yy=self.text(p['lede'],M,yy,CW,11,15.5)+21
            if p['visual'].startswith('atlas'):
                bottom=self.atlas(p,yy)
            else:
                yy=self.visual(p,yy)
                bottom=self.blocks(p.get('blocks',[]),yy)
            self.bottoms.append({'page':idx,'title':p['title'],'content_bottom':round(bottom,1)})
            if bottom>766:
                raise ValueError(f'Page {idx} overflows: {bottom:.1f} > 766; {p["title"]}')
            self.text(p['sources'],M,782,CW,7.1,9.2,'Body','#596060')
            self.text('DESIGN BASELINE 1.0',M,823,220,7,9,'Mono',BLUE)
            self.text(f'{idx:02d}  /  {len(self.guide["pages"])}',W-M-52,820,52,9,12,'Bold')
            self.c.showPage()
        self.c.save()
        write_json(self.output/'layout-check.json',self.bottoms)


def text_edition(guide,entries,tokens,output):
    lines=['# Q2PSX Design Reference','',f'Version {guide["version"]} | 28 October 2026','',
           'Editable source: [guide.json](design/guide.json), [atlas.json](design/atlas.json), [tokens.json](design/tokens.json).',
           'Illustrated local editions: [Design guide](../.branding/design/Q2PSX-Design-Guide.pdf) and [Design atlas](../.branding/design/Q2PSX-Design-Atlas.pdf).',
           'Illustrated files include commercial reference artwork and remain outside Git. This text edition contains no embedded game assets.','']
    byid={e['id']:e for e in entries}
    for n,p in enumerate(guide['pages'],1):
        lines += [f'## {n:02d} {p["title"]}','',p['lede'],'']
        if p['visual']=='palette':
            lines += ['| Token | sRGB | Use |','| --- | --- | --- |']
            lines += [f'| {k} | `{v["value"]}` | {v["use"]} |' for k,v in tokens['colors'].items()]
            lines += ['']
        if p['visual']=='type':
            lines += ['| Role | Face | Size / leading |','| --- | --- | --- |']
            for k,v in tokens['typography'].items():
                if isinstance(v,dict):
                    size=f'{v["sizePt"]} / {v["linePt"]} pt' if 'sizePt' in v else 'Short pixel specimens'
                    lines += [f'| {k} | {v["family"]} {v.get("weight", "")} | {size} |']
            lines += ['']
        if p.get('table'):
            table=p['table'];lines += ['| '+' | '.join(table['heads'])+' |','|'+'|'.join([' --- ']*len(table['heads']))+'|']
            lines += ['| '+' | '.join(r)+' |' for r in table['rows']];lines+=['']
        for block in p.get('blocks',[]):lines += [f'### {block["h"]}','',block['t'],'']
        for key in p.get('atlas',[]):
            e=byid[key]
            lines += [f'### {key} {e["title"]}','',f'**{e["evidence"]}.** {e["use"]}', '',e['limit'],'',f'Source: `{e["source"]}`.']
            if e.get('url'):lines += [f'Archive: [{e.get("credit","Source")}]({e["url"]}).']
            if e.get('crop'):lines += [f'Normalized crop bounds: `{e["crop"]}`.']
            if e.get('cropPixels'):lines += [f'Crop bounds in source pixels: `{e["cropPixels"]}`.']
            lines+=['']
        lines += ['Source basis: '+p['sources'],'']
    lines += ['## Exact capture recipe','',
              'Run from `.install/`. This example reproduces A13; use a new disposable task directory for a new edition.','',
              '```powershell',
              '.\\q2psx.exe --disc disc/game.cue --map SECURITY --headless --frames 60 `',
              '  --size 1024x768 --aspect 4:3 --no-crt --no-bloom `',
              '  --saves ..\\.agents\\tmp\\design-document\\saves\\SECURITY `',
              '  --shot ..\\.agents\\tmp\\design-document\\captures\\SECURITY-raw.ppm `',
              '  --shot-display ..\\.agents\\tmp\\design-document\\captures\\SECURITY.ppm',
              '```','',
              'For noninteractive scripting on Windows, launch the GUI-subsystem client through a process API that waits, such as Python `subprocess.run`, and check the returned exit code. All captures are engine render-target output. No OS screen capture or mouse/keyboard automation is used.','',
              '## Rebuild the illustrated reference','',
              'Use Python with `reportlab`, `Pillow` and `pypdf` installed. On this task the Codex bundled Python was used. The generator reads the retained prepared images in `.branding/design/assets/` and does not need to launch the game for a text or layout revision.','',
              '```powershell','python scripts/build_design_reference.py','```','',
              'Pass `--prepare` only when the inputs listed in `atlas.json` exist; it regenerates the prepared images and register. Original scans are read only. `--fonts` selects the Windows reading faces; `--brand-fonts` selects the local reconstructed Q2PSX faces. Exact embedded font hashes are saved in font-register.json. Do not commit font binaries or illustrated outputs.','',
              '## Historical source links','',
              '- [PSX Data Center US archive](https://psxdatacenter.com/games/U/Q/SLUS-00757.html)',
              '- [PSX Data Center PAL archive](https://psxdatacenter.com/games/SLES-01534.html)',
              '- [HFS DB manual source](https://db.hfsplay.fr/games/87888-quake-ii?lang=en)',
              '- [LaunchBox PlayStation image archive](https://gamesdb.launchbox-app.com/games/images/11167-quake-ii)',
              '- [Vintage Computing and Gaming publication identification](https://www.vintagecomputing.com/index.php/archives/683)','']
    (ROOT/'docs/DESIGN.md').write_text('\n'.join(lines),encoding='utf-8')
    plates=sum(p['section']=='Design atlas' for p in guide['pages'])
    local=f'''# Q2PSX design reference

Version {guide['version']} | 28 October 2026

Blue and violet lead the artwork system; orange provides secondary text and
bounded background panels. Native lens flares have a dedicated reference page
and atlas plate. The PDFs embed the reconstructed Menu, Advert and HUD faces.

- [Design guide](Q2PSX-Design-Guide.pdf): {len(guide['pages'])} pages of artwork direction,
  typography, color, composition, capture rules and supporting-document specimens.
- [Design atlas](Q2PSX-Design-Atlas.pdf): {plates} plates, {len(entries)} indexed references.
  Guide page numbers are retained for cross-reference.
- [Preview](Q2PSX-Design-Preview.png): selected pages from the finished guide.
- [Editable sources and templates](../../docs/design/README.md).
- [Searchable text edition](../../docs/DESIGN.md).
- [Reconstructed type collection](../fonts/README.md).

`asset-register.json` identifies sources, crops, dimensions and checksums.
`capture-manifest.json` records scene and inspector commands. `font-register.json`
records the exact embedded faces. `contrast-check.json`, `layout-check.json`
and `verification.json` retain the final checks. Source imagery and useful
capture/build logs live in `evidence/`; prepared atlas images live in `assets/`.

The original archive remains unchanged. Illustrated references and local fonts
are excluded from Git; editable text, tokens and the generator are repository
sources. This design task changes no game behavior.

Regenerate from the repository root with:

```powershell
python scripts/build_design_reference.py
```

Use `--prepare` to rebuild image derivatives from retained evidence. The builder
requires ReportLab, Pillow, pypdf, the Windows reading faces and the three local
Q2PSX TTFs. Render and inspect the PDFs after changing the source.
'''
    (output/'README.md').write_text(local,encoding='utf-8')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepare',action='store_true')
    ap.add_argument('--output',type=Path,default=ROOT/'.branding/design')
    ap.add_argument('--fonts',type=Path,default=Path(os.environ.get('WINDIR','C:/Windows'))/'Fonts')
    ap.add_argument('--brand-fonts',type=Path,default=ROOT/'.branding/fonts')
    args=ap.parse_args()
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    # Font files are used locally for PDF subsetting, never copied by this tool.
    fonts=[(name,args.fonts/file) for name,file in [('Body','arial.ttf'),('Bold','arialbd.ttf'),('Italic','ariali.ttf'),('Display','ARIALNB.TTF'),('Mono','consola.ttf')]]
    fonts += [(name,args.brand_fonts/file) for name,file in [('Menu','Q2PSX-Menu-Regular.ttf'),('Advert','Q2PSX-Advert-Regular.ttf'),('HUD','Q2PSX-HUD-Pixel-Regular.ttf')]]
    for name,path in fonts:
        if not path.is_file():raise SystemExit(f'Missing required font: {path}')
        pdfmetrics.registerFont(TTFont(name,str(path)))
    guide=read_json(ROOT/'docs/design/guide.json')
    tokens=read_json(ROOT/'docs/design/tokens.json')
    global PAPER, INK, WARM, ORANGE, VOID, STEEL, BLUE, NAVY, VIOLET, CYAN
    PAPER,INK,WARM,ORANGE,VOID,STEEL,BLUE,NAVY,VIOLET,CYAN=[tokens['colors'][k]['value'] for k in
        ('paper','ink','printOrange','orange','void','steel','printBlue','navy','violet','cyan')]
    entries=read_json(ROOT/'docs/design/atlas.json')['entries']
    if args.prepare:prepare_assets(output,entries)
    if not (output/'asset-register.json').is_file():
        raise SystemExit('Prepared assets are missing. Supply the listed local inputs, then run with --prepare.')
    for entry in entries:
        if not (output/'assets'/f'{entry["id"]}.png').is_file():raise SystemExit(f'Missing prepared asset {entry["id"]}')
    book=Book(output,guide,tokens,entries);book.build()
    reader=PdfReader(output/'Q2PSX-Design-Guide.pdf')
    writer=PdfWriter()
    atlas_indices=[i for i,p in enumerate(guide['pages']) if p['section']=='Design atlas']
    for i in atlas_indices:
        writer.add_page(reader.pages[i])
        writer.add_outline_item(guide['pages'][i]['title'],len(writer.pages)-1)
    writer.add_metadata({'/Title':'Q2PSX Design Atlas','/Author':'Q2PSX-PC project reference','/Subject':f'{len(atlas_indices)} reference plates; guide page numbers retained'})
    with (output/'Q2PSX-Design-Atlas.pdf').open('wb') as f:writer.write(f)
    text_edition(guide,entries,tokens,output)
    ratios=[]
    for a,b in CONTRAST_PAIRS+[('ink','paper'),('printBlue','paper'),('cyan','navy'),('white','violet')]:
        ratios.append({'foreground':a,'background':b,'contrast':round(contrast(tokens['colors'][a]['value'],tokens['colors'][b]['value']),3)})
    write_json(output/'contrast-check.json',ratios)
    write_json(output/'font-register.json',[{'role':n,'source':str(p),'sha256':digest(p)} for n,p in fonts])
    print(json.dumps({'guide_pages':len(reader.pages),'atlas_pages':len(writer.pages),'atlas_entries':len(entries),'output':str(output)},indent=2))


if __name__=='__main__':main()
