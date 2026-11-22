# BadAppleEPD

CH32V203C8T6 + SSD1680 152×152 黑白墨水屏 Bad Apple 播放 demo，将30fps的原片压缩到64k从而放入CH32V203中，另有画质更好的224k版本。

本项目的非库代码均为AI生成且未经校对。本说明文档由直立猿猴撰写。

压缩方式为cnlohr的Badderapple项目，基本保持原样，少许区别在下一节说明。原理说明建议参考其repo，这里附原压缩方式的简单说明：

视频的每一个画面都是通过数个8x8的glyph拼接而成。于此视频从单个像素用0和1表示变为每一帧由编号为xxx的glyph拼接而成，类似活字印刷。活字印刷这一步带来了最大的压缩。

glyph的数量是有限的，编码视频分两部分，首先要生成一组最适合这个视频的glyph组成一个码表，随后对每一帧都使用这个码表重新拼接出来，因此只要一张表和一点点编号就能表示一帧。

拼接的画面存在方形接缝，可通过类似H.264的去方块滤镜消除，滤镜做的事在于根据周围像素改变中间像素的值，滤过之后接缝即消失。

如何生成合适的glyph是此压缩方式的核心，glyph的生成方式有k-means和通过机器学习生成两种。原视频被分割成6x8，每格8x8像素的形式，由此得出约31万张小图片，在六十四维空间中形成三十一万个点。

随后随机生成一堆glyph的初始点，通过k-means跑几轮，即可形成所需的码表。此方式生成的码表有黑白灰三色（像素在取平均的过程中会变成0.5）。cnlohr的实践表明三色码表的压缩效果比纯黑白要好一些。
这些生成的码表通过0b 1b 11b分别表示黑白灰三色的形式编码为比特流并被间距编码压缩放到flash中，运行时解包到ram里。间距编码压缩（在这一分钟）可以先理解为rar压缩文件，其效果非常接近理论极限。

生成码表后，每一帧的表示方式变成了48个编号，但由于许多块大部分时候保持不变，还可进一步压缩。方法为每帧只记录该帧相对上一帧有哪几个地方的变了，再记录下变了的新的glyph的编码就行。

因此，数据格式就是6x8=48条掩码比特流分别表示48个格子在每一帧是否变化，还有一个按顺序叠起来的glyph编号堆比特流，编码时按顺序遍历每一帧，如果某个格子和上一帧不同就给该位置的掩码记1，并往
glyph编号堆里塞进新的glyph。解码时反过来，从压缩的掩码比特流里一个个拆，拆到1的话就从glyph里也拆一个出来画上去即可。现在问题变为如何压缩这48+1条比特流。

（深吸气）间距编码在压缩时需要输入待压缩的bit和对这个bit的概率预测。解码时也需要输入概率。只要编码器和解码器对概率的预测相同，就可以正确解码。若输入的bit是个小概率事件就需要更多bit去编码
（例如输入bit为1，但给的概率预测说这个bit为1的概率是1%，那么就要消耗很多个bit来编码这一位）（相应地，如果一直输入0.5那编码前后的bit量基本不会变）（再相应地，如果你精确地知道每个位的bit是1还是0，你完全可以
在编码的时候输入这个bit为1，且它为1的概率是100%，那么只需要0个bit就能编码这个位，你编码完所有位发现编码完成的数据只有0kb，可喜可贺，但解码的时候你必须要知道每一个位的概率，结果是你的压缩文件只有0kb，但你
为了解压这个压缩文件带的概率表和原来的文件一样大了）。直觉是把这几条比特流扫一遍统计里1和0出现的概率，再按照这个固定概率喂进范围编码器当中，每次编码都告诉编码器全局概率就行了。这样做的确有效，但如果利用更多的
信息就能做的更好。（喘气）

抛开glyph编码不谈，先看掩码的压缩，我们要压缩的比特流主要是某个格子否变化的掩码（每一帧都有48个bit），只可能是1或者0。在播放的时候除了比起利用全局比特概率，解码器还知道两个信息，分别是“当前显示的glyph是什么”
以及“当前显示的glyph已经多久没变了”。这个这个信息在编码的时候也是可以知道的，因此若编码的时候利用这两个信息做一个全局统计，就可以给出一张概率表。把这张概率表存起来，解码的时候也用上，那就可以正确解码。而且因为
利用了额外的信息，被压缩的信息流也得以进一步地减少了。关键在于如何构建这个概率表。

