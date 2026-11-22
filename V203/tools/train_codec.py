#!/usr/bin/env python3
"""以真实 tree/run 概率为码率先验，联合优化 glyph 感知画质与期望码长。"""
import argparse
import contextlib
import ctypes as ct
import hashlib
import os
import subprocess
import tempfile
import time
from pathlib import Path

import numpy as np

from recompress import (ROOT, U8, U16, aligned_size, asset_model, decode, first_order_model, native,
                        read_asset, run_counts_context, transition_model, write_header)

SOURCE_SHA256 = "dc4faefff54e668394f91a383a4259777ab86d751b4a1277b869b55731de1913"
U32 = np.ctypeslib.ndpointer(dtype=np.uint32, flags="C_CONTIGUOUS")


def unpack_levels(glyphs):
    low = np.unpackbits(glyphs[:, :8], axis=1)
    high = np.unpackbits(glyphs[:, 8:], axis=1)
    return np.where(high, 255, low * 128).astype(np.uint8).reshape(-1, 8, 8)


def pack_levels(levels):
    return np.concatenate([np.packbits(levels != 0, axis=2).reshape(len(levels), 8),
                           np.packbits(levels == 255, axis=2).reshape(len(levels), 8)], axis=1)


def compact_levels(levels, stream):
    active = np.unique(np.concatenate([stream.ravel(), np.array([0, 1], np.uint16)]))
    mapping = np.zeros(len(levels), np.uint16)
    mapping[active] = np.arange(len(active), dtype=np.uint16)
    return levels[active].copy(), mapping[stream], active


def load_gray(video, cache, ffmpeg, width, height):
    if cache.exists():
        gray = np.load(cache)
        if gray.ndim == 3 and gray.shape[1:] == (height, width) and gray.dtype == np.uint8:
            return gray
    source = str(video)
    if os.name == "posix" and ffmpeg.lower().endswith(".exe"):
        source = subprocess.check_output(["wslpath", "-w", str(video.resolve())], text=True).strip()
    raw = subprocess.check_output([ffmpeg, "-v", "error", "-i", source, "-an", "-vf",
                                   f"fps=30,scale={width}:{height}:flags=area", "-pix_fmt", "gray",
                                   "-f", "rawvideo", "-"])
    if not raw or len(raw) % (width * height):
        raise ValueError("视频为空或帧数据不完整")
    gray = np.frombuffer(raw, np.uint8).reshape(-1, height, width).copy()
    cache.parent.mkdir(parents=True, exist_ok=True)
    np.save(cache, gray)
    return gray


def patches_from_gray(gray, cols, rows):
    return gray.reshape(-1, rows, 8, cols, 8).transpose(0, 1, 3, 2, 4).reshape(-1, cols * rows, 64).copy()


def select_device(request):
    import torch
    if request in ("dml", "auto"):
        try:
            import torch_directml
            if not torch_directml.is_available():
                raise RuntimeError("DirectML reports no available device")
            device = torch_directml.device()
            a = torch.arange(1024, dtype=torch.float32, device=device).reshape(32, 32)
            result = float((a @ a).sum().cpu())
            if not np.isfinite(result) or result == 0:
                raise RuntimeError("DirectML tensor smoke test returned invalid data")
            print(f"DirectML: {torch_directml.device_name(0).rstrip(chr(0))}", flush=True)
            return device
        except Exception:
            if request == "dml":
                raise
            print("DirectML unavailable; trying ROCm", flush=True)
    if request in ("rocm", "auto"):
        try:
            if not torch.version.hip or not torch.cuda.is_available():
                raise RuntimeError("PyTorch reports no HIP/CUDA device")
            device = torch.device("cuda")
            a = torch.arange(1024, dtype=torch.float32, device=device).reshape(32, 32)
            result = float((a @ a).sum().cpu())
            if not np.isfinite(result) or result == 0:
                raise RuntimeError("ROCm tensor smoke test returned invalid data")
            print(f"ROCm: {torch.cuda.get_device_name(0)}; HIP={torch.version.hip}", flush=True)
            return device
        except Exception:
            if request == "rocm":
                raise
            print("ROCm unavailable; using CPU", flush=True)
    return torch.device("cpu")


