"""Build the checked Chinese user manual from its Markdown source.

Uses ReportLab and Windows Microsoft YaHei. Run with the bundled Codex Python.
The Markdown remains the editable source of truth; pagebreak markers map the
13 intentionally composed pages in the PDF.
"""
from html import escape
import json
from pathlib import Path
import re
import sys

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    Flowable, Image, KeepTogether, PageBreak, Paragraph, Preformatted,
    SimpleDocTemplate, Spacer, Table, TableStyle,
)
from pypdf import PdfReader

HERE = Path(__file__).resolve().parent
APP = HERE.parent
SOURCE = HERE / "USAGE_MANUAL.md"
OUTPUT = APP / "output/pdf/PID项目使用手册_V1.2_2026-10-19.pdf"
INK = colors.HexColor("#152A3B")
TEAL = colors.HexColor("#087F78")
MUTED = colors.HexColor("#526676")
LINE = colors.HexColor("#D9E3E9")
LIGHT = colors.HexColor("#F1F6F8")
WIDTH = A4[0] - 88

pdfmetrics.registerFont(TTFont("ManualCN", "C:/Windows/Fonts/msyh.ttc", subfontIndex=0))
pdfmetrics.registerFont(TTFont("ManualBold", "C:/Windows/Fonts/msyhbd.ttc", subfontIndex=0))
pdfmetrics.registerFontFamily("ManualCN", normal="ManualCN", bold="ManualBold", italic="ManualCN", boldItalic="ManualBold")

BODY = ParagraphStyle("body", fontName="ManualCN", fontSize=10, leading=17,
                      textColor=INK, spaceAfter=8, wordWrap="CJK")
TITLE = ParagraphStyle("title", parent=BODY, fontName="ManualBold", fontSize=27,
                       leading=38, textColor=INK, spaceAfter=15)
CHAPTER = ParagraphStyle("chapter", parent=BODY, fontName="ManualBold", fontSize=19,
                         leading=28, textColor=TEAL, spaceAfter=14, keepWithNext=True)
SUB = ParagraphStyle("sub", parent=BODY, fontName="ManualBold", fontSize=11.4,
                     leading=19, spaceBefore=7, spaceAfter=7, keepWithNext=True)
SMALL = ParagraphStyle("small", parent=BODY, fontSize=8.5, leading=13.5, textColor=MUTED, spaceAfter=10)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=9, leading=14, spaceAfter=0)
HEAD = ParagraphStyle("head", parent=CELL, fontName="ManualBold", textColor=colors.white)
CODE = ParagraphStyle("code", fontName="ManualCN", fontSize=8.8, leading=13.2,
                      textColor=INK, backColor=LIGHT, borderPadding=9, spaceBefore=4, spaceAfter=11)


def inline(value):
    # Preserve literal API and CLI punctuation before adding the few supported
    # Markdown spans. There is deliberately no arbitrary HTML execution.
    value = escape(value)
    value = re.sub(r"`([^`]+)`", r'<font color="#087F78">\1</font>', value)
    value = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", value)
    return value


class RouteDiagram(Flowable):
    def __init__(self):
        super().__init__()
        self.width, self.height = WIDTH, 88

    def draw(self):
        canvas = self.canv
        boxes = [("电脑", "上位机 / AI"), ("主机 S3", "旋钮 / OLED"),
                 ("从机 S3", "接收 / 编程"), ("目标 MCU", "F103 已验收")]
        box_width, gap, top = 103, (WIDTH-412)/3, 72
        for index, (name, detail) in enumerate(boxes):
            x = index*(box_width+gap)
            canvas.setFillColor(LIGHT)
            canvas.setStrokeColor(LINE)
            canvas.roundRect(x, top-48, box_width, 48, 6, fill=1, stroke=1)
            canvas.setFillColor(INK)
            canvas.setFont("ManualBold", 12)
            canvas.drawCentredString(x+box_width/2, top-19, name)
            canvas.setFillColor(MUTED)
            canvas.setFont("ManualCN", 8.4)
            canvas.drawCentredString(x+box_width/2, top-36, detail)
            if index < 3:
                start, end, y = x+box_width+3, x+box_width+gap-3, top-25
                canvas.setStrokeColor(TEAL)
                canvas.line(start, y, end, y)
                canvas.line(end-4, y+3, end, y)
                canvas.line(end-4, y-3, end, y)
        canvas.setFont("ManualCN", 8.5)
        canvas.setFillColor(TEAL)
        canvas.drawString(0, 7, "USB 接主机   |   主从之间 ESP-NOW   |   从机到目标 UART / SWD（F407）")


def table_widths(rows):
    count = len(rows[0])
    header = " ".join(rows[0])
    if count == 4 and "H2" in header:
        return [52, 58, 177, WIDTH-287]
    if count == 4:
        return [79, 67, 126, WIDTH-272]
    if count == 3 and "页码" in header:
        return [39, 173, WIDTH-212]
    if count == 3 and "工程目录" in header:
        return [48, 258, WIDTH-306]
    if count == 3:
        return [77, 183, WIDTH-260]
    if rows[0] == ["位置", "内容"]:
        return [270, WIDTH-270]
    return [160, WIDTH-160]