作者选取的方式是将256个glyph分为13个类（class），然后持续时间的上限（run-length）设置为128，这样概率表就有13*128个入口，每个入口里面存着一个概率，代表“属于这个类的glyph在持续了这个长度的帧之后下一帧发生变化的概率是多少”
例如概率表第2行第42列的值是23，那么代表属于class 2的glyph在持续了42帧之后发生变化的概率是23/256=0.08984375。这个表每个值都可以通过对原视频进行统计得出。有了概率，有了要编码的比特流，那就能做压缩了。至于将glyph分入13个类
的方法，作者写了一个自己的算法，我们这里暂且按下不表。

至此掩码流的压缩已经结束了。再回到glyph的压缩上来。直觉解法同样是将待压缩的glyph用数字编码，然后头尾相接弄成一个比特流，再统计全局概率并塞进范围编码器里压缩，解码的时候一个个弹出来。而如同掩码比特流一样，glyph的解码也应该
利用额外信息。因为从一个glyph变为其他不同的glyph的概率有大有小，同样可以维护一张概率表，复用之前的那13个类，glyph分类也相同，这次的表有13*255个格子，概率表回答的问题是“在当前class里，已知前几位是xxx的情况下，下一位是1的
概率是多少”。因为一个glyph是8个bit表示的，所以在一个类里，需要连续解码8次才能知道这个glyph是什么，且每次解码都要告诉解码器当前bit的概率。作者采用的方法是从最高bit开始解，输入概率为该类下所有glyph的最高位统计为1的概率，
例如类4里所有的glyph的最高位的概率是0.74，那么就将0.74放入类4所在的第四行的第0列。但解码的时候该位可能1也可能是0，因此接下来必须要两个格子，分别对应最高位是1和最高位是0时，第二位是1的概率，以此类推，8位就总共有255个概率。
编码的时候则反过来，统计最高位是1的概率放到第0列，再统计最高位是1时第二位是1的概率放到第1列，最高位是0是第二位是1的概率放到第二列，如此循环，形成一个广度优先的树状结构。这么讲可能很绕，但记住只要编码器和解码器对概率的预测是
相同的，那么就能正确地还原信息，因此只要手里有概率表和知道概率表怎么用，解码器就能无损解压。且由于压缩解压glyph的过程中利用了“当前glyph在哪一类”和“解码出来的glyph编号的头几位长这样”两个信息，整体压缩率比起用全局概率也是得到
了提高的。

## 仓库里的东西

64k版大小64k，可以放进零等待flash。224k需要跨0等待和非0等待flash。对两个 profile 的灰度统一使用固定面板坐标棋盘格。flash的读取速度还是够用的。224k 在等待帧时刻和 BUSY 时使用 WFI，不过功耗降低不算很明显（13mA降低到9mA）。
如果对于功耗有追求的话，可以考虑V407，该单片机几乎能在一瞬间完成解码，加上stop模式休眠，即使在200x200的墨水屏上播放完整分辨率的badapple也只需要2.5mA左右。

相比于原来的压缩方式，本仓库的版本在概率表的构建上做了一些改动，并且使用 cnlohr 自己撰写的范围编码库。224k版本使用了584个glyph，概率表则将这584个glyph分为24个类，每个类的概率使用4个bit。
glyph的持续时间所对应的变化概率表（即为run-lenght对应的概率表）只有24*6个入口，6列将1-16帧非均匀地分为6个区间（3或者4帧发生变化的概率其实差不多，5-8帧也差不多，16帧以上的全都差不多，所以可以稍微合并一下），
每个区间的概率被视为相同的。

64k版本则使用了320个glyph，分为20个类，概率表和224k一样，64k版本固件大小为65,532字节，刚好塞进64k里面。若想如同原作者一样把音频也加入，可以考虑将glyph数量减少到256个左右。

两个的码表都使用了机器学习做优化，同时优化码率和画面效果，结果相比k-means在数据上有略微的提升，不过实际效果不算很明显。这些改动带来的最大提升在于更小的体积做到相似的效果。使用原版方式需要将glyph数量缩减到128左右
才能在已有墨水屏代码的情况下塞进flash里。不过机器学习训练的320个glyph和纯k-means选择的248个glyph最终渲染出来的视觉效果差异并不明显，相对于硬减少glyph数量塞进去的版本倒是有不错的提升。

