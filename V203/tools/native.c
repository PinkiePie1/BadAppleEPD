// 主机三色 k-means、量化和 speedtropy 编码加速。
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define SPEEDTROPY_WRITE
#define SPEEDTROPY_READ
#define SPEEDTROPY_IMPLEMENTATION
#include "../src/speedtropy.h"

static uint32_t rng;
// 生成固定种子的 xorshift 随机数。
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

// 在原始灰度上量化，字形中心限制为 0、128、255。
// 计算两个 8×8 灰度块的平方距离。
static unsigned gray_distance(const uint8_t *a, const uint8_t *b)
{
    unsigned result = 0;
    for (unsigned p = 0; p < 64; ++p) {
        int difference = (int)a[p] - (int)b[p];
        result += difference * difference;
    }
    return result;
}

// 将均值量化为黑、灰、白三色。
static uint8_t gray_level(unsigned mean)
{
    return mean < 64 ? 0 : mean < 192 ? 128 : 255;
}

// 训练三色字形，固定纯黑和纯白中心。
void train_gray(const uint8_t *samples, const uint32_t *weights, int n,
                uint8_t *centers, int k, int iterations, uint32_t seed)
{
    unsigned *distance = malloc((size_t)n * sizeof(*distance));
    uint64_t *sums = calloc((size_t)k * 64, sizeof(*sums));
    uint64_t *counts = calloc((size_t)k, sizeof(*counts));
    rng = seed ? seed : 1;
    memset(centers, 0, 64);
    memset(centers + 64, 255, 64);
    for (int i = 0; i < n; ++i) distance[i] = UINT_MAX;
    for (int c = 1; c < k; ++c) {
        uint64_t total = 0;
        for (int i = 0; i < n; ++i) {
            const uint8_t *sample = samples + (size_t)i * 64;
            unsigned d = gray_distance(sample, centers + (size_t)(c - 1) * 64);
            if (d < distance[i]) distance[i] = d;
            if (c == 1) {
                d = gray_distance(sample, centers + 64);
                if (d < distance[i]) distance[i] = d;
            }
            total += (uint64_t)distance[i] * weights[i];
        }
        if (c + 1 >= k) break;
        uint64_t target = total ? (((uint64_t)random32() << 32) | random32()) % total : 0;
        int pick = 0;
        for (; pick < n - 1; ++pick) {
            uint64_t mass = (uint64_t)distance[pick] * weights[pick];
            if (target < mass) break;
            target -= mass;
        }
        for (int p = 0; p < 64; ++p)
            centers[(c + 1) * 64 + p] = gray_level(samples[(size_t)pick * 64 + p]);
    }
    for (int it = 0; it < iterations; ++it) {
        memset(sums, 0, (size_t)k * 64 * sizeof(*sums));
        memset(counts, 0, (size_t)k * sizeof(*counts));
        for (int i = 0; i < n; ++i) {
            const uint8_t *sample = samples + (size_t)i * 64;
            unsigned best = UINT_MAX;
            int winner = 0;
            for (int c = 0; c < k; ++c) {
                unsigned d = gray_distance(sample, centers + (size_t)c * 64);
                if (d < best) { best = d; winner = c; }
            }
            counts[winner] += weights[i];
            for (int p = 0; p < 64; ++p)
                sums[winner * 64 + p] += (uint64_t)sample[p] * weights[i];
        }
        for (int c = 2; c < k; ++c) {
            if (!counts[c]) continue;
            for (int p = 0; p < 64; ++p)
                centers[c * 64 + p] = gray_level((unsigned)((sums[c * 64 + p] + counts[c] / 2) / counts[c]));
        }
    }
    free(distance); free(sums); free(counts);
}

