"""The 40 x 40 mm device label for the bench tool (tools/bench_provision.py).

The label is white print on a black label (NIIMBOT M2, white ribbon), 300 dpi:
472 x 472 dots. The printer transfers ribbon wherever the image is dark, so
the file to print (render_ink) is black on white; render_preview inverts it
to show how the label will look.

Top to bottom: "RF Power Monitor", the board revision, a rule, the hostname,
MAC / User / PW, then S/N, firmware and date bottom left with a QR code
bottom right. The QR code holds hostname, MAC, serial number and firmware,
never the password. Fonts: DejaVu (tools/fonts, see its LICENSE).
"""

import os
from dataclasses import dataclass

import segno
from PIL import Image, ImageDraw, ImageFont, ImageOps

SIZE_DOTS = 472          # 40 mm at 300 dpi
MARGIN_DOTS = 24         # 2 mm
QR_MODULE_DOTS = 4       # 0.34 mm per module
QR_BORDER_MODULES = 2    # quiet zone without ink
QR_ERROR = "l"           # version 4 for the usual text: about 12.5 mm with 4-dot modules
DPI = 300
WEB_USER = "admin"

FONT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts")
SANS = os.path.join(FONT_DIR, "DejaVuSans.ttf")
SANS_BOLD = os.path.join(FONT_DIR, "DejaVuSans-Bold.ttf")
MONO = os.path.join(FONT_DIR, "DejaVuSansMono.ttf")

# Start sizes in dots; each line shrinks until it fits its width
TITLE_DOTS = 46
REVISION_DOTS = 22
HOSTNAME_DOTS = 38
DETAIL_LABEL_DOTS = 24
DETAIL_VALUE_DOTS = 30
SMALL_DOTS = 21
DETAIL_LABEL_WIDTH = 78  # MAC / User / PW column
RULE_DOTS = 3
GAP_DOTS = 6


@dataclass
class LabelData:
    """What goes on one label. mac as 2C:CF:67:A1:B2:C3, date as YYYY-MM-DD."""
    hostname: str
    mac: str
    serial: str
    firmware: str
    password: str
    revision: str
    date: str


def qr_text(d: LabelData) -> str:
    """The QR code's text: RFPM;H=<hostname>;M=<12 hex, upper case>;S=<serial>;FW=<firmware>."""
    mac = d.mac.replace(":", "").replace("-", "").upper()
    return "RFPM;H=%s;M=%s;S=%s;FW=%s" % (d.hostname, mac, d.serial, d.firmware)


def _fit(draw, text, path, size, width):
    """The font from path at size, made smaller in 1-dot steps until text is at most width wide."""
    while size > 6:
        font = ImageFont.truetype(path, size)
        if draw.textlength(text, font=font) <= width:
            return font
        size -= 1
    return ImageFont.truetype(path, size)


def _text(draw, x, y, text, font):
    """Draw text with its top at y; return the y below it."""
    left, top, right, bottom = draw.textbbox((x, y), text, font=font, anchor="la")
    draw.text((x, y - (top - y)), text, font=font, fill=0, anchor="la")
    return y + (bottom - top)


def _qr(text):
    """The QR code as an image: ink (0) for dark modules, quiet zone included."""
    qr = segno.make(text, error=QR_ERROR, boost_error=False)
    n = len(qr.matrix)
    size = (n + 2 * QR_BORDER_MODULES) * QR_MODULE_DOTS
    img = Image.new("1", (size, size), 1)
    draw = ImageDraw.Draw(img)
    for r, row in enumerate(qr.matrix):
        for c, dark in enumerate(row):
            if dark:
                x = (QR_BORDER_MODULES + c) * QR_MODULE_DOTS
                y = (QR_BORDER_MODULES + r) * QR_MODULE_DOTS
                draw.rectangle((x, y, x + QR_MODULE_DOTS - 1, y + QR_MODULE_DOTS - 1), fill=0)
    return img


def render_ink(d: LabelData) -> Image.Image:
    """The image to print: 472 x 472, mode "1", ink (ribbon) black on white."""
    img = Image.new("1", (SIZE_DOTS, SIZE_DOTS), 1)
    draw = ImageDraw.Draw(img)
    left, right = MARGIN_DOTS, SIZE_DOTS - MARGIN_DOTS
    width = right - left

    y = MARGIN_DOTS
    y = _text(draw, left, y, "RF Power Monitor", _fit(draw, "RF Power Monitor", SANS_BOLD, TITLE_DOTS, width))
    y = _text(draw, left, y + GAP_DOTS, d.revision, _fit(draw, d.revision, SANS, REVISION_DOTS, width))
    y += GAP_DOTS + 2
    draw.rectangle((left, y, right - 1, y + RULE_DOTS - 1), fill=0)
    y += RULE_DOTS + GAP_DOTS + 2
    y = _text(draw, left, y, d.hostname, _fit(draw, d.hostname, SANS_BOLD, HOSTNAME_DOTS, width))
    y += GAP_DOTS + 2

    for label, value in (("MAC", d.mac.upper()), ("User", WEB_USER), ("PW", d.password)):
        value_font = _fit(draw, value, MONO, DETAIL_VALUE_DOTS, width - DETAIL_LABEL_WIDTH)
        label_font = ImageFont.truetype(SANS_BOLD, DETAIL_LABEL_DOTS)
        # Both parts on one baseline
        base = y + value_font.getmetrics()[0]
        draw.text((left, base), label, font=label_font, fill=0, anchor="ls")
        draw.text((left + DETAIL_LABEL_WIDTH, base), value, font=value_font, fill=0, anchor="ls")
        y = base + value_font.getmetrics()[1] + 2

    qr = _qr(qr_text(d))
    qr_x = right - qr.size[0]
    qr_y = SIZE_DOTS - MARGIN_DOTS - qr.size[1]
    img.paste(qr, (qr_x, qr_y))

    # Bottom left, bottom-aligned with the QR code's last module row
    small_width = qr_x - left - GAP_DOTS
    lines = ("S/N " + d.serial, "FW " + d.firmware, d.date)
    fonts = [_fit(draw, t, SANS, SMALL_DOTS, small_width) for t in lines]
    bottom = SIZE_DOTS - MARGIN_DOTS - QR_BORDER_MODULES * QR_MODULE_DOTS
    for text, font in reversed(list(zip(lines, fonts))):
        ascent, descent = font.getmetrics()
        draw.text((left, bottom - descent), text, font=font, fill=0, anchor="ls")
        bottom -= ascent + descent + 2
    return img


def render_preview(d: LabelData) -> Image.Image:
    """How the label looks: white print on black, mode "L"."""
    return ImageOps.invert(render_ink(d).convert("L"))


def save_label(d: LabelData, folder: str) -> tuple:
    """Write <hostname>.png (to print, 300 dpi) and <hostname>-preview.png into folder; return both paths."""
    os.makedirs(folder, exist_ok=True)
    ink_path = os.path.join(folder, d.hostname + ".png")
    preview_path = os.path.join(folder, d.hostname + "-preview.png")
    render_ink(d).save(ink_path, dpi=(DPI, DPI))
    render_preview(d).save(preview_path, dpi=(DPI, DPI))
    return ink_path, preview_path