## 文件

```text
64k/apple_data.h    64k 的 glyph、类别、概率表、完整视频码流
224k/apple_data.h   224k 的对应数据
src/               两个目标共用的 MCU 程序
hal/               WCH的HAL库的一部分
tools/encode.py     视频采样、k-means/ML、字形合并、压缩参数搜索
tools/recompress.py 复用现有字库重建码流，搜索分类、run、概率精度和编号
tools/train_codec.py 面向真实 tree/run 码率的 glyph 训练与率失真量化
tools/render_preview.py 按固件棋盘格规则生成新旧版本对比视频
tools/native.c     主机训练、最近邻/率失真量化、范围编解码的 C 加速
Link.ld            链接脚本，flash容量不同
Makefile
```
HAL来自官方HAL，经过裁剪。间距编码的压缩和解压缩代码原样复制自cnlohr的speedtropy项目

## 接线

| 面板 | MCU |
| --- | --- |
| BUSY | PA0 |
| DC | PA3 |
| CS | PA4 |
| SCK | PA5 |
| RST | PA6 |
| MOSI | PA7 |

## 编译与烧录

需要从mounriver官网下载linux工具链并在环境变量中指定gcc12的工具链目录。

```sh
export WCH_TOOLCHAIN_PATH=/path/to/wchgcc/bin/
make -j4             # 编译两个目标
make 64k            # 单独编译
make 224k
```

烧录：由于使用上游minchlink要烧录一百秒以上且无法unbrick，本项目提供一个改版的minichlink，针对V203单片机可以在4秒左右烧写完全部数据。烧录命令：

```sh
make PROFILE=64k flash
make PROFILE=224k flash 
```

救砖（清空flash）命令：
```sh
make -u
```

由于电工猴未经过机器学习相关培训，接下来的部分为纯AI生成。

## 根据视频生成 glyph 和码流

需要主机 GCC、ffmpeg、Python3 和 numpy。需要在本地下载一个badapple视频。

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install 'numpy<2'
python3 tools/encode.py input.mp4 --profile 64k
make 64k
```

脚本以 30fps 面积采样到目标尺寸，按 8×8 分块训练三色字形，以最小灰度误差选取每帧字形。固定前两个字形为纯黑、纯白。删除未使用字形，再按失真与转移频率合并到存储上限；随后建立类别转移树和 run 上下文，用 speedtropy 范围编码生成完整码流。

64k 默认使用全部灰度块训练，256 个中心、24 轮、随机种子 203，最终最多 248 个 glyph。224k 从边缘块取最多 65,536 个样本，624 个中心，然后使用 badderapple 的 glyph-only ML 训练，最终最多 584 个 glyph。

### 224k 的 ML 训练

额外需要 PyTorch、torchvision、LPIPS；已验证的 CPU 环境为 Python 3.10、PyTorch 2.5.1、torchvision 0.20.1、LPIPS 0.1.4。下面在同一虚拟环境安装 CPU 版本：

```sh
pip install torch==2.5.1 torchvision==0.20.1 --index-url https://download.pytorch.org/whl/cpu
pip install lpips==0.1.4
python3 tools/encode.py input.mp4 --profile 224k --save-glyphs /tmp/glyphs224.npy
make 224k
```

训练使用软分配、连续字形、LPIPS/VGG 和 Adam，`steps=1500`、`batch=4`、`lr=0.002`、`beta=35`、choice 权重 1、MSE 权重 0.01、默认 CPU 8 线程。每 100 步将候选量化为三色，以灰转黑验证 LPIPS 选择候选，保留原实验的选择方式。

`--ml-steps 0` 可跳过 ML，只用 k-means；`--glyph-file /tmp/glyphs224.npy` 可复用合并前的三色码本，省去再次训练。`--save-glyphs` 保存的 NPY 为 uint8、N×8×8，像素为 0/128/255。

### 调整容量和压缩参数

- `--training-glyphs`：训练中心数，影响拟合能力和训练时间。
- `--glyphs`：最终存储字形上限，控制画质和数据大小；短视频去掉未使用项后可能更少。
- `--classes`：按上一字形分组的预测模型数量；
- `--run-lengths`：相同字形连续保持时使用的年龄上下文数。
- `hold=0` 固定，更大数量则可容忍画面误差并保持上一字形。帧率固定 30fps。

class 和 run 可以一次给多个数，例如：

```sh
python3 tools/encode.py input.mp4 --profile 224k \
  --glyph-file /tmp/glyphs224.npy \
  --classes 8 12 15 16 20 24 --run-lengths 8 16 32 64
