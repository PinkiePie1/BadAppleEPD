#!/usr/bin/env python3
"""将两份 apple_data.h 按固件棋盘格规则渲染为 side-by-side 视频。"""
import argparse
import contextlib
import os
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from recompress import ROOT, asset_model, decode, native, read_asset
from train_codec import unpack_levels


def load_stream(lib, path):
    macros, arrays = read_asset(path)
    model = asset_model(macros, arrays)
    model["class_count"] = int(model["classes"].max()) + 1
    glyphs = arrays["apple_glyphs"]
    stream = decode(lib, (macros["APPLE_FRAME_COUNT"], macros["APPLE_CELLS"]),
                    glyphs, model, arrays["apple_payload"])
    return macros, unpack_levels(glyphs), stream


def render_frame(levels, ids, cols, rows):
    image = levels[ids].reshape(rows, cols, 8, 8).transpose(0, 2, 1, 3)
    image = image.reshape(rows * 8, cols * 8)
    scale = 128 // (cols * 8)
    image = np.repeat(np.repeat(image, scale, axis=0), scale, axis=1)
    y, x = np.indices(image.shape)
    checker = (12 + x + 28 + y) & 1
    white = (image == 255) | ((image == 128) & (checker == 0))
    return white.astype(np.uint8) * 255


def font(size):
    paths = [Path("C:/Windows/Fonts/arial.ttf"),
             Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")]
    for path in paths:
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--scale", type=int, default=4)
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--frames", type=int, default=0)
    args = parser.parse_args()
    if args.scale < 1 or args.frames < 0 or args.start < 0:
        parser.error("scale 必须为正，frames/start 不能为负")
    work_context = (contextlib.nullcontext(ROOT / "build/training/native-preview") if os.name == "nt"
                    else tempfile.TemporaryDirectory(prefix="badapple-preview-"))
    with work_context as work:
        if os.name == "nt":
            work.mkdir(parents=True, exist_ok=True)
        lib = native(work)
        left_macros, left_levels, left_stream = load_stream(lib, args.left)
        right_macros, right_levels, right_stream = load_stream(lib, args.right)
    if left_stream.shape != right_stream.shape:
        raise ValueError("两侧帧数或 tile 数不一致")
    if args.start >= len(left_stream):
        parser.error("start 超过视频帧数")
    remaining = len(left_stream) - args.start
    count = min(remaining, args.frames) if args.frames else remaining
    panel_width, panel_height = 128 * args.scale, 96 * args.scale
    width, height = panel_width * 2, panel_height + 32
    text_font = font(20)
    header = Image.new("L", (width, 32), 0)
    draw = ImageDraw.Draw(header)
    draw.text((12, 5), f"Original 64k - {len(left_levels)} glyphs", fill=255, font=text_font)
    draw.text((panel_width + 12, 5), f"Trained 64k - {len(right_levels)} glyphs", fill=255, font=text_font)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = [args.ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray",
               "-s", f"{width}x{height}", "-r", "30", "-i", "-", "-an",
               "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
               "-movflags", "+faststart", str(args.output)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    for index in range(count):
        f = args.start + index
        left = render_frame(left_levels, left_stream[f], left_macros["APPLE_COLS"], left_macros["APPLE_ROWS"])
        right = render_frame(right_levels, right_stream[f], right_macros["APPLE_COLS"], right_macros["APPLE_ROWS"])
        canvas = Image.new("L", (width, height), 0)
        canvas.paste(header, (0, 0))
        canvas.paste(Image.fromarray(left).resize((panel_width, panel_height), Image.Resampling.NEAREST), (0, 32))
        canvas.paste(Image.fromarray(right).resize((panel_width, panel_height), Image.Resampling.NEAREST), (panel_width, 32))
        process.stdin.write(canvas.tobytes())
        if (index + 1) % 500 == 0 or index + 1 == count:
            print(f"rendered {index + 1}/{count}", flush=True)
    process.stdin.close()
    if process.wait():
        raise RuntimeError("ffmpeg 编码失败")
    print(f"wrote {args.output}", flush=True)


if __name__ == "__main__":
    main()
