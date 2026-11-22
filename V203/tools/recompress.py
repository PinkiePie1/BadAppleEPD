#!/usr/bin/env python3
"""复用现有固件字库，重建 glyph 流并搜索更小的预测模型。"""
import argparse
import contextlib
import ctypes as ct
import os
import re
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from encode import c_array, classify, entropy, probability, transition_model, tree_model

ROOT = Path(__file__).resolve().parents[1]
U8 = np.ctypeslib.ndpointer(dtype=np.uint8, flags="C_CONTIGUOUS")
U16 = np.ctypeslib.ndpointer(dtype=np.uint16, flags="C_CONTIGUOUS")


def native(directory):
    if os.name == "nt":
        directory = Path(directory) / f"native-{os.getpid()}"
        directory.mkdir(parents=True, exist_ok=True)
    library = Path(directory) / "native.so"
    env = None
    if os.name == "nt":
        host_tmp = ROOT / "build/training/host-tmp"
        host_tmp.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["TMPDIR"] = env["TMP"] = env["TEMP"] = str(host_tmp)
    subprocess.run(["gcc", "-O3", "-march=native", "-shared", "-fPIC",
                    str(ROOT / "tools/native.c"), "-o", str(library)], check=True, env=env)
    lib = ct.CDLL(str(library))
    lib.encode_tiles2.argtypes = [U16, ct.c_uint, ct.c_uint, ct.c_uint, ct.c_uint, U8,
                                 ct.c_uint, U8, ct.c_uint, U8, ct.c_uint, U8,
                                 ct.c_uint, U8, ct.c_uint, U8, ct.c_uint]
    lib.encode_tiles2.restype = ct.c_int
    lib.decode_tiles2.argtypes = [U16, ct.c_uint, ct.c_uint, ct.c_uint, ct.c_uint, U8,
                                 ct.c_uint, U8, ct.c_uint, U8, ct.c_uint, U8,
                                 ct.c_uint, U8, ct.c_uint, U8, ct.c_uint]
    lib.decode_tiles2.restype = ct.c_int
    return lib


def read_asset(path):
    text = path.read_text()
    macros = {name: int(value) for name, value in
              re.findall(r"#define\s+(APPLE_\w+)\s+(\d+)u", text)}
    arrays = {}
    pattern = re.compile(r"static const uint8_t\s+(\w+)((?:\[\d+\])+)\s+"
                         r"APPLE_ASSET\(\w+\)\s*=\s*(.*?);", re.S)
    for name, dimensions, body in pattern.findall(text):
        shape = tuple(int(n) for n in re.findall(r"\d+", dimensions))
        values = np.fromiter((int(n) for n in re.findall(r"\d+", body)), np.uint8)
        arrays[name] = values.reshape(shape).copy()
    required = {"apple_glyphs", "apple_classes", "apple_tree", "apple_run", "apple_payload"}
    if not required <= arrays.keys():
        raise ValueError(f"头文件缺少数组：{required - arrays.keys()}")
    return macros, arrays


def unpack_probabilities(packed, count, bits=7):
    indices = np.arange(count)
    offsets = indices * bits // 8
    words = packed[:, offsets].astype(np.uint16) | (packed[:, offsets + 1].astype(np.uint16) << 8)
    shift = 8 - bits
    return ((((words >> ((indices * bits) % 8)) & ((1 << bits) - 1)) << shift) + (1 << (shift - 1))).astype(np.uint8)


