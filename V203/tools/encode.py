#!/usr/bin/env python3
"""从视频生成 64k / 224k 固件使用的三色 glyph、概率表和完整 speedtropy 码流。"""
import argparse
import ctypes as ct
import hashlib
import os
import subprocess
import tempfile
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
U8 = np.ctypeslib.ndpointer(dtype=np.uint8, flags="C_CONTIGUOUS")
U16 = np.ctypeslib.ndpointer(dtype=np.uint16, flags="C_CONTIGUOUS")
U32 = np.ctypeslib.ndpointer(dtype=np.uint32, flags="C_CONTIGUOUS")


def entropy(counts):
    total = counts.sum(axis=-1)
    with np.errstate(divide="ignore", invalid="ignore"):
        terms = np.where(counts > 0, counts * np.log2(np.maximum(counts, 1)), 0)
        return total * np.log2(np.maximum(total, 1)) - terms.sum(axis=-1)


def classify(transitions, classes):
    """Greedy minimum entropy grouping of previous glyphs, as in badderapple."""
    groups = np.zeros((classes, transitions.shape[1]), dtype=np.float64)
    assignment = np.zeros(len(transitions), dtype=np.uint8)
    # Assign high frequency rows first, then refine each group using real counts.
    for i in np.argsort(-transitions.sum(axis=1)):
        cost = entropy(groups + transitions[i]) - entropy(groups)
        group = int(cost.argmin())
        assignment[i] = group
        groups[group] += transitions[i]
    for _ in range(12):
        changed = 0
        for i in range(len(transitions)):
            old = int(assignment[i])
            groups[old] -= transitions[i]
            cost = entropy(groups + transitions[i]) - entropy(groups)
            group = int(cost.argmin())
            groups[group] += transitions[i]
            assignment[i] = group
            changed += group != old
        if not changed:
            break
    return assignment


def probability(zero, one):
    # speedtropy represents probability of zero. Keep impossible branches inexpensive
    # without using out-of-range probabilities or differing decoder rules.
    return np.clip(np.rint(256 * zero / np.maximum(zero + one, 1)), 1, 255).astype(np.uint8)


