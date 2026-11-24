#!/usr/bin/env python3
"""Rasterize the original brand and control symbols at the target UI density."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("font")
parser.add_argument("output", type=Path)
parser.add_argument("--full-hd", action="store_true")
args = parser.parse_args()

SAMPLES = 8
WHITE = (249, 246, 255, 255)
LAVENDER = (203, 175, 255, 255)


def dimensions(width, height):
    return (width * 3 // 2, height * 3 // 2) if args.full_hd else (width, height)


def mark(size):
    """A playback mark in a folded broadcast bubble."""
    unit = size / 64
    mask = Image.new("L", (size, size))
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((1 * unit, 1 * unit, 63 * unit, 55 * unit), radius=17 * unit, fill=255)
    draw.polygon([(12 * unit, 48 * unit), (32 * unit, 48 * unit), (17 * unit, 62 * unit),
                  (13 * unit, 63 * unit)], fill=255)

    image = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        at = y / (size - 1)
        color = tuple(round(a + (b - a) * at) for a, b in zip((197, 151, 255), (108, 62, 214)))
        draw.line((0, y, size, y), fill=color + (255,))
    image.putalpha(mask)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((15 * unit, 15 * unit, 49 * unit, 41 * unit), radius=8 * unit,
                           fill=(35, 20, 67, 255))
    draw.polygon([(28 * unit, 21 * unit), (28 * unit, 35 * unit), (40 * unit, 28 * unit)], fill=WHITE)
    return image


def wordmark(width, height):
    image = Image.new("RGBA", (width * SAMPLES, height * SAMPLES))
    unit = height * SAMPLES / 52
    image.alpha_composite(mark(round(42 * unit)), (0, round(5 * unit)))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(args.font, round(27 * unit))
    left, midline = round(50 * unit), round(26 * unit)
    draw.text((left, midline), "IZtwiNS", font=font, fill=WHITE, anchor="lm")
    tail = draw.textlength("IZtwiNS", font=font)
    draw.text((left + tail, midline), "2", font=font, fill=LAVENDER, anchor="lm")
    return image.resize((width, height), Image.Resampling.LANCZOS)


def tile():
    scale = 4
    size = 256 * scale
    image = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        at = y / (size - 1)
        color = tuple(round(a + (b - a) * at) for a, b in zip((34, 25, 57), (14, 12, 27)))
        draw.line((0, y, size, y), fill=color + (255,))
    image.alpha_composite(mark(138 * scale), (59 * scale, 33 * scale))
    font = ImageFont.truetype(args.font, 32 * scale)
    draw.text((128 * scale, 207 * scale), "IZtwiNS2", font=font, fill=WHITE, anchor="mm")
    return image.resize((256, 256), Image.Resampling.LANCZOS).convert("RGB")


def symbol(name, size):
    mask = Image.new("L", (size * SAMPLES, size * SAMPLES))
    draw = ImageDraw.Draw(mask)
    unit = mask.width / 26

    def pt(points):
        return [(round(x * unit), round(y * unit)) for x, y in points]

    def stroke(points, width=2.0, value=255):
        coords = pt(points)
        pen = round(width * unit)
        draw.line(coords, fill=value, width=pen, joint="curve")
        radius = pen / 2
        for x, y in coords:
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=value)

    def rect(box, radius, value=255):
        draw.rounded_rectangle(tuple(round(v * unit) for v in box), radius=round(radius * unit), fill=value)

    def dot(x, y, radius, value=255):
        draw.ellipse(tuple(round(v * unit) for v in (x-radius, y-radius, x+radius, y+radius)), fill=value)

    def arc(box, start, end, width=2):
        draw.arc(tuple(round(v * unit) for v in box), start, end, fill=255, width=round(width * unit))

    def polygon(points):
        draw.polygon(pt(points), fill=255)

    if name == "play":
        polygon([(8, 5.5), (8, 20.5), (20, 13)])
    elif name == "pause":
        rect((6.5, 5.5, 11, 20.5), 1.4)
        rect((15, 5.5, 19.5, 20.5), 1.4)
    elif name in ("back", "forward"):
        arc((5, 5, 21, 21), 285, 580)
        stroke([(4.4, 5.8), (4.4, 11.1), (9.7, 11.1)], 1.8)
        if name == "forward":
            mask = mask.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    elif name in ("volume", "mute"):
        polygon([(4, 10), (8, 10), (13, 6), (13, 20), (8, 16), (4, 16)])
        if name == "mute":
            stroke([(16.5, 10.5), (22, 16)], 1.8)
            stroke([(22, 10.5), (16.5, 16)], 1.8)
        else:
            arc((9, 8.5, 19.5, 17.5), 306, 414, 1.7)
            arc((8, 5.5, 23, 20.5), 305, 415, 1.7)
    elif name in ("chat", "message"):
        stroke([(7, 19), (5.5, 22), (11, 19)], 1.8)
        rect((4, 5, 22, 19), 4)
        rect((6, 7, 20, 17), 2, 0)
        if name == "chat":
            for x in (9, 13, 17):
                dot(x, 12, 1)
        else:
            stroke([(9, 11), (17, 11)], 1.4)
            stroke([(9, 14), (14, 14)], 1.4)
    elif name == "settings":
        for y, x in ((7, 10), (13, 17), (19, 12)):
            stroke([(5, y), (21, y)], 1.75)
            dot(x, y, 2.7)
    elif name == "check":
        stroke([(5.5, 13.5), (10.7, 18.7), (20.5, 8.7)], 2.5)
    elif name == "right":
        stroke([(10, 7), (16, 13), (10, 19)], 2.1)
    elif name == "left":
        stroke([(16, 7), (10, 13), (16, 19)], 2.1)
    elif name == "home":
        stroke([(4, 12), (13, 4.5), (22, 12)], 1.8)
        stroke([(6, 11), (6, 21), (10.3, 21), (10.3, 15.5), (15.7, 15.5),
                (15.7, 21), (20, 21), (20, 11)], 1.7)
    elif name == "trends":
        stroke([(4.5, 19), (10, 13.5), (14, 16), (21, 8)], 2)
        stroke([(16, 8), (21, 8), (21, 13)], 1.8)
    elif name == "following":
        dot(9.5, 9, 3)
        arc((4, 13.5, 15, 22), 180, 360)
        dot(18, 10.5, 2.4)
        arc((15.5, 14.5, 22.5, 22), 200, 360, 1.7)
    elif name == "continue":
        arc((4.5, 4.5, 21.5, 21.5), 15, 342)
        polygon([(11, 8.5), (17.5, 13), (11, 17.5)])
    elif name == "favorites":
        polygon([(13, 4), (15.8, 9.7), (22, 10.6), (17.5, 15), (18.6, 21.2),
                 (13, 18.3), (7.4, 21.2), (8.5, 15), (4, 10.6), (10.2, 9.7)])
    elif name == "recent":
        arc((4.5, 4.5, 21.5, 21.5), 0, 360)
        stroke([(13, 8), (13, 13.3), (16.5, 15)], 1.7)
    elif name == "search":
        arc((5, 5, 18.5, 18.5), 0, 360)
        stroke([(16.7, 16.7), (21.5, 21.5)], 1.9)
    elif name == "link":
        arc((3.5, 9.5, 15.5, 21), 58, 286, 1.9)
        arc((10.5, 5, 22.5, 16.5), 237, 468, 1.9)
        stroke([(10, 16), (16, 10)], 1.7)
    elif name == "account":
        dot(13, 9, 3.6)
        arc((5, 14.5, 21, 24), 180, 360, 2.0)
    else:
        raise ValueError(name)
    return mask.resize((size, size), Image.Resampling.LANCZOS)


def sidebar_icon(name, size, color):
    side = size * SAMPLES
    mask = Image.new("L", (side, side))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, side - 1, side - 1), radius=side * 0.30, fill=255)
    image = Image.new("RGBA", (size, size), color)
    image.putalpha(mask.resize((size, size), Image.Resampling.LANCZOS))
    width = dimensions(25, 25)[0]
    mark_mask = symbol(name, width)
    image.paste(WHITE, ((size - width) // 2, (size - width) // 2,
                        (size - width) // 2 + width, (size - width) // 2 + width), mark_mask)
    return image


def write_bytes(header, data, indent):
    for start in range(0, len(data), 24):
        header.write(indent + ", ".join(map(str, data[start:start + 24])) + ",\n")


logo_width, logo_height = dimensions(180, 52)
icon_size = dimensions(34, 34)[0]
glyph_size = dimensions(26, 26)[0]
menu_icons = (
    ("home", (90, 62, 168, 255)), ("trends", (121, 52, 125, 255)),
    ("following", (33, 93, 125, 255)), ("continue", (121, 68, 37, 255)),
    ("favorites", (117, 57, 93, 255)), ("recent", (46, 76, 118, 255)),
    ("search", (79, 57, 135, 255)), ("link", (41, 85, 118, 255)), ("account", (93, 53, 113, 255)),
)
glyphs = ("play", "pause", "back", "forward", "volume", "mute", "chat", "message", "settings",
          "check", "right", "left", "favorites", "trends")

tile_image = tile()
tile_image.save(args.output / "icon.png")
tile_image.save(args.output / "icon.jpg", quality=96, subsampling=0)
with (args.output / "brand.h").open("w") as header:
    header.write(f"enum {{ BRAND_WIDTH = {logo_width}, BRAND_HEIGHT = {logo_height},\n"
                 f"       CONTROL_ICON_SIZE = {icon_size}, GLYPH_SIZE = {glyph_size} }};\n")
    header.write("typedef enum { GLYPH_NONE = -1, " +
                 ", ".join(f"GLYPH_{glyph.upper()}" for glyph in glyphs) + " } Glyph;\n")
    header.write("static const unsigned char brand_rgba[] = {\n")
    write_bytes(header, wordmark(logo_width, logo_height).tobytes(), "    ")
    header.write("};\nstatic const unsigned char control_rgba[][CONTROL_ICON_SIZE * CONTROL_ICON_SIZE * 4] = {\n")
    for name, color in menu_icons:
        header.write("    {\n")
        write_bytes(header, sidebar_icon(name, icon_size, color).tobytes(), "        ")
        header.write("    },\n")
    header.write("};\nstatic const unsigned char glyph_alpha[][GLYPH_SIZE * GLYPH_SIZE] = {\n")
    for name in glyphs:
        header.write("    {\n")
        write_bytes(header, symbol(name, glyph_size).tobytes(), "        ")
        header.write("    },\n")
    header.write("};\n")