def pack_probability_bits(values, bits):
    rows = []
    shift = 8 - bits
    for row in values:
        result = bytearray((len(row) * bits + 7) // 8 + 1)
        for i, value in enumerate(row):
            word = (int(value) >> shift) << ((i * bits) % 8)
            offset = i * bits // 8
            result[offset] |= word & 255
            result[offset + 1] |= word >> 8
        rows.append(list(result))
    return np.array(rows, dtype=np.uint8)


def asset_model(macros, arrays):
    k = macros["APPLE_GLYPH_COUNT"]
    bits = macros["APPLE_GLYPH_BITS"]
    prob_bits = macros.get("APPLE_PROB_BITS", 7)
    nodes = ((1 << bits) + k - 3) // 2 + 1
    limit = macros["APPLE_RUN_LIMIT"]
    buckets = macros.get("APPLE_RUN_BUCKETS", limit)
    context_order = macros.get("APPLE_CONTEXT_ORDER", 1)
    run_order = macros.get("APPLE_RUN_ORDER", context_order)
    order = 1 if context_order == 1 else (2 if run_order == 2 else 3)
    pairs = arrays.get("apple_pairs", np.zeros(1, np.uint8))
    run_map = arrays.get("apple_run_map", np.arange(limit, dtype=np.uint8))
    return dict(classes=arrays["apple_classes"], pairs=pairs, order=order, prob_bits=prob_bits,
                tree=unpack_probabilities(arrays["apple_tree"], nodes, prob_bits),
                run=unpack_probabilities(arrays["apple_run"], buckets, prob_bits),
                run_map=run_map, run_limit=limit)


def decode(lib, stream_shape, glyphs, model, payload):
    frames, cells = stream_shape
    k = len(glyphs)
    bits = (k - 1).bit_length()
    stream = np.empty((frames, cells), np.uint16)
    classes = np.ascontiguousarray(model["classes"], np.uint8)
    pairs = np.ascontiguousarray(model.get("pairs", np.zeros(1, np.uint8)), np.uint8)
    tree = np.ascontiguousarray(model["tree"], np.uint8)
    run = np.ascontiguousarray(model["run"], np.uint8)
    run_map = np.ascontiguousarray(model["run_map"], np.uint8)
    result = lib.decode_tiles2(stream, frames, cells, k, bits, classes, model["class_count"],
                               pairs, model["order"], tree, tree.shape[1], run, run.shape[1],
                               run_map, model["run_limit"], payload, len(payload))
    if result:
        raise ValueError(f"主机解码失败，错误码 {result}")
    return stream


def encode(lib, stream, glyphs, model):
    frames, cells = stream.shape
    k = len(glyphs)
    bits = (k - 1).bit_length()
    classes = np.ascontiguousarray(model["classes"], np.uint8)
    class_count = int(model["class_count"])
    pairs = np.ascontiguousarray(model.get("pairs", np.zeros(1, np.uint8)), np.uint8)
    prob_bits = model.get("prob_bits", 7)
    if not 1 <= prob_bits <= 7:
        raise ValueError("概率表精度范围 1..7 bit")
    step = 1 << (8 - prob_bits)
    tree_values = np.clip(model["tree"] // step, 0, (1 << prob_bits) - 1)
    run_values = np.clip(model["run"] // step, 0, (1 << prob_bits) - 1)
    tree = np.ascontiguousarray(tree_values * step + step // 2, np.uint8)
    run = np.ascontiguousarray(run_values * step + step // 2, np.uint8)
    model["tree"], model["run"], model["prob_bits"] = tree, run, prob_bits
    run_map = np.ascontiguousarray(model["run_map"], np.uint8)
    buffer = np.empty(stream.size * 2 + 4096, np.uint8)
    while True:
        n = lib.encode_tiles2(stream, frames, cells, k, bits, classes, class_count,
                              pairs, model["order"], tree, tree.shape[1], run,
                              run.shape[1], run_map, len(run_map), buffer, len(buffer))
        if n == -2:
            raise ValueError("编码参数或 glyph ID 无效")
        if n >= 0:
            break
        buffer = np.empty(len(buffer) * 2, np.uint8)
    data = dict(glyphs=glyphs, classes=classes)
    if model["order"] >= 2:
        data["pairs"] = pairs
    if not np.array_equal(run_map, np.arange(len(run_map), dtype=np.uint8)):
        data["run_map"] = run_map
    data.update(tree=pack_probability_bits(tree, prob_bits), run=pack_probability_bits(run, prob_bits),
                payload=buffer[:n].copy())
    return data


def aligned_size(data):
    return sum((value.nbytes + 3) // 4 * 4 for value in data.values())


def write_header(path, macros, stream, data, model, description):
    k = len(data["glyphs"])
    context_order = 2 if model["order"] >= 2 else 1
    run_order = 2 if model["order"] == 2 else 1
    prefix = ("// " + description + "\n#ifndef APPLE_DATA_H\n#define APPLE_DATA_H\n#include <stdint.h>\n"
              '#define APPLE_ASSET(name) __attribute__((section(".apple." #name), aligned(4)))\n'
              f"#define APPLE_WIDTH {macros['APPLE_WIDTH']}u\n#define APPLE_HEIGHT {macros['APPLE_HEIGHT']}u\n"
              f"#define APPLE_COLS {macros['APPLE_COLS']}u\n#define APPLE_ROWS {macros['APPLE_ROWS']}u\n"
              f"#define APPLE_CELLS {stream.shape[1]}u\n#define APPLE_FRAME_COUNT {len(stream)}u\n"
              f"#define APPLE_FPS {macros['APPLE_FPS']}u\n#define APPLE_GLYPH_COUNT {k}u\n"
              f"#define APPLE_GLYPH_BITS {(k - 1).bit_length()}u\n"
              f"#define APPLE_CONTEXT_ORDER {context_order}u\n#define APPLE_RUN_ORDER {run_order}u\n"
              f"#define APPLE_CLASS_COUNT {model['class_count']}u\n#define APPLE_MODEL_COUNT {model['tree'].shape[0]}u\n"
              f"#define APPLE_RUN_LIMIT {model['run_limit']}u\n#define APPLE_RUN_BUCKETS {model['run'].shape[1]}u\n"
              f"#define APPLE_RUN_MAPPED {1 if 'run_map' in data else 0}u\n"
              f"#define APPLE_PROB_BITS {model.get('prob_bits', 7)}u\n")
    arrays = [c_array("apple_" + name, value).replace(" = ", f" APPLE_ASSET({name}) = ", 1)
              for name, value in data.items()]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(prefix + "\n".join(arrays) + "#endif\n", newline="\n")


def print_candidate(name, data, model):
    details = " ".join(f"{key}={value.nbytes}" for key, value in data.items())
    print(f"{name}: total={aligned_size(data)} order={model['order']} "
          f"classes={model['class_count']} models={model['tree'].shape[0]} "
          f"run={model['run_limit']}/{model['run'].shape[1]}; {details}", flush=True)


def run_counts_context(context, same, count, run_map):
    buckets = int(run_map.max()) + 1
    counts = np.zeros((count, buckets, 2), np.int64)
    age = np.zeros(context.shape[1], np.uint16)
    for f in range(len(context)):
        np.add.at(counts, (context[f], run_map[age], same[f].astype(np.uint8)), 1)
        age = np.where(same[f], np.minimum(age + 1, len(run_map) - 1), 0)
    return counts


def bucket_counts(age_counts, run_map):
    counts = np.zeros((len(age_counts), int(run_map.max()) + 1, 2), np.int64)
    for age, bucket in enumerate(run_map):
        counts[:, bucket] += age_counts[:, age]
    return counts


def quantized_cost(counts, prob_bits=7):
    zero = counts[..., 0].astype(np.float64)
    one = counts[..., 1].astype(np.float64)
    prob = np.clip(np.rint(256 * zero / np.maximum(zero + one, 1)), 1, 255).astype(np.int64)
    step = 1 << (8 - prob_bits)
    prob = np.clip(prob // step, 0, (1 << prob_bits) - 1) * step + step // 2
    return -(zero * np.log2(prob / 256) + one * np.log2(1 - prob / 256))


def optimize_run_map(model_age_counts, buckets, prob_bits=7):
    limit = model_age_counts.shape[1]
    if buckets > limit:
        raise ValueError("run 桶数不能超过年龄范围")
    segment = np.zeros((limit, limit + 1))
    for start in range(limit):
        for end in range(start + 1, limit + 1):
            segment[start, end] = quantized_cost(model_age_counts[:, start:end].sum(axis=1), prob_bits).sum()
    costs = np.full((buckets + 1, limit + 1), np.inf)
    previous = np.zeros((buckets + 1, limit + 1), np.int64)
    costs[0, 0] = 0
    for count in range(1, buckets + 1):
        for end in range(count, limit + 1):
            starts = np.arange(count - 1, end)
            values = costs[count - 1, starts] + segment[starts, end]
            best = int(values.argmin())
            costs[count, end] = values[best]
            previous[count, end] = starts[best]
    cuts = []
    end = limit
    for count in range(buckets, 1, -1):
        end = int(previous[count, end])
        cuts.append(end)
    cuts = np.array(sorted(cuts))
    return np.searchsorted(cuts, np.arange(limit), side="right").astype(np.uint8), cuts, costs[buckets, limit]


def tree_event_counts(transitions, bits):
    k = transitions.shape[1]
    nodes = ((1 << bits) + k - 3) // 2 + 1
    counts = np.zeros((len(transitions), nodes, 2), np.int64)
    old, new = np.nonzero(transitions)
    weights = transitions[old, new]
    node = np.zeros(len(old), np.int64)
    for shift in range(bits - 1, -1, -1):
        bit = (new >> shift) & 1
        np.add.at(counts, (old, node, bit), weights)
        node = node * 2 + 1 + bit
    return counts


def joint_refine(transitions, runs, assignment, classes, bits, seed, passes=10, tree_rows=None):
    if tree_rows is None:
        tree_rows = tree_event_counts(transitions, bits)
    assignment = assignment.astype(np.uint8, copy=True)
    tree_groups = np.zeros((classes, tree_rows.shape[1], 2), np.int64)
    run_groups = np.zeros((classes, runs.shape[1], 2), np.int64)
    np.add.at(tree_groups, assignment, tree_rows)
    np.add.at(run_groups, assignment, runs)
    rng = np.random.default_rng(seed)
    for iteration in range(passes):
        changed = 0
        order = rng.permutation(len(assignment))
        for i in order:
            old = int(assignment[i])
            tree_groups[old] -= tree_rows[i]
            run_groups[old] -= runs[i]
            cost = (entropy(tree_groups + tree_rows[i]) - entropy(tree_groups)).sum(axis=1)
            cost += (entropy(run_groups + runs[i]) - entropy(run_groups)).sum(axis=1)
            group = int(cost.argmin())
            tree_groups[group] += tree_rows[i]
            run_groups[group] += runs[i]
            assignment[i] = group
            changed += group != old
        if not changed:
            break
    return assignment


def em_classify(transitions, classes, seed):
    totals = transitions.sum(axis=1)
    active = np.flatnonzero(totals)
    points = np.zeros_like(transitions, dtype=np.float64)
    points[active] = np.sqrt(transitions[active] / totals[active, None])
    rng = np.random.default_rng(seed)
    first = rng.choice(active, p=totals[active] / totals.sum())
    centers = np.empty((classes, transitions.shape[1]), np.float64)
    centers[0] = points[first]
    closest = np.full(len(transitions), np.inf)
    for c in range(1, classes):
        closest = np.minimum(closest, ((points - centers[c - 1]) ** 2).sum(axis=1))
        weights = closest * totals
        if weights.sum():
            pick = rng.choice(len(transitions), p=weights / weights.sum())
        else:
            pick = rng.choice(active)
        centers[c] = points[pick]
    assignment = np.zeros(len(transitions), np.uint8)
    for _ in range(24):
        distance = ((points ** 2).sum(axis=1)[:, None] + (centers ** 2).sum(axis=1)
                    - 2 * points @ centers.T)
        new_assignment = distance.argmin(axis=1).astype(np.uint8)
        if np.array_equal(new_assignment, assignment):
            break
        assignment = new_assignment
        for c in range(classes):
            selected = assignment == c
            if np.any(selected):
                weights = totals[selected, None]
                centers[c] = (points[selected] * weights).sum(axis=0) / max(weights.sum(), 1)
    responsibilities = np.eye(classes)[assignment]
    for _ in range(16):
        counts = responsibilities.T @ transitions
        distributions = (counts + .01) / (counts.sum(axis=1, keepdims=True) + .01 * transitions.shape[1])
        scores = transitions @ np.log(distributions.T)
        scores -= scores.max(axis=1, keepdims=True)
        responsibilities = np.exp(scores)
        responsibilities /= responsibilities.sum(axis=1, keepdims=True)
    result = scores.argmax(axis=1).astype(np.uint8)
    result[totals == 0] = assignment[totals == 0]
    return result


def first_order_model(lib, stream, glyphs, groups, transitions, per_glyph_runs, run_map, prob_bits=7):
    classes = len(groups) and int(groups.max()) + 1
    k = len(glyphs)
    bits = (k - 1).bit_length()
    tree = tree_model(transitions, groups, classes, k, bits)
    counts = np.zeros((classes, per_glyph_runs.shape[1], 2), np.int64)
    np.add.at(counts, groups, per_glyph_runs)
    run = probability(counts[:, :, 0], counts[:, :, 1])
    model = dict(classes=groups.astype(np.uint8), class_count=classes, order=1, prob_bits=prob_bits,
                 tree=tree, run=run, run_map=run_map, run_limit=len(run_map))
    data = encode(lib, stream, glyphs, model)
    return model, data


def second_order_stats(stream, previous, same, groups, class_count, run_map):
    older = np.empty_like(stream)
    state = np.zeros(stream.shape[1], np.uint16)
    for f in range(len(stream)):
        older[f] = state
        changed = ~same[f]
        state[changed] = previous[f][changed]
    context = (groups[older].astype(np.uint16) * class_count + groups[previous]).astype(np.uint16)
    transitions = np.zeros((class_count * class_count, stream.max() + 1), np.int64)
    np.add.at(transitions, (context[~same], stream[~same]), 1)
    runs = run_counts_context(context, same, class_count * class_count, run_map)
    return transitions, runs


def second_order_model(lib, stream, glyphs, groups, pairs, transitions, runs, run_map,
                       first_runs=None, tree_only=False, prob_bits=7):
    class_count = int(groups.max()) + 1
    models = int(pairs.max()) + 1
    bits = (len(glyphs) - 1).bit_length()
    tree = tree_model(transitions, pairs, models, len(glyphs), bits)
    run_models = class_count if tree_only else models
    counts = np.zeros((run_models, runs.shape[1], 2), np.int64)
    if tree_only:
        np.add.at(counts, groups, first_runs)
    else:
        np.add.at(counts, pairs, runs)
    run = probability(counts[:, :, 0], counts[:, :, 1])
    model = dict(classes=groups.astype(np.uint8), class_count=class_count,
                 pairs=pairs.astype(np.uint8), order=3 if tree_only else 2, tree=tree, run=run,
                 run_map=run_map, run_limit=len(run_map), prob_bits=prob_bits)
    data = encode(lib, stream, glyphs, model)
    return model, data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "224k/apple_data.h")
    parser.add_argument("--cache", type=Path, default=ROOT / "build/compression/reconstructed224.npz")
    parser.add_argument("--output", type=Path, default=ROOT / "build/compression/best.h")
    parser.add_argument("--mode", choices=["baseline", "run", "class", "order2", "order2tree", "runopt", "precision", "renumber"], default="baseline")
    parser.add_argument("--classes", type=int, nargs="+", default=[8, 12, 16, 20, 24])
    parser.add_argument("--models", type=int, nargs="+", default=[8, 12, 16])
    parser.add_argument("--prob-bits", type=int, default=7)
    parser.add_argument("--seeds", type=int, nargs="+", default=[203, 204, 205])
    parser.add_argument("--rebuild-cache", action="store_true")
    args = parser.parse_args()
    macros, arrays = read_asset(args.input)
    original = asset_model(macros, arrays)
    glyphs = arrays["apple_glyphs"]
    shape = (macros["APPLE_FRAME_COUNT"], macros["APPLE_CELLS"])
    work_context = (contextlib.nullcontext(ROOT / "build/recompression/native-host") if os.name == "nt"
                    else tempfile.TemporaryDirectory(prefix="badapple-recompress-"))
    with work_context as work:
        if os.name == "nt":
            work.mkdir(parents=True, exist_ok=True)
        lib = native(work)
        stream = None
        if args.cache.exists() and not args.rebuild_cache:
            cached = np.load(args.cache)
            cached_stream = cached["stream"]
            if np.array_equal(cached["glyphs"], glyphs) and cached_stream.shape == shape:
                stream = cached_stream
        if stream is None:
            original["class_count"] = int(original["classes"].max()) + 1
            stream = decode(lib, shape, glyphs, original, arrays["apple_payload"])
            args.cache.parent.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(args.cache, stream=stream, glyphs=glyphs)
            print(f"Decoded {stream.size} glyph IDs to {args.cache}", flush=True)
        original["class_count"] = int(original["classes"].max()) + 1
        baseline = encode(lib, stream, glyphs, original)
        if not np.array_equal(baseline["payload"], arrays["apple_payload"]):
            raise ValueError("原参数重编码与现有 payload 不一致")
        decoded = decode(lib, shape, glyphs, original, baseline["payload"])
        if not np.array_equal(decoded, stream):
            raise ValueError("基线 round-trip 不一致")
        print_candidate("baseline", baseline, original)
        if args.mode == "baseline":
            return

        previous, same, transitions = transition_model(stream, len(glyphs))
        reuse_groups = args.mode in ("order2", "order2tree", "runopt", "precision", "renumber")
        groups = original["classes"] if reuse_groups else classify(transitions, 16)
        if not reuse_groups and not np.array_equal(groups, original["classes"]):
            print("注意：重新计算的贪婪 classes 与头文件不同", flush=True)
        uniform16 = np.arange(16, dtype=np.uint8)
        six32 = np.searchsorted(np.array([1, 2, 4, 8, 16]), np.arange(32), side="right").astype(np.uint8)
        best = None
        best_stream, best_glyphs = stream, glyphs

        def consider(name, model, data, candidate_stream=None, candidate_glyphs=None):
            nonlocal best, best_stream, best_glyphs
            print_candidate(name, data, model)
            size = aligned_size(data)
            if best is None or size < best[0]:
                best = size, name, model, data
                best_stream = stream if candidate_stream is None else candidate_stream
                best_glyphs = glyphs if candidate_glyphs is None else candidate_glyphs

        if args.mode == "run":
            run_maps = {"uniform16": uniform16, "uniform32": np.arange(32, dtype=np.uint8), "six32": six32}
            for name, run_map in run_maps.items():
                runs = run_counts_context(previous, same, len(glyphs), run_map)
                model, data = first_order_model(lib, stream, glyphs, groups, transitions, runs, run_map)
                consider(name, model, data)
        elif args.mode == "class":
            runs = run_counts_context(previous, same, len(glyphs), six32)
            bits = (len(glyphs) - 1).bit_length()
            tree_rows = tree_event_counts(transitions, bits)
            for classes in args.classes:
                greedy = classify(transitions, classes)
                assignments = [(f"c{classes}-greedy", greedy),
                               (f"c{classes}-greedy-joint", joint_refine(transitions, runs, greedy, classes,
                                                                         bits, 203, tree_rows=tree_rows))]
                for seed in args.seeds:
                    initial = em_classify(transitions, classes, seed)
                    refined = joint_refine(transitions, runs, initial, classes, bits, seed, tree_rows=tree_rows)
                    assignments.append((f"c{classes}-em{seed}", refined))
                for name, assignment in assignments:
                    model, data = first_order_model(lib, stream, glyphs, assignment, transitions, runs, six32,
                                                    args.prob_bits)
                    consider(name, model, data)
            _, name, model, data = best
            runs16 = run_counts_context(previous, same, len(glyphs), uniform16)
            model16, data16 = first_order_model(lib, stream, glyphs, model["classes"], transitions, runs16,
                                                uniform16, args.prob_bits)
            consider(name + "-uniform16", model16, data16)
        elif args.mode == "renumber":
            if original["order"] != 1:
                raise ValueError("renumber 当前要求一阶输入模型")
            best = aligned_size(baseline), "input", original, baseline
            run_map = original["run_map"]
            orderings = {
                "frequency": np.argsort(-np.bincount(stream.ravel(), minlength=len(glyphs)), kind="stable"),
                "changes": np.argsort(-transitions.sum(axis=0), kind="stable"),
            }
            for ordering_name, old_order in orderings.items():
                remap = np.empty(len(glyphs), np.uint16)
                remap[old_order] = np.arange(len(glyphs), dtype=np.uint16)
                new_glyphs = np.ascontiguousarray(glyphs[old_order])
                if not np.array_equal(new_glyphs[remap], glyphs):
                    raise ValueError("glyph 重编号映射不一致")
                new_stream = remap[stream]
                new_previous, new_same, new_transitions = transition_model(new_stream, len(glyphs))
                new_runs = run_counts_context(new_previous, new_same, len(glyphs), run_map)
                new_groups = groups[old_order]
                tree_rows = tree_event_counts(new_transitions, (len(glyphs) - 1).bit_length())
                refined = joint_refine(new_transitions, new_runs, new_groups, original["class_count"],
                                       (len(glyphs) - 1).bit_length(), 203, tree_rows=tree_rows)
                for suffix, assignment in [("permuted", new_groups), ("refined", refined)]:
                    model, data = first_order_model(lib, new_stream, new_glyphs, assignment, new_transitions,
                                                    new_runs, run_map, original.get("prob_bits", 7))
                    consider(f"renumber-{ordering_name}-{suffix}", model, data, new_stream, new_glyphs)
        elif args.mode == "precision":
            if original["order"] != 1:
                raise ValueError("precision 当前要求一阶输入模型")
            best = aligned_size(baseline), "input", original, baseline
            run_map = original["run_map"]
            runs = run_counts_context(previous, same, len(glyphs), run_map)
            for prob_bits in [2, 3, 4, 5, 6, 7]:
                model, data = first_order_model(lib, stream, glyphs, groups, transitions, runs, run_map, prob_bits)
                consider(f"precision{prob_bits}", model, data)
        elif args.mode == "runopt":
            if original["order"] != 1:
                raise ValueError("runopt 当前要求一阶输入模型")
            best = aligned_size(baseline), "input", original, baseline
            for limit in [16, 32, 64]:
                age_counts = run_counts_context(previous, same, len(glyphs), np.arange(limit, dtype=np.uint8))
                model_age = np.zeros((original["class_count"], limit, 2), np.int64)
                np.add.at(model_age, groups, age_counts)
                for buckets in [4, 6, 8, 10, 12]:
                    run_map, cuts, _ = optimize_run_map(model_age, buckets, original.get("prob_bits", 7))
                    runs = bucket_counts(age_counts, run_map)
                    model, data = first_order_model(lib, stream, glyphs, groups, transitions, runs, run_map,
                                                    original.get("prob_bits", 7))
                    consider(f"runopt-l{limit}b{buckets}-{'-'.join(map(str, cuts))}", model, data)
        else:
            best = aligned_size(baseline), "input", original, baseline
            tree_only = args.mode == "order2tree"
            run_map = original["run_map"]
            class_count = original["class_count"]
            pair_transitions, pair_runs = second_order_stats(stream, previous, same, groups, class_count, run_map)
            first_runs = run_counts_context(previous, same, len(glyphs), run_map) if tree_only else None
            fit_runs = np.zeros((len(pair_runs), 1, 2), np.int64) if tree_only else pair_runs
            bits = (len(glyphs) - 1).bit_length()
            tree_rows = tree_event_counts(pair_transitions, bits)
            for models in args.models:
                if models > class_count * class_count:
                    continue
                greedy = classify(pair_transitions, models)
                prefix = "t" if tree_only else "p"
                assignments = [(f"{prefix}{models}-greedy", greedy),
                               (f"{prefix}{models}-greedy-joint", joint_refine(pair_transitions, fit_runs, greedy, models,
                                                                               bits, 203, tree_rows=tree_rows))]
                for seed in args.seeds:
                    initial = em_classify(pair_transitions, models, seed)
                    refined = joint_refine(pair_transitions, fit_runs, initial, models, bits, seed, tree_rows=tree_rows)
                    assignments.append((f"{prefix}{models}-em{seed}", refined))
                for name, assignment in assignments:
                    model, data = second_order_model(lib, stream, glyphs, groups, assignment,
                                                     pair_transitions, pair_runs, run_map,
                                                     first_runs=first_runs, tree_only=tree_only,
                                                     prob_bits=original.get("prob_bits", 7))
                    consider(name, model, data)
        _, name, model, data = best
        write_header(args.output, macros, best_stream, data, model,
                     f"recompressed {name}; identical decoded glyph images")
        decoded = decode(lib, shape, best_glyphs, model, data["payload"])
        if not np.array_equal(decoded, best_stream):
            raise ValueError("最佳候选 round-trip 不一致")
        print(f"Best {name}: {aligned_size(data)} B; wrote {args.output}", flush=True)


if __name__ == "__main__":
    main()