make 224k
```

会搜索最优值。最终完整镜像容量以 `make` 链接为准。新视频`--output /tmp/apple_data.h` 可先生成候选，避免覆盖附带数据。

### 从现有码流无损重压缩

`tools/recompress.py` 可以直接读取 `apple_data.h`，反解完整 glyph 流并复用字库，不需要原视频或 ML 训练缓存。基线模式要求原参数重编码的 payload 逐字节一致，其他模式也会为最佳候选执行完整 round-trip；候选评分包含所有概率表、映射表、payload 和 4 字节对齐开销。

```sh
python3 tools/recompress.py --mode baseline
python3 tools/recompress.py --mode precision --output build/compression/precision.h
python3 tools/recompress.py --mode runopt --output build/compression/runopt.h
python3 tools/recompress.py --mode renumber --output build/compression/renumber.h
```

可用模式还包括 `class`、`order2` 和 `order2tree`。当前 224k 实测二阶上下文不划算，因此固件仍使用一阶上下文，不增加历史 RAM。当前 6 个 run 桶分别对应持续 1、2、3–4、5–6、7–11、12–16 帧，最后一个桶饱和后也覆盖更长持续时间。旧数据头未声明新宏时仍按 7-bit 概率和逐帧 run age 解码。

### 面向实际编码的 glyph 训练

`tools/train_codec.py` 使用 [Felixoofed/badapple-frames](https://github.com/Felixoofed/badapple-frames) 的 `badapple.mp4`，当前源文件 SHA-256 为 `dc4faefff54e668394f91a383a4259777ab86d751b4a1277b869b55731de1913`。它从现有三色字库 warm start，采用 LPIPS-Squeeze、MSE、水平/垂直边缘、分配尖锐度和真实编码期望码率的联合 loss；β 从 5 退火到 35，每 20 步执行一次三值化和硬最近邻验证。

码率项直接读取当前 4-bit tree/run：保持 glyph 使用 same bit 成本，切换 glyph 使用 change bit 加完整 glyph 路径成本。`--rd-weight` 还可在不训练的情况下，让全片量化在归一化像素误差和真实 bit 成本之间做因果率失真选择。最终候选按实际码流的 LPIPS/MSE、完整数据区和 BIN 容量共同筛选，不以训练 loss 代替压缩结果。

```sh
python3 tools/recompress.py --mode baseline \
  --input 224k/apple_data.h --cache build/compression/current224.npz --rebuild-cache

build/training-venv/bin/python tools/train_codec.py build/input/badapple.mp4 \
  --asset 224k/apple_data.h --stream-cache build/compression/current224.npz \
  --steps 600 --learning-rate 0.003 --rate-weight 0.001 \
  --rd-weight 0.001 --finalize --output build/training/next-glyphs.npy \
  --ffmpeg /path/to/ffmpeg
```

`--evaluate-only` 可对实际码流执行同一套 96 帧感知评分，`--rd-only` 可跳过训练只扫率失真量化权重。当前版本使用学习率 0.003、600 步、训练码率权重 0.001 和最终 RD 权重 0.001。训练环境、视频、灰度帧和 Torch 权重均可放在已忽略的 `build/` 下。

### 64k side-by-side 预览

`64k/side-by-side.mp4` 为 1024×416、30 fps、完整 6572 帧对比视频：左侧为原 248 glyph 版本，右侧为 320 glyph DirectML 训练版，均按固件的固定棋盘格规则渲染。64k 最终 RD 权重为 0.000625，链接后的 `__payload_end = 0xfffc`，全部代码和数据均在前 64 KiB 内。

```sh
python3 tools/render_preview.py \
  --left build/compression/good64-v1.h --right 64k/apple_data.h \
  --output 64k/side-by-side.mp4 --ffmpeg /path/to/ffmpeg
```