// 按灰度距离为全部视频块选择字形，输出逐帧索引。
void quantize_gray(const uint8_t *samples, int frames, int cells, const uint8_t *centers,
                   int k, int hold, uint16_t *stream, uint64_t *error)
{
    uint16_t previous[cells];
    memset(previous, 0, sizeof(previous));
    *error = 0;
    for (int f = 0; f < frames; ++f) {
        for (int cell = 0; cell < cells; ++cell) {
            const uint8_t *sample = samples + ((size_t)f * cells + cell) * 64;
            int winner = previous[cell];
            unsigned old = gray_distance(sample, centers + (size_t)winner * 64);
            unsigned best = old;
            for (int c = 0; c < k; ++c) {
                unsigned d = gray_distance(sample, centers + (size_t)c * 64);
                if (d < best) { best = d; winner = c; }
            }
            /* hold uses a squared-error budget equivalent to full pixels. */
            if (old <= best + (uint64_t)hold * 255 * 255) {
                winner = previous[cell]; best = old;
            }
            previous[cell] = winner;
            stream[f * cells + cell] = winner;
            *error += best;
        }
    }
}

void quantize_rd(const uint8_t *samples, int frames, int cells, const uint8_t *centers,
                 int k, const uint8_t *classes, const float *tree_cost,
                 const float *same_cost, const float *change_cost,
                 const uint8_t *run_map, int run_limit, float rate_weight,
                 uint16_t *stream, uint64_t *error)
{
    uint16_t previous[cells];
    uint8_t age[cells];
    memset(previous, 0, sizeof(previous));
    memset(age, 0, sizeof(age));
    *error = 0;
    for (int f = 0; f < frames; ++f) {
        for (int cell = 0; cell < cells; ++cell) {
            const uint8_t *sample = samples + ((size_t)f * cells + cell) * 64;
            unsigned old = previous[cell];
            unsigned group = classes[old], bucket = run_map[age[cell]];
            unsigned best_distance = gray_distance(sample, centers + (size_t)old * 64);
            double best_score = best_distance / (64.0 * 255 * 255)
                + rate_weight * same_cost[group * run_limit + bucket];
            int winner = old;
            for (int c = 0; c < k; ++c) {
                if (c == old) continue;
                unsigned distance = gray_distance(sample, centers + (size_t)c * 64);
                float bits = change_cost[group * run_limit + bucket] + tree_cost[group * k + c];
                double score = distance / (64.0 * 255 * 255) + rate_weight * bits;
                if (score < best_score) {
                    best_score = score;
                    best_distance = distance;
                    winner = c;
                }
            }
            if (winner == old) {
                if (age[cell] < run_limit - 1) ++age[cell];
            } else {
                age[cell] = 0;
            }
            previous[cell] = winner;
            stream[f * cells + cell] = winner;
            *error += best_distance;
        }
    }
}

// 按字形类别和 run 上下文对完整视频执行 speedtropy 范围编码。
int encode_tiles(const uint16_t *stream, unsigned frames, unsigned cells,
                 unsigned glyphs, unsigned bits, const uint8_t *classes,
                 const uint8_t *tree, unsigned nodes, const uint8_t *run,
                 unsigned limit, uint8_t *output, unsigned capacity)
{
    if (!cells || !limit || limit > 256) return -2;
    uint16_t previous[cells];
    uint8_t age[cells];
    memset(previous, 0, sizeof(previous));
    memset(age, 0, sizeof(age));
    speedtropy_writer writer;
    SpeedtropyWriterInit(&writer, output, capacity);
    for (unsigned f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < cells; ++c) {
            unsigned value = stream[f * cells + c];
            if (value >= glyphs) return -2;
            unsigned group = classes[previous[c]], same = value == previous[c];
            SpeedtropyWriterEmit(&writer, same, run[group * limit + age[c]]);
            if (same) {
                if (age[c] < limit - 1) ++age[c];
            } else {
                unsigned node = 0;
                for (unsigned b = bits; b-- > 0;) {
                    if (node >= nodes) return -2;
                    unsigned bit = (value >> b) & 1;
                    SpeedtropyWriterEmit(&writer, bit, tree[group * nodes + node]);
                    node = node * 2 + 1 + bit;
                }
                age[c] = 0;
                previous[c] = value;
            }
        }
    }
    return SpeedtropyWriterFinish(&writer);
}