def make_table(rows):
    data = [[Paragraph(inline(cell), HEAD if row_index == 0 else CELL) for cell in row]
            for row_index, row in enumerate(rows)]
    value = Table(data, colWidths=table_widths(rows), repeatRows=1, hAlign="LEFT")
    value.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), TEAL),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, LIGHT]),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("RIGHTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
        ("LINEBELOW", (0, -1), (-1, -1), .5, LINE),
    ]))
    return [value, Spacer(1, 11)]


def code_lines(lines):
    for line in lines:
        if pdfmetrics.stringWidth(line, CODE.fontName, CODE.fontSize) > WIDTH-24:
            raise ValueError("Code line exceeds the printable width; use a valid PowerShell continuation: "+line)
    return "\n".join(lines)


def content(source):
    lines = source.splitlines()
    story = []
    index = 0
    cover = True
    while index < len(lines):
        line = lines[index].strip()
        if not line:
            index += 1
            continue
        if line == "<!-- pagebreak -->":
            story.append(PageBreak())
            cover = False
            index += 1
        elif line.startswith("```"):
            index += 1
            snippet = []
            while index < len(lines) and not lines[index].strip().startswith("```"):
                snippet.append(lines[index])
                index += 1
            block = Table([[Preformatted(code_lines(snippet), CODE)]], colWidths=[WIDTH])
            block.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, -1), LIGHT),
                ("LEFTPADDING", (0, 0), (-1, -1), 10),
                ("RIGHTPADDING", (0, 0), (-1, -1), 10),
                ("TOPPADDING", (0, 0), (-1, -1), 8),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 8),
            ]))
            story.extend([block, Spacer(1, 10)])
            index += 1
        elif line.startswith("|"):
            rows = []
            while index < len(lines) and lines[index].strip().startswith("|"):
                cells = [cell.strip() for cell in lines[index].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
                    rows.append(cells)
                index += 1
            story.extend(make_table(rows))
        elif line.startswith("!["):
            match = re.match(r"!\[(.*?)\]\((.*?)\)", line)
            path = HERE / match[2]
            image = Image(str(path))
            scale = min(460/image.imageWidth, 325/image.imageHeight)
            image.drawWidth = image.imageWidth*scale
            image.drawHeight = image.imageHeight*scale
            image.hAlign = "CENTER"
            story.extend([image, Spacer(1, 9)])
            index += 1
        elif line.startswith("# "):
            story.extend([Spacer(1, 8), Paragraph(inline(line[2:]), TITLE)])
            index += 1
        elif line.startswith("### "):
            story.append(Paragraph(inline(line[4:]), SUB))
            index += 1
        elif line.startswith("## "):
            story.append(Paragraph(inline(line[3:]), SUB if cover else CHAPTER))
            index += 1
        else:
            group = [line]
            index += 1
            if not re.match(r"\d+\. ", line):
                while index < len(lines) and lines[index].strip() and not lines[index].lstrip().startswith(("#", "|", "```", "<!--", "![")):
                    group.append(lines[index].strip())
                    index += 1
            paragraph = " ".join(group)
            style = SMALL if paragraph.startswith(("使用手册 V", "图示为当前界面")) else BODY
            story.append(Paragraph(inline(paragraph), style))
            if paragraph.startswith("**固定路径："):
                story.append(RouteDiagram())
    return story


def chrome(canvas, document):
    canvas.saveState()
    canvas.setStrokeColor(LINE)
    canvas.setLineWidth(.6)
    canvas.line(44, A4[1]-31, A4[0]-44, A4[1]-31)
    canvas.setFont("ManualCN", 8)
    canvas.setFillColor(MUTED)
    canvas.drawString(44, A4[1]-23, "PID WORKBENCH  /  无线主从机使用手册")
    canvas.drawRightString(A4[0]-44, A4[1]-23, "V1.2 · 2026-10-19")
    canvas.line(44, 35, A4[0]-44, 35)
    canvas.drawString(44, 22, "F103 / F407VE 已验收  |  F407 接线见 13 页  |  多从机规划见 12 页")
    canvas.drawRightString(A4[0]-44, 22, f"{document.page:02d}")
    canvas.restoreState()


def main():
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text(encoding="utf-8")
    document = SimpleDocTemplate(str(OUTPUT), pagesize=A4, rightMargin=44, leftMargin=44,
                                 topMargin=46, bottomMargin=46,
                                 title="无线 PID 调试与烧录工作台 - 使用手册 V1.2",
                                 author="PID 项目", subject="硬件接线、调参、无线 UART 烧录、回传与 AI 接入")
    document.build(content(source), onFirstPage=chrome, onLaterPages=chrome)
    reader = PdfReader(str(OUTPUT))
    expected = source.count("<!-- pagebreak -->")+1
    page_text = [page.extract_text() or "" for page in reader.pages]
    expected_headings = ["无线 PID 调试与烧录工作台"] + re.findall(r"^## (\d\d .+)$", source, flags=re.M)
    report = {"pdf": str(OUTPUT), "page_count": len(reader.pages), "expected_pages": expected,
              "headings": expected_headings,
              "page_characters": [len(text) for text in page_text],
              "page_heading_matches": [heading in text for heading, text in zip(expected_headings, page_text)]}
    report_path = APP / "validation/manual_20261019.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    if len(reader.pages) != expected or not all(report["page_heading_matches"]):
        raise SystemExit("Manual page overflow or heading placement mismatch; adjust layout before delivery")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