def pack_probabilities(values):
    """Seven bits encode odd probabilities 1..255, with a safe trailing byte."""
    rows = []
    for row in values:
        result = bytearray((len(row) * 7 + 7) // 8 + 1)
        for i, value in enumerate(row):
            word = (int(value) >> 1) << ((i * 7) % 8)
            offset = i * 7 // 8
            result[offset] |= word & 255
            result[offset + 1] |= word >> 8
        rows.append(list(result))
    return np.array(rows, dtype=np.uint8)


def c_array(name, values):
    values = np.asarray(values, dtype=np.uint8)
    dims = "".join(f"[{n}]" for n in values.shape)
    def rows(v):
        if v.ndim == 1:
            if len(v) > 16:
                return "{\n" + ",\n".join(",".join(str(int(x)) for x in v[i:i + 16])
                                           for i in range(0, len(v), 16)) + "\n}"
            return "{" + ",".join(str(int(x)) for x in v) + "}"
        return "{\n" + ",\n".join(rows(r) for r in v) + "\n}"
    return f"static const uint8_t {name}{dims} = {rows(values)};\n"


def transition_model(stream, k):
    previous = np.vstack([np.zeros((1, stream.shape[1]), np.uint16), stream[:-1]])
    same = stream == previous
    counts = np.zeros((k, k), np.int64)
    np.add.at(counts, (previous[~same], stream[~same]), 1)
    return previous, same, counts


def tree_model(transitions, groups, classes, k, bits):
    nodes = ((1 << bits) + k - 3) // 2 + 1
    counts = np.zeros((classes, nodes, 2), np.int64)
    old, new = np.nonzero(transitions)
    weights = transitions[old, new]
    node = np.zeros(len(old), np.int64)
    for shift in range(bits - 1, -1, -1):
        bit = (new >> shift) & 1
        np.add.at(counts, (groups[old], node, bit), weights)
        node = node * 2 + 1 + bit
    return probability(counts[:, :, 0], counts[:, :, 1])


def run_counts(previous, same, k, limit):
    counts = np.zeros((k, limit, 2), np.int64)
    age = np.zeros(previous.shape[1], np.uint16)
    for f in range(len(previous)):
        np.add.at(counts, (previous[f], age, same[f].astype(np.uint8)), 1)
        age = np.where(same[f], np.minimum(age + 1, limit - 1), 0)
    return counts


def model(lib, stream, glyphs, groups, tree, per_glyph_runs, classes, limit, precision):
    counts = np.zeros((classes, limit, 2), np.int64)
    np.add.at(counts, groups, per_glyph_runs)
    run = probability(counts[:, :, 0], counts[:, :, 1])
    if precision == 7:
        tree = (tree & 254) | 1
        run = (run & 254) | 1
    bits = (len(glyphs) - 1).bit_length()
    buffer = np.empty(stream.size * 2 + 4096, np.uint8)
    while True:
        n = lib.encode_tiles(stream, len(stream), stream.shape[1], len(glyphs), bits,
                             groups, tree, tree.shape[1], run, limit, buffer, len(buffer))
        if n == -2:
            raise ValueError("Invalid codec dimensions or glyph IDs")
        if n >= 0: break
        buffer = np.empty(len(buffer) * 2, np.uint8)
    return dict(glyphs=glyphs, classes=groups,
                tree=pack_probabilities(tree) if precision == 7 else tree,
                run=pack_probabilities(run) if precision == 7 else run,
                payload=buffer[:n].copy())


def merge(levels, stream, target):
    k = len(levels)
    frequency = np.bincount(stream.ravel(), minlength=k)
    pixels = levels.reshape(k, 64).astype(np.int64)
    norms = (pixels * pixels).sum(axis=1)
    distance = norms[:, None] + norms[None, :] - 2 * (pixels @ pixels.T)
    cost = frequency[:, None] * distance
    active = frequency > 0
    active[:2] = True
    assert np.all(levels[0] == 0) and np.all(levels[1] == 255)
    mapping = np.arange(k)
    previous = np.vstack([np.zeros((1, stream.shape[1]), np.uint16), stream[:-1]])
    transitions = np.zeros((k, k), np.int64)
    np.add.at(transitions, (previous.ravel(), stream.ravel()), 1)
    history = []
    while active.sum() > target:
        valid = active[:, None] & active[None, :] & ~np.eye(k, dtype=bool)
        valid[:2] = False
        delta = cost - np.diag(cost)[:, None]
        score = np.where(valid, delta / (transitions + transitions.T + 1), np.inf)
        source, destination = np.unravel_index(score.argmin(), score.shape)
        history.append(dict(source=int(source), destination=int(destination)))
        cost[destination] += cost[source]
        transitions[destination] += transitions[source]
        transitions[:, destination] += transitions[:, source]
        transitions[source] = 0
        transitions[:, source] = 0
        mapping[mapping == source] = destination
        active[source] = False
    representatives = np.flatnonzero(active)
    indices = np.zeros(k, np.uint16)
    indices[representatives] = np.arange(len(representatives), dtype=np.uint16)
    error = int((frequency * distance[np.arange(k), mapping]).sum())
    return levels[representatives], indices[mapping[stream]], history, error / (stream.size * 64 * 255 ** 2)


def native(directory):
    library = Path(directory) / "native.so"
    subprocess.run([os.environ.get("HOST_CC", "gcc"), "-O3", "-march=native", "-shared", "-fPIC",
                    str(ROOT / "tools/native.c"), "-o", str(library)], check=True)
    lib = ct.CDLL(str(library))
    lib.train_gray.argtypes = [U8, U32, ct.c_int, U8, ct.c_int, ct.c_int, ct.c_uint32]
    lib.quantize_gray.argtypes = [U8, ct.c_int, ct.c_int, U8, ct.c_int, ct.c_int, U16, ct.POINTER(ct.c_uint64)]
    lib.encode_tiles.argtypes = [U16, ct.c_uint, ct.c_uint, ct.c_uint, ct.c_uint, U8,
                                U8, ct.c_uint, U8, ct.c_uint, U8, ct.c_uint]
    lib.encode_tiles.restype = ct.c_int
    return lib


def train_ml(gray, levels, steps, threads):
    import torch
    import torch.nn.functional as F
    import lpips

    torch.set_num_threads(threads)
    torch.manual_seed(203)
    rng = np.random.default_rng(203)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    def ternary(value):
        return torch.where(value < 64 / 255, 0., torch.where(value < 192 / 255, 128 / 255, 1.))

    class Glyphs(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.blocks = torch.nn.Parameter(torch.from_numpy(levels.astype(np.float32) / 255).reshape(-1, 64))

        def forward(self, images, training=False):
            patches = F.unfold(images, 8, stride=8).transpose(1, 2)
            blocks = self.blocks if training else ternary(self.blocks)
            distances = patches.square().sum(-1, keepdim=True) + blocks.square().sum(-1) - 2 * patches @ blocks.T
            probabilities = torch.softmax(-distances * 35, dim=-1)
            weights = probabilities if training else F.one_hot(distances.argmin(-1), len(blocks)).to(blocks.dtype)
            prediction = F.fold((weights @ blocks).transpose(1, 2), gray.shape[1:], 8, stride=8)
            choice = torch.minimum(probabilities, 1 - probabilities).sum(-1).mean()
            return prediction, choice

    model = Glyphs().to(device)
    perceptual = lpips.LPIPS(net="vgg", verbose=False).to(device).eval()
    for parameter in perceptual.parameters():
        parameter.requires_grad_(False)
    optimizer = torch.optim.Adam(model.parameters(), lr=.002)
    validation = np.linspace(0, len(gray) - 1, min(96, max(1, len(gray) // 2)), dtype=int)
    training = np.setdiff1d(np.arange(len(gray)), validation)
    best, chosen = float("inf"), levels.copy()

    def batch(indices):
        return torch.from_numpy(np.array(gray[indices], copy=True)).to(device).float().unsqueeze(1) / 255

    def loss(a, b):
        return perceptual(a.expand(-1, 3, -1, -1) * 2 - 1, b.expand(-1, 3, -1, -1) * 2 - 1).mean()

    for step in range(1, steps + 1):
        target = batch(rng.choice(training, min(4, len(training)), replace=False))
        optimizer.zero_grad(set_to_none=True)
        prediction, choice = model(target, training=True)
        objective = loss(prediction, target) + choice + .01 * F.mse_loss(prediction, target)
        objective.backward()
        model.blocks.grad[:2].zero_()
        optimizer.step()
        with torch.no_grad():
            model.blocks.clamp_(0, 1)
            model.blocks[0].zero_()
            model.blocks[1].fill_(1)
        if step % 100 == 0 or step == steps:
            scores = []
            with torch.no_grad():
                for start in range(0, len(validation), 4):
                    target = batch(validation[start:start + 4])
                    prediction, _ = model(target)
                    scores.append(float(loss((prediction > .75).float(), target)))
                quantized = (ternary(model.blocks).cpu().numpy().reshape(-1, 8, 8) * 255).round().astype(np.uint8)
            score = float(np.mean(scores))
            if np.any(quantized != levels) and score < best:
                best, chosen = score, quantized.copy()
            print(f"ML {step}/{steps}, validation={score:.6f}", flush=True)
    return chosen


def write_header(path, width, height, stream, data, run_limit, description):
    k = len(data["glyphs"])
    prefix = ("// " + description + "\n#ifndef APPLE_DATA_H\n#define APPLE_DATA_H\n#include <stdint.h>\n"
              '#define APPLE_ASSET(name) __attribute__((section(".apple." #name), aligned(4)))\n'
              f"#define APPLE_WIDTH {width}u\n#define APPLE_HEIGHT {height}u\n"
              f"#define APPLE_COLS {width // 8}u\n#define APPLE_ROWS {height // 8}u\n#define APPLE_CELLS {stream.shape[1]}u\n"
              f"#define APPLE_FRAME_COUNT {len(stream)}u\n#define APPLE_FPS 30u\n"
              f"#define APPLE_GLYPH_COUNT {k}u\n#define APPLE_GLYPH_BITS {(k - 1).bit_length()}u\n"
              f"#define APPLE_RUN_LIMIT {run_limit}u\n")
    arrays = [c_array("apple_" + name, value).replace(" = ", f" APPLE_ASSET({name}) = ", 1)
              for name, value in data.items()]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(prefix + "\n".join(arrays) + "#endif\n")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("video", type=Path)
    p.add_argument("--profile", choices=["64k", "224k"], required=True)
    p.add_argument("--output", type=Path)
    p.add_argument("--glyphs", type=int, help="最终存储字形上限，默认 248 / 584")
    p.add_argument("--training-glyphs", type=int, help="训练码本大小，默认 256 / 624")
    p.add_argument("--classes", type=int, nargs="+", default=[16], help="给出多个数时逐项搜索")
    p.add_argument("--run-lengths", type=int, nargs="+", help="默认 32 / 16；给出多个数时逐项搜索")
    p.add_argument("--ml-steps", type=int, help="默认 0 / 1500；设 0 跳过 ML")
    p.add_argument("--threads", type=int, default=8)
    p.add_argument("--glyph-file", type=Path, help="加载已训练的三色 NPY 码本，跳过训练")
    p.add_argument("--save-glyphs", type=Path, help="可选：保存合并前的训练码本，便于调整压缩参数")
    a = p.parse_args()
    large = a.profile == "224k"
    width, height = (128, 96) if large else (64, 48)
    glyphs = a.glyphs if a.glyphs is not None else (584 if large else 248)
    count = a.training_glyphs if a.training_glyphs is not None else (624 if large else 256)
    limits = sorted(set(a.run_lengths if a.run_lengths is not None else [16 if large else 32]))
    steps = a.ml_steps if a.ml_steps is not None else (1500 if large else 0)
    if not 2 <= glyphs <= count <= 2048 or steps < 0 or a.threads < 1:
        p.error("glyphs <= training-glyphs，范围 2..2048；训练步数非负、线程数为正")
    if not all(1 <= n <= 255 for n in a.classes) or not all(1 <= n <= 256 for n in limits):
        p.error("class 范围 1..255，run 上下文范围 1..256")
    raw = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(a.video), "-an", "-vf",
                                   f"fps=30,scale={width}:{height}:flags=area",
                                   "-pix_fmt", "gray", "-f", "rawvideo", "-"])
    if not raw or len(raw) % (width * height):
        p.error("视频为空或帧数据不完整")
    gray = np.frombuffer(raw, np.uint8).reshape(-1, height, width)
    if steps and not a.glyph_file and len(gray) < 2:
        p.error("ML 训练至少需要两帧")
    cells = width * height // 64
    patches = gray.reshape(-1, height // 8, 8, width // 8, 8).transpose(0, 1, 3, 2, 4).reshape(-1, 64).copy()
    with tempfile.TemporaryDirectory(prefix="badapple-encode-") as work:
        lib = native(work)
        if a.glyph_file:
            levels = np.load(a.glyph_file)
            if levels.ndim != 3 or levels.shape[1:] != (8, 8) or levels.dtype != np.uint8:
                p.error("NPY 码本必须为 uint8，尺寸为 N×8×8")
            if len(levels) < glyphs or len(levels) > 2048 or not set(np.unique(levels)) <= {0, 128, 255}:
                p.error("字形数不足或像素不是 0/128/255")
            if not np.all(levels[0] == 0) or not np.all(levels[1] == 255):
                p.error("前两个字形必须是纯黑和纯白")
        else:
            samples = patches
            if large:
                edges = patches[(patches.max(1) != 0) & (patches.min(1) != 255)]
                if len(edges):
                    rng = np.random.default_rng(203)
                    samples = edges[rng.choice(len(edges), min(65536, len(edges)), replace=False)]
            unique, weights = np.unique(samples, axis=0, return_counts=True)
            centers = np.zeros((count, 64), np.uint8)
            print(f"K-means: {len(gray)} frames, {count} glyphs, {len(unique)} samples", flush=True)
            lib.train_gray(unique, weights.astype(np.uint32), len(unique), centers, count, 24, 203)
            if not large:
                distinct = list(dict.fromkeys(row.tobytes() for row in centers))
                centers = np.frombuffer(b"".join(distinct + [bytes(64)] * (count - len(distinct))), np.uint8).reshape(count, 64).copy()
            levels = centers.reshape(-1, 8, 8)
            if steps:
                levels = train_ml(gray, levels, steps, a.threads)
        if a.save_glyphs:
            a.save_glyphs.parent.mkdir(parents=True, exist_ok=True)
            np.save(a.save_glyphs, levels)
        stream = np.empty((len(gray), cells), np.uint16)
        error = ct.c_uint64()
        lib.quantize_gray(patches, len(gray), cells, levels.reshape(-1, 64).copy(), len(levels), 0, stream, ct.byref(error))
        levels, stream, _, _ = merge(levels, stream, glyphs)
        glyphs = np.concatenate([np.packbits(levels != 0, axis=2).reshape(len(levels), 8),
                                 np.packbits(levels == 255, axis=2).reshape(len(levels), 8)], axis=1)
        previous, same, transitions = transition_model(stream, len(levels))
        runs = {r: run_counts(previous, same, len(levels), r) for r in limits}
        best, chosen = float("inf"), None
        for classes in sorted(set(a.classes)):
            groups = classify(transitions, classes)
            tree = tree_model(transitions, groups, classes, len(levels), (len(levels) - 1).bit_length())
            for limit in limits:
                data = model(lib, stream, glyphs, groups, tree, runs[limit], classes, limit, 7)
                size = sum((v.nbytes + 3) // 4 * 4 for v in data.values())
                print(f"class={classes}, run={limit}, glyphs={len(levels)}, data={size} B", flush=True)
                if size < best:
                    best, chosen = size, (classes, limit, data)
        classes, limit, data = chosen
        description = (f"source SHA256={hashlib.sha256(a.video.read_bytes()).hexdigest()}; "
                       f"hold=0, class={classes}, run={limit}, glyphs={len(levels)}")
        path = a.output or ROOT / a.profile / "apple_data.h"
        write_header(path, width, height, stream, data, limit, description)
        print(f"Wrote {path}; data={best} B. Run make {a.profile} to check the complete Flash budget.")


if __name__ == "__main__":
    main()