static unsigned tile_group(const uint8_t *classes, unsigned class_count,
                           const uint8_t *pairs, unsigned order,
                           unsigned older, unsigned current)
{
    unsigned group = classes[current];
    if (order >= 2) group = pairs[group + class_count * older];
    return group;
}

int encode_tiles2(const uint16_t *stream, unsigned frames, unsigned cells,
                  unsigned glyphs, unsigned bits, const uint8_t *classes,
                  unsigned class_count, const uint8_t *pairs, unsigned order,
                  const uint8_t *tree, unsigned nodes, const uint8_t *run,
                  unsigned run_buckets, const uint8_t *run_map,
                  unsigned run_limit, uint8_t *output, unsigned capacity)
{
    if (!cells || !class_count || !run_buckets || !run_limit || run_limit > 256 || order > 3) return -2;
    uint16_t previous[cells];
    uint8_t older[cells], age[cells];
    memset(previous, 0, sizeof(previous));
    memset(age, 0, sizeof(age));
    for (unsigned c = 0; c < cells; ++c) older[c] = classes[0];
    speedtropy_writer writer;
    SpeedtropyWriterInit(&writer, output, capacity);
    for (unsigned f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < cells; ++c) {
            unsigned value = stream[f * cells + c];
            if (value >= glyphs) return -2;
            unsigned group = tile_group(classes, class_count, pairs, order, older[c], previous[c]);
            unsigned same = value == previous[c];
            SpeedtropyWriterEmit(&writer, same, run[(order == 2 ? group : classes[previous[c]]) * run_buckets + run_map[age[c]]]);
            if (same) {
                if (age[c] < run_limit - 1) ++age[c];
            } else {
                unsigned node = 0;
                for (unsigned b = bits; b-- > 0;) {
                    if (node >= nodes) return -2;
                    unsigned bit = (value >> b) & 1;
                    SpeedtropyWriterEmit(&writer, bit, tree[group * nodes + node]);
                    node = node * 2 + 1 + bit;
                }
                older[c] = classes[previous[c]];
                previous[c] = value;
                age[c] = 0;
            }
        }
    }
    return SpeedtropyWriterFinish(&writer);
}

int decode_tiles2(uint16_t *stream, unsigned frames, unsigned cells,
                  unsigned glyphs, unsigned bits, const uint8_t *classes,
                  unsigned class_count, const uint8_t *pairs, unsigned order,
                  const uint8_t *tree, unsigned nodes, const uint8_t *run,
                  unsigned run_buckets, const uint8_t *run_map,
                  unsigned run_limit, const uint8_t *payload, unsigned payload_length)
{
    if (!cells || !class_count || !run_buckets || !run_limit || run_limit > 256 || order > 3) return -2;
    uint16_t previous[cells];
    uint8_t older[cells], age[cells];
    memset(previous, 0, sizeof(previous));
    memset(age, 0, sizeof(age));
    for (unsigned c = 0; c < cells; ++c) older[c] = classes[0];
    speedtropy_reader reader;
    SpeedtropyReaderInit(&reader, payload, payload_length);
    for (unsigned f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < cells; ++c) {
            unsigned group = tile_group(classes, class_count, pairs, order, older[c], previous[c]);
            int same = SpeedtropyReaderRead(&reader, run[(order == 2 ? group : classes[previous[c]]) * run_buckets + run_map[age[c]]]);
            if (same < 0) return -1;
            if (same) {
                if (age[c] < run_limit - 1) ++age[c];
                stream[f * cells + c] = previous[c];
            } else {
                unsigned node = 0, value = 0;
                for (unsigned b = 0; b < bits; ++b) {
                    if (node >= nodes) return -2;
                    int bit = SpeedtropyReaderRead(&reader, tree[group * nodes + node]);
                    if (bit < 0) return -1;
                    value = (value << 1) | bit;
                    node = node * 2 + 1 + bit;
                }
                if (value >= glyphs) return -3;
                older[c] = classes[previous[c]];
                previous[c] = value;
                stream[f * cells + c] = value;
                age[c] = 0;
            }
        }
    }
    return 0;
}