def stream_ages(stream, same, limit):
    result = np.empty(stream.shape, np.uint8)
    age = np.zeros(stream.shape[1], np.uint8)
    for f in range(len(stream)):
        result[f] = age
        age = np.where(same[f], np.minimum(age + 1, limit - 1), 0).astype(np.uint8)
    return result


def tree_costs(model, glyphs, bits):
    classes = model["tree"].shape[0]
    result = np.zeros((classes, glyphs), np.float32)
    node = np.zeros((classes, glyphs), np.int64)
    ids = np.arange(glyphs)
    for shift in range(bits - 1, -1, -1):
        bit = (ids >> shift) & 1
        p0 = model["tree"][np.arange(classes)[:, None], node] / 256
        result -= np.log2(np.where(bit, 1 - p0, p0))
        node = node * 2 + 1 + bit
    return result


def expected_stream_bits(stream, model):
    previous, same, _ = transition_model(stream, stream.max() + 1)
    ages = stream_ages(stream, same, len(model["run_map"]))
    costs = tree_costs(model, stream.max() + 1, int(stream.max()).bit_length())
    total = 0.0
    for f in range(len(stream)):
        group = model["classes"][previous[f]]
        p0 = model["run"][group, model["run_map"][ages[f]]].astype(np.float64) / 256
        change_cost = -np.log2(p0) + costs[group, stream[f]]
        same_cost = -np.log2(1 - p0)
        total += float(np.where(same[f], same_cost, change_cost).sum())
    return total


def mean_squared(levels, stream, patches):
    total = 0.0
    count = 0
    for start in range(0, len(stream), 32):
        stop = min(start + 32, len(stream))
        reconstruction = levels[stream[start:stop]].reshape(-1, 64).astype(np.float32) / 255
        target = patches[start:stop].reshape(-1, 64).astype(np.float32) / 255
        total += float(((reconstruction - target) ** 2).sum())
        count += target.size
    return total / count


def temporal_difference_mse(gray, levels, stream, cols, rows):
    target = np.diff(gray.astype(np.float32), axis=0) / 255
    total = 0.0
    for start in range(0, len(gray) - 1, 32):
        ids = stream[start:start + 33]
        images = levels[ids].reshape(len(ids), rows, cols, 8, 8)
        images = images.transpose(0, 1, 3, 2, 4).reshape(len(ids), rows * 8, cols * 8)
        prediction = np.diff(images.astype(np.float32), axis=0) / 255
        total += float(np.square(prediction - target[start:start + len(prediction)]).sum())
    return total / target.size


