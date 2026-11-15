import os
import sys

from PIL import Image, ImageFont

# 拉丁字模用 Roboto（google/fonts ofl/roboto，Apache 2.0），汉字用 Noto Sans SC
# （google/fonts ofl/notosanssc，OFL）。两个都只是生成时的输入，不进仓库。
LATIN_TTF = os.environ.get('FONT_TTF', 'Roboto[wdth,wght].ttf')
CJK_TTF = os.environ.get('FONT_CJK_TTF', 'NotoSansSC[wght].ttf')

# 汉字只带屏幕上真用到的那些，省 flash：从入口、屏幕 UI 和天气文字的字符串字面量里扫。
# （portal_web.c 里是网页文案，走浏览器自己的字体，不参与。）
import re


def screen_chars():
    used = set()
    for path in ('main/main.c', 'main/ui.c', 'main/weather.c'):
        for lit in re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', open(path, encoding='utf-8').read()):
            used.update(ch for ch in lit if ord(ch) > 0x7F)
    return ''.join(sorted(used))


CJK = screen_chars()

# name, px, weight, 是否带汉字
# 12px 的汉字掉笔画看不懂，所以汉字只带在 16px 的 font_mid 里；需要汉字就用 font_mid。
# 14px 只给五天温度行：`12°-25°` 在 16px 是 53px 放不进 52px 列，14px 是 47px。
SPECS = [('font_huge', 46, 'Bold', False),
         ('font_mid', 16, 'Medium', True),
         ('font_temp', 14, 'Medium', False),
         ('font_small', 12, 'Regular', False)]


def load(path, size, weight):
    f = ImageFont.truetype(path, size)
    try:
        f.set_variation_by_name(weight)
    except Exception as e:
        print('variation failed for', weight, e, file=sys.stderr)
    return f


out = ['// Generated from Roboto + Noto Sans SC with PIL; 1bpp glyphs, FreeType mono rasterizer.',
       '#include "fonts_aa.h"', '', '']
decls, tables = [], []

for name, size, weight, cjk in SPECS:
    latin = load(LATIN_TTF, size, weight)
    han = load(CJK_TTF, size, weight) if cjk else None
    cps = list(range(0x20, 0x80))
    if han:
        cps = sorted(set(cps + [ord(ch) for ch in CJK]))
    # U+00B0 度符号：温度数值必须带它，否则屏上只有裸数字。
    cps = sorted(set(cps + [0xB0]))
    data, glyphs = [], []
    for cp in cps:
        # 汉字走 Noto；拉丁、数字和 °（U+00B0）用 Roboto，与同行的数字度量一致。
        f = han if han and cp > 0xFF else latin
        ch = chr(cp)
        adv = int(round(f.getlength(ch)))
        # 1-bit 屏没有灰阶：误差扩散会把 12~16px 的 1px 笔画打散成噪点，看起来"发虚"。
        # FreeType 的 mono 目标（mode='1'）把笔画网格对齐到整像素，边缘干净、粗细一致；
        # 大字号（46px）两种方式几乎一样，统一用 mono 也让字模体积和代码只有一条路径。
        mask, (x0, y0) = f.getmask2(ch, anchor='ls', mode='1')
        img = Image.Image()._new(mask)
        w, h = img.size
        off = len(data)
        if w and h:
            px = img.load()
            stride = (w + 7) // 8
            for row in range(h):
                line = bytearray(stride)
                for col in range(w):
                    if px[col, row]:
                        line[col >> 3] |= 0x80 >> (col & 7)
                data.extend(line)
        glyphs.append((off, w, h, adv, x0, y0))
    decls.append(f'static const uint8_t {name}_data[] = {{{",".join("0x%02x" % b for b in data)}}};')
    rows = '\n'.join('    {%d,%d,%d,%d,%d,%d},' % g for g in glyphs)
    decls.append(f'static const aaglyph {name}_glyphs[] = {{\n{rows}\n}};')
    decls.append(f'static const uint16_t {name}_cps[] = {{{",".join(str(c) for c in cps)}}};')
    tables.append(f'const aafont {name} = {{{name}_data, {name}_glyphs, {name}_cps, {len(cps)}, {size + 4}}};')
    print(f'{name}: {len(cps)} glyphs, {len(data)} bytes', file=sys.stderr)

open('main/fonts_aa.c', 'w').write('\n'.join(out + decls + [''] + tables + ['']))
