from pathlib import Path
import math
import struct


ROOT = Path(__file__).resolve().parent
WIDTH = 8
HEIGHT = 4


def write_tga() -> None:
    red = (255, 0, 0, 255)
    blue = (0, 0, 255, 255)
    green = (0, 255, 0, 255)
    yellow = (255, 255, 0, 255)
    rows = [
        [(255, 0, 255, 255)] * WIDTH,
        [red, blue, blue, green, green, yellow, yellow, red],
        [red, blue, blue, green, green, yellow, yellow, red],
        [(0, 255, 255, 255)] * WIDTH,
    ]
    header = struct.pack(
        "<BBBHHBHHHHBB",
        0, 0, 2, 0, 0, 0, 0, 0, WIDTH, HEIGHT, 32, 0x28
    )
    pixels = bytes(
        channel
        for row in rows
        for red_value, green_value, blue_value, alpha_value in row
        for channel in (blue_value, green_value, red_value, alpha_value)
    )
    (ROOT / "AnalyticalLDR.tga").write_bytes(header + pixels)


def to_rgbe(rgb: tuple[float, float, float]) -> tuple[int, int, int, int]:
    maximum = max(rgb)
    if maximum == 0:
        return (0, 0, 0, 0)
    fraction, exponent = math.frexp(maximum)
    scale = fraction * 256.0 / maximum
    return (
        int(rgb[0] * scale),
        int(rgb[1] * scale),
        int(rgb[2] * scale),
        exponent + 128,
    )


def write_hdr() -> None:
    a = (0.125, 0.25, 0.5)
    b = (0.5, 1.0, 2.0)
    c = (1.0, 2.0, 4.0)
    d = (2.0, 4.0, 8.0)
    rows = [
        [(4.0, 0.5, 0.125)] * WIDTH,
        [a, b, b, c, c, d, d, a],
        [a, b, b, c, c, d, d, a],
        [(8.0, 4.0, 1.0)] * WIDTH,
    ]
    payload = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n")
    for row in rows:
        scanline = [to_rgbe(pixel) for pixel in row]
        payload.extend((2, 2, 0, WIDTH))
        for channel_index in range(4):
            payload.append(WIDTH)
            payload.extend(pixel[channel_index] for pixel in scanline)
    (ROOT / "AnalyticalHDR.hdr").write_bytes(payload)


write_tga()
write_hdr()