def perceptual_scores(gray, levels, stream, net, threads, cols, rows, device_request="cpu", include_dists=False):
    os.environ.setdefault("TORCH_HOME", str(ROOT / "build/training/torch"))
    import torch
    import torch.nn.functional as F
    import lpips
    from piq import DISTS, multi_scale_ssim

    torch.set_num_threads(threads)
    device = select_device(device_request)
    perceptual = lpips.LPIPS(net=net, verbose=False).to(device).eval()
    dists_metric = DISTS(reduction="none").to(device).eval() if include_dists else None
    indices = np.linspace(0, len(gray) - 1, min(96, max(1, len(gray) // 2)), dtype=int)
    lpips_scores, ms_ssim_scores, dists_scores, mse_scores = [], [], [], []
    with torch.no_grad():
        for start in range(0, len(indices), 4):
            selected = indices[start:start + 4]
            target = torch.from_numpy(np.array(gray[selected], copy=True)).to(device).float().unsqueeze(1) / 255
            images = levels[stream[selected]].reshape(len(selected), rows, cols, 8, 8)
            images = images.transpose(0, 1, 3, 2, 4).reshape(len(selected), rows * 8, cols * 8)
            images = torch.from_numpy(np.array(images, copy=True)).to(device).float().unsqueeze(1) / 255
            score = perceptual(images.expand(-1, 3, -1, -1) * 2 - 1,
                               target.expand(-1, 3, -1, -1) * 2 - 1).mean()
            lpips_scores.append(float(score))
            mse_scores.append(float(F.mse_loss(images, target)))
            weights = images.new_tensor([0.0448, 0.2856, 0.3001])
            ms_ssim_scores.append(float(multi_scale_ssim(images, target, data_range=1., scale_weights=weights)))
            if dists_metric is not None:
                dists_scores.extend(dists_metric(images.expand(-1, 3, -1, -1),
                                                target.expand(-1, 3, -1, -1)).cpu().tolist())
    scores = {"lpips": float(np.mean(lpips_scores)), "ms_ssim": float(np.mean(ms_ssim_scores)),
              "mse": float(np.mean(mse_scores)),
              "temporal_mse": temporal_difference_mse(gray, levels, stream, cols, rows)}
    if dists_metric is not None:
        scores["dists"] = float(np.mean(dists_scores))
    return scores


def quantize(lib, patches, levels):
    frames, cells = patches.shape[:2]
    stream = np.empty((frames, cells), np.uint16)
    error = ct.c_uint64()
    lib.quantize_gray(patches.reshape(-1, 64).copy(), frames, cells, levels.reshape(-1, 64).copy(),
                      len(levels), 0, stream, ct.byref(error))
    return stream


def expand_levels(lib, patches, levels, groups, stream, target, seed):
    if target <= len(levels):
        return levels, groups
    errors = np.empty(stream.shape, np.float64)
    for start in range(0, len(stream), 32):
        stop = min(start + 32, len(stream))
        reconstruction = levels[stream[start:stop]].reshape(stop - start, stream.shape[1], 64)
        difference = patches[start:stop].astype(np.float32) - reconstruction.astype(np.float32)
        errors[start:stop] = (difference * difference).sum(axis=-1)
    flat = patches.reshape(-1, 64)
    flat_errors = errors.ravel()
    count = min(16384, len(flat))
    indices = np.argpartition(flat_errors, -count)[-count:]
    samples = flat[indices].copy()
    scale = max(float(flat_errors[indices].max()), 1)
    weights = np.maximum(1, np.rint(flat_errors[indices] * 1000000 / scale)).astype(np.uint32)
    lib.train_gray.argtypes = [U8, U32, ct.c_int, U8, ct.c_int, ct.c_int, ct.c_uint32]
    lib.train_gray.restype = None
    centers = np.zeros((target - len(levels) + 2, 64), np.uint8)
    lib.train_gray(samples, weights, len(samples), centers, len(centers), 24, seed)
    seen = {row.tobytes() for row in levels}
    additions = []
    for row in centers[2:].reshape(-1, 8, 8):
        key = row.tobytes()
        if key not in seen:
            seen.add(key)
            additions.append(row)
    if not additions:
        raise ValueError("residual k-means 没有产生新的 glyph")
    old_pixels = levels.reshape(len(levels), 64).astype(np.int32)
    new_groups = []
    for row in additions:
        distance = ((old_pixels - row.reshape(64).astype(np.int32)) ** 2).sum(axis=1)
        new_groups.append(groups[int(distance.argmin())])
    print(f"expanded glyphs {len(levels)} -> {len(levels) + len(additions)}", flush=True)
    return (np.concatenate([levels, np.array(additions, np.uint8)]),
            np.concatenate([groups, np.array(new_groups, np.uint8)]))


def quantize_with_rate(lib, patches, levels, model, rate_weight):
    f32 = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    lib.quantize_rd.argtypes = [U8, ct.c_int, ct.c_int, U8, ct.c_int, U8, f32, f32, f32,
                               U8, ct.c_int, ct.c_float, U16, ct.POINTER(ct.c_uint64)]
    lib.quantize_rd.restype = None
    frames, cells = patches.shape[:2]
    stream = np.empty((frames, cells), np.uint16)
    error = ct.c_uint64()
    tree_cost = np.ascontiguousarray(tree_costs(model, len(levels), (len(levels) - 1).bit_length()))
    p0 = model["run"].astype(np.float32) / 256
    same_cost = np.ascontiguousarray((-np.log2(1 - p0))[:, model["run_map"]], dtype=np.float32)
    change_cost = np.ascontiguousarray((-np.log2(p0))[:, model["run_map"]], dtype=np.float32)
    lib.quantize_rd(patches.reshape(-1, 64).copy(), frames, cells, levels.reshape(-1, 64).copy(),
                    len(levels), model["classes"], tree_cost, same_cost, change_cost,
                    model["run_map"], len(model["run_map"]), rate_weight, stream, ct.byref(error))
    return stream


def train(gray, patches, levels, stream, model, output, steps, rate_weight, learning_rate, threads,
          seed, lpips_net, device_request, ms_ssim_weight=0, temporal_weight=0, dists_weight=0):
    os.environ.setdefault("TORCH_HOME", str(ROOT / "build/training/torch"))
    import torch
    import torch.nn.functional as F
    import lpips
    if ms_ssim_weight:
        from piq import multi_scale_ssim
    if dists_weight:
        from piq import DISTS

    torch.set_num_threads(threads)
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    device = select_device(device_request)
    previous, same, _ = transition_model(stream, len(levels))
    ages = stream_ages(stream, same, len(model["run_map"]))
    bits = (len(levels) - 1).bit_length()
    tree_cost = tree_costs(model, len(levels), bits)
    classes = model["classes"]
    run = model["run"]
    run_map = model["run_map"]
    k = len(levels)
    validation = np.linspace(0, len(gray) - 1, min(96, max(1, len(gray) // 2)), dtype=int)
    held_out = np.union1d(validation, np.maximum(validation - 1, 0)) if temporal_weight else validation
    training = np.setdiff1d(np.arange(len(gray)), held_out)
    pair_starts = training[np.isin(training + 1, training)]
    if temporal_weight and not len(pair_starts):
        raise ValueError("没有可用于时间一致性训练的连续帧对")
    output.parent.mkdir(parents=True, exist_ok=True)

    class Glyphs(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.blocks = torch.nn.Parameter(torch.from_numpy(levels.astype(np.float32) / 255).reshape(-1, 64))

        def forward(self, images, prev, age, beta, training=False):
            patches = F.unfold(images, 8, stride=8).transpose(1, 2)
            blocks = self.blocks if training else ternary(self.blocks)
            distances = (patches.square().sum(-1, keepdim=True) + blocks.square().sum(-1)
                         - 2 * patches @ blocks.T)
            probabilities = torch.softmax(-distances * beta, dim=-1)
            nearest = distances.argmin(-1, keepdim=True)
            weights = probabilities if training else (torch.arange(k, device=blocks.device).reshape(1, 1, k) == nearest).to(blocks.dtype)
            prediction = F.fold((weights @ blocks).transpose(1, 2), gray.shape[1:], 8, stride=8)
            group = classes[prev]
            bucket = run_map[age]
            p0 = torch.from_numpy(run[group, bucket].astype(np.float32) / 256).to(device)
            candidate = torch.from_numpy(tree_cost[group]).to(device)
            change = -torch.log2(p0).unsqueeze(-1) + candidate
            unchanged = -torch.log2(1 - p0).unsqueeze(-1)
            previous_ids = torch.from_numpy(prev.astype(np.int64).copy()).to(device).unsqueeze(-1)
            mask = torch.arange(k, device=blocks.device).reshape(1, 1, k) == previous_ids
            cost = torch.where(mask, unchanged, change)
            rate = (weights * cost).sum(-1).mean()
            choice = torch.minimum(probabilities, 1 - probabilities).sum(-1).mean()
            return prediction, choice, rate

    def ternary(value):
        return torch.where(value < 64 / 255, 0., torch.where(value < 192 / 255, 128 / 255, 1.))

    codec = Glyphs().to(device)
    perceptual = lpips.LPIPS(net=lpips_net, verbose=False).to(device).eval()
    for parameter in perceptual.parameters():
        parameter.requires_grad_(False)
    dists_metric = DISTS().to(device).eval() if dists_weight else None
    if dists_metric is not None:
        for parameter in dists_metric.parameters():
            parameter.requires_grad_(False)
    optimizer = torch.optim.Adam(codec.parameters(), lr=learning_rate)
    initial_values = codec.blocks.detach().clone()

    def batch(indices):
        return torch.from_numpy(np.array(gray[indices], copy=True)).to(device).float().unsqueeze(1) / 255

    def perceptual_loss(a, b):
        return perceptual(a.expand(-1, 3, -1, -1) * 2 - 1, b.expand(-1, 3, -1, -1) * 2 - 1).mean()

    def structural_loss(a, b):
        weights = a.new_tensor([0.0448, 0.2856, 0.3001])
        return 1 - multi_scale_ssim(a.clamp(0, 1), b.clamp(0, 1), data_range=1., scale_weights=weights)

    def dists_loss(a, b):
        return dists_metric(a.clamp(0, 1).expand(-1, 3, -1, -1),
                            b.clamp(0, 1).expand(-1, 3, -1, -1)).mean()

    def evaluate(indices):
        scores, structural_scores, dists_scores = [], [], []
        mse_scores, temporal_scores, rate_scores = [], [], []
        with torch.no_grad():
            for start in range(0, len(indices), 4):
                selected = indices[start:start + 4]
                target = batch(selected)
                prediction, _, rate = codec(target, previous[selected], ages[selected], 35)
                scores.append(float(perceptual_loss(prediction, target)))
                mse_scores.append(float(F.mse_loss(prediction, target)))
                rate_scores.append(float(rate))
                if ms_ssim_weight:
                    structural_scores.append(float(1 - structural_loss(prediction, target)))
                if dists_weight:
                    dists_scores.append(float(dists_loss(prediction, target)))
                if temporal_weight:
                    keep = selected > 0
                    selected = selected[keep]
                    target = target[keep]
                    prediction = prediction[keep]
                    if not len(selected):
                        continue
                    target_pair = batch(selected - 1)
                    prediction_pair, _, _ = codec(target_pair, previous[selected - 1], ages[selected - 1], 35)
                    difference = ((prediction[1:] - prediction_pair[:-1])
                                  - (target[1:] - target_pair[:-1]))
                    temporal_scores.extend(difference.square().mean(dim=(1, 2, 3)).cpu().tolist())
        quantized = (ternary(codec.blocks).cpu().numpy().reshape(-1, 8, 8) * 255).round().astype(np.uint8)
        delta = float((codec.blocks - initial_values).abs().max())
        changed = int(np.count_nonzero(np.any(quantized != levels, axis=(1, 2))))
        structural_score = float(np.mean(structural_scores)) if structural_scores else 0.0
        dists_score = float(np.mean(dists_scores)) if dists_scores else 0.0
        temporal_score = float(np.mean(temporal_scores)) if temporal_scores else 0.0
        return (float(np.mean(scores)), structural_score, dists_score, float(np.mean(mse_scores)), temporal_score,
                float(np.mean(rate_scores)), quantized, delta, changed)

    best = float("inf")
    chosen = levels.copy()
    started = time.monotonic()
    for step in range(0, steps + 1):
        if step % 20 == 0 or step == steps:
            (lpips_score, structural_score, dists_score, mse_score, temporal_score,
             rate_score, quantized, delta, changed) = evaluate(validation)
            score = (lpips_score + ms_ssim_weight * (1 - structural_score) + dists_weight * dists_score
                     + temporal_weight * temporal_score + rate_weight * rate_score)
            if score < best:
                best = score
                chosen = quantized.copy()
                np.save(output, chosen)
            np.save(output.parent / f"{output.stem}-step{step}.npy", quantized)
            extra = (f" msssim={structural_score:.7f}" if ms_ssim_weight else "")
            extra += (f" dists={dists_score:.7f}" if dists_weight else "")
            extra += (f" temporal={temporal_score:.8f}" if temporal_weight else "")
            print(f"step={step}/{steps} lpips={lpips_score:.7f}{extra} mse={mse_score:.7f} "
                  f"rate={rate_score:.4f} score={score:.7f} delta={delta:.5f} changed={changed} "
                  f"elapsed={time.monotonic() - started:.1f}s", flush=True)
        if step == steps:
            break
        if temporal_weight:
            starts = rng.choice(pair_starts, min(2, len(pair_starts)), replace=False)
            indices = np.stack([starts, starts + 1], axis=1).reshape(-1)
        else:
            indices = rng.choice(training, min(4, len(training)), replace=False)
        target = batch(indices)
        beta = 5 + 30 * min((step + 1) / max(steps - 1, 1), 1)
        prediction, choice, rate = codec(target, previous[indices], ages[indices], beta, training=True)
        reconstruction = F.mse_loss(prediction, target)
        horizontal = F.mse_loss(prediction[:, :, :, 1:] - prediction[:, :, :, :-1],
                                target[:, :, :, 1:] - target[:, :, :, :-1])
        vertical = F.mse_loss(prediction[:, :, 1:, :] - prediction[:, :, :-1, :],
                              target[:, :, 1:, :] - target[:, :, :-1, :])
        perceptual_value = perceptual_loss(prediction, target)
        structural_value = structural_loss(prediction, target) if ms_ssim_weight else prediction.new_zeros(())
        dists_value = dists_loss(prediction, target) if dists_weight else prediction.new_zeros(())
        temporal_value = prediction.new_zeros(())
        if temporal_weight:
            temporal_value = F.mse_loss(prediction[1::2] - prediction[::2], target[1::2] - target[::2])
        objective = (perceptual_value + ms_ssim_weight * structural_value + dists_weight * dists_value
                     + temporal_weight * temporal_value + .01 * reconstruction
                     + .0025 * (horizontal + vertical) + .002 * choice + rate_weight * rate)
        if not torch.isfinite(objective):
            raise ValueError(f"第 {step + 1} 步目标非有限：perceptual={float(perceptual_value.detach())}, "
                             f"ms_ssim={float(structural_value.detach())}, dists={float(dists_value.detach())}, "
                             f"temporal={float(temporal_value.detach())}, mse={float(reconstruction.detach())}, "
                             f"horizontal={float(horizontal.detach())}, vertical={float(vertical.detach())}, "
                             f"choice={float(choice.detach())}, rate={float(rate.detach())}")
        optimizer.zero_grad(set_to_none=True)
        objective.backward()
        if not torch.isfinite(codec.blocks.grad).all():
            raise ValueError(f"第 {step + 1} 步出现非有限梯度")
        codec.blocks.grad[:2].zero_()
        optimizer.step()
        with torch.no_grad():
            codec.blocks.clamp_(0, 1)
            codec.blocks[0].zero_()
            codec.blocks[1].fill_(1)
            if not torch.isfinite(codec.blocks).all():
                raise ValueError(f"第 {step + 1} 步优化器产生非有限 glyph")
        if (step + 1) % 20 == 0:
            print(f"train={step + 1}/{steps} objective={float(objective.detach()):.7f} "
                  f"rate={float(rate.detach()):.4f}", flush=True)
    return chosen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--asset", type=Path, default=ROOT / "224k/apple_data.h")
    parser.add_argument("--stream-cache", type=Path, default=ROOT / "build/compression/final224.npz")
    parser.add_argument("--gray-cache", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/training/codec-glyphs.npy")
    parser.add_argument("--steps", type=int, default=600)
    parser.add_argument("--glyphs", type=int, default=0)
    parser.add_argument("--device", choices=["rocm", "dml", "auto", "cpu"], default="auto")
    parser.add_argument("--rate-weight", type=float, default=.001)
    parser.add_argument("--learning-rate", type=float, default=.002)
    parser.add_argument("--ms-ssim-weight", type=float, default=0)
    parser.add_argument("--temporal-weight", type=float, default=0)
    parser.add_argument("--dists-weight", type=float, default=0)
    parser.add_argument("--rd-weight", type=float, default=0)
    parser.add_argument("--rd-only", action="store_true")
    parser.add_argument("--evaluate-only", action="store_true")
    parser.add_argument("--evaluate-dists", action="store_true")
    parser.add_argument("--context-source", choices=["asset", "requantized"], default="asset")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--lpips-net", choices=["squeeze", "alex", "vgg"], default="squeeze")
    parser.add_argument("--seed", type=int, default=203)
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--finalize", action="store_true")
    args = parser.parse_args()
    if (args.steps < 0 or args.glyphs < 0 or args.rate_weight < 0 or args.rd_weight < 0
            or args.ms_ssim_weight < 0 or args.temporal_weight < 0 or args.dists_weight < 0
            or args.threads < 1 or args.learning_rate <= 0):
        parser.error("steps/glyphs/rate-weight/rd-weight/ms-ssim-weight/temporal-weight/dists-weight 不能为负，"
                     "threads/learning-rate 必须为正")
    if args.rd_only:
        args.finalize = True
    digest = hashlib.sha256(args.video.read_bytes()).hexdigest()
    print(f"source sha256={digest}", flush=True)
    macros, arrays = read_asset(args.asset)
    width, height = macros["APPLE_WIDTH"], macros["APPLE_HEIGHT"]
    cols, rows = macros["APPLE_COLS"], macros["APPLE_ROWS"]
    gray_cache = args.gray_cache or ROOT / f"build/training/gray{width}x{height}.npy"
    gray = load_gray(args.video, gray_cache, args.ffmpeg, width, height)
    patches = patches_from_gray(gray, cols, rows)
    model = asset_model(macros, arrays)
    model["class_count"] = int(model["classes"].max()) + 1
    levels = unpack_levels(arrays["apple_glyphs"])
    groups = model["classes"]
    if args.glyphs and args.glyphs < len(levels):
        parser.error("--glyphs 不能小于当前字库 glyph 数")
    work_context = (contextlib.nullcontext(ROOT / "build/training/native-host") if os.name == "nt"
                    else tempfile.TemporaryDirectory(prefix="badapple-train-codec-"))
    with work_context as work:
        if os.name == "nt":
            work.mkdir(parents=True, exist_ok=True)
        lib = native(work)
        lib.quantize_gray.argtypes = [U8, ct.c_int, ct.c_int, U8, ct.c_int, ct.c_int, U16,
                                     ct.POINTER(ct.c_uint64)]
        lib.quantize_gray.restype = None
        nearest_stream = quantize(lib, patches, levels)
        expanded = args.glyphs > len(levels)
        if expanded:
            levels, groups = expand_levels(lib, patches, levels, groups, nearest_stream, args.glyphs, args.seed)
            nearest_stream = quantize(lib, patches, levels)
        cached = np.load(args.stream_cache) if args.stream_cache.exists() else None
        cache_matches = (not expanded and cached is not None and digest == SOURCE_SHA256
                         and np.array_equal(cached["glyphs"], arrays["apple_glyphs"])
                         and cached["stream"].shape == nearest_stream.shape)
        if cache_matches:
            print(f"stream differences from asset={np.count_nonzero(cached['stream'] != nearest_stream)}", flush=True)
        if args.context_source == "asset" and cache_matches:
            stream = cached["stream"]
            baseline_data = arrays
        else:
            stream = nearest_stream
            previous, same, transitions = transition_model(stream, len(levels))
            runs = run_counts_context(previous, same, len(levels), model["run_map"])
            model, baseline_data = first_order_model(lib, stream, pack_levels(levels), groups,
                                                     transitions, runs, model["run_map"], model.get("prob_bits", 4))
        baseline_payload = baseline_data.get("payload", baseline_data.get("apple_payload"))
        print(f"baseline mse={mean_squared(levels, stream, patches):.8f}", flush=True)
        print(f"baseline expected={expected_stream_bits(stream, model) / 8:.1f} B "
              f"payload={len(baseline_payload)} B data={aligned_size(baseline_data)}", flush=True)
        if args.evaluate_only:
            scores = perceptual_scores(gray, levels, stream, args.lpips_net, args.threads, cols, rows,
                                       args.device, include_dists=args.evaluate_dists)
            print("validation " + " ".join(f"{key}={value:.8f}" for key, value in scores.items()), flush=True)
            return
        chosen = levels if args.rd_only else train(
            gray, patches, levels, stream, model, args.output, args.steps, args.rate_weight,
            args.learning_rate, args.threads, args.seed, args.lpips_net, args.device,
            ms_ssim_weight=args.ms_ssim_weight, temporal_weight=args.temporal_weight,
            dists_weight=args.dists_weight)
        if args.finalize:
            new_stream = (quantize_with_rate(lib, patches, chosen, model, args.rd_weight)
                          if args.rd_weight else quantize(lib, patches, chosen))
            chosen, new_stream, active = compact_levels(chosen, new_stream)
            new_glyphs = pack_levels(chosen)
            previous, same, transitions = transition_model(new_stream, len(chosen))
            run_map = model["run_map"]
            runs = run_counts_context(previous, same, len(chosen), run_map)
            new_model, data = first_order_model(lib, new_stream, new_glyphs, model["classes"][active],
                                                transitions, runs, run_map, model.get("prob_bits", 4))
            verified = decode(lib, new_stream.shape, new_glyphs, new_model, data["payload"])
            if not np.array_equal(verified, new_stream):
                raise ValueError("训练候选的完整 round-trip 不一致")
            header = args.output.with_suffix(".h")
            write_header(header, macros, new_stream, data, new_model,
                         f"codec-aware glyphs rate={args.rate_weight}, steps={args.steps}, "
                         f"ms_ssim={args.ms_ssim_weight}, temporal={args.temporal_weight}, "
                         f"dists={args.dists_weight}, device={args.device}")
            np.savez_compressed(args.output.with_suffix(".npz"), stream=new_stream, glyphs=new_glyphs)
            print(f"candidate mse={mean_squared(chosen, new_stream, patches):.8f} "
                  f"glyphs={len(chosen)} data={aligned_size(data)} header={header}", flush=True)


if __name__ == "__main__":
    main()
