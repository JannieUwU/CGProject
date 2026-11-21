# 基于软件光栅化的交互式太阳系可视化与抗锯齿分析

本项目使用 C++ 实现 CPU 软件光栅器，以太阳系、行星近景和环带为场景，比较单采样、SSAA 与 MSAA，并通过真实像素记录解释覆盖、遮挡、着色和颜色合成。Metal 或 OpenGL 负责显示 CPU 生成的图像和桌面界面。

## 当前功能与阅读入口

当前实现包含三项反馈及后续修复：

1. **真实像素过程记录。** 在 `Pixel → Details → Processing history` 中查看实际覆盖、深度测试、着色位置、透明混合和写入。各级手动展开，收起父层或更换目标时重置下级。
2. **共享边归属。** 所有三角形采样方式共用 top-left 规则和 16 位子像素几何，配合一致近裁剪及分级精确整数计算。
3. **MSAA 几何选点。** 三角形共享颜色优先在合法中心计算，必要时使用完整几何覆盖平均点或固定后备；逐样本深度、拾取 UV 和混合底色保持独立。

已修复重复点击当前近景星球时的多余重入，保留像素选择、Inspect/Compare、展开状态和有效测速。后续减少了无用属性准备与云层重复深度计算。土星仍有细密环影与硬阴影边界的局部跳变，两项阴影过滤暂不实施。

| 想了解什么 | 阅读入口 |
| --- | --- |
| 像素检查各项是什么意思、怎样逐层查看 | [第一项改动说明与查看指南](docs/第一项反馈-改动说明与查看指南.md) |
| 共享边为什么需要统一归属、怎样演示 | [第二项改动说明与查看指南](docs/第二项反馈-改动说明与查看指南.md) |
| MSAA 菱形位置和规则标签怎样理解 | [第三项改动说明与查看指南](docs/第三项反馈-改动说明与查看指南.md) |
| 记录字段、回放、截断与测速口径 | [像素过程技术说明](docs/PIXEL_PROCESS_TRACE.md) |
| top-left、16 位精度、裁剪和数值边界 | [共享边规则与验证](docs/第二项反馈-共享边规则与验证.md) |
| MSAA 选点、性能结果与土星残余问题 | [MSAA 实现与验证](docs/第三项反馈-实现与验证.md) |

这六份说明与本 README 构成当前交付说明。关键实验结论已收入技术说明；开发阶段完整数据与跨版本审计材料另行保留，不作为这些使用步骤的依赖。项目原有的 [素材来源和授权](docs/ASSET_SOURCES.md)、[行星渲染说明](docs/PLANET_RENDERING.md) 继续适用。

## 构建与运行

所有命令从项目根目录执行，路径中的 `build` 是本说明统一采用的桌面构建目录。需要 CMake 3.21 或更高版本、C 编译器、支持 C++17 的编译器及平台图形开发环境。首次配置会获取 nlohmann/json、REBOUND 5.0.1、Dear ImGui；没有已安装 GLFW 时还会获取 GLFW，因此需要可访问依赖下载地址。

macOS 使用 Xcode 命令行工具和系统 Metal；Linux/Windows 桌面端使用 OpenGL 及相应窗口系统开发环境。下面的 `./build/...` 启动方式用于 macOS/Linux 的单配置构建。

### 桌面构建与验证

```sh
cmake -S . -B build -DPLANET_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 6
ctest --test-dir build -C Release --output-on-failure
```

桌面配置注册 9 项测试，包括核心渲染、像素记录、共享边、MSAA 选点、行星外观、批量导出和桌面状态/渲染/文档测试。构建目录与源码位置绑定；更换机器或源码目录后重新配置即可。

构建完成后启动桌面端：

```sh
./build/planet_studio
```

使用 Windows Visual Studio 等多配置生成器时，构建与测试命令仍带 `--config Release`、`-C Release`；程序通常位于 `build/Release/planet_studio.exe`。以下其他可执行程序也需按生成器采用对应的子目录和 `.exe` 后缀。Windows GUI 待进行原生环境验证。

### 仅构建命令行版本

不需要桌面界面时使用独立目录，避免改变已有桌面配置：

```sh
cmake -S . -B build-headless -DPLANET_BUILD_DESKTOP=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless --config Release --parallel 6
ctest --test-dir build-headless -C Release --output-on-failure
```

此配置注册 6 项非桌面测试。下文导出命令也可将可执行程序所在的 `build` 改为 `build-headless`。已有依赖源码时可通过 CMake 的本地 FetchContent 源码配置复用；REBOUND 另支持 `REBOUND_SOURCE_DIR`，均须指向真实存在且版本兼容的源码目录。本说明不依赖开发者个人机器的路径或预览应用包。

## 命令行导出与离线采样对比

桌面构建同时生成命令行程序 `planet_sandbox` 和 `planet_aa_benchmark`。先查看参数：

```sh
./build/planet_sandbox --help
./build/planet_aa_benchmark --help
```

导出一张地球 MSAA 图像及场景状态，使用小分辨率便于快速检查：

```sh
./build/planet_sandbox --body earth --sampling msaa4 --width 320 --height 240 --output build/example-earth-msaa4.ppm --state build/example-earth-state.json
```

该命令生成 PPM 图像与 JSON 状态；再次使用相同文件名会覆盖示例输出。`--sampling` 接受 `single`、`ssaa4`、`msaa4`；兼容参数 `--samples 1|4` 对应单采样或 SSAA4，一次调用只选择其中一种参数写法。`--config` 可读取已存在的项目 JSON，`--time` 指定模拟时刻，`--body` 指定场景中存在的行星。

批量导出固定的地球、土星和轨道场景：

```sh
./build/planet_aa_benchmark --output-dir build/aa-example-001 --width 320 --height 240
```

每次导出使用一个新目录；目录已存在时，程序会提示更换名称。每个场景生成 Single、SSAA4、MSAA4、SSAA16 PNG 与边缘掩码，并导出 `metrics.csv` 和 `project.json`。这些结果由命令自动生成。SSAA16 提供离线的更密采样近似参考；桌面菜单提供 Single、SSAA4 和 MSAA4。

批量导出的误差按显示图像反解 gamma 2.2 后的线性 RGB 计算；每种方法预热一次，记录随后一次 CPU 渲染耗时，不含 PNG 写入。该命令用于固定场景的离线导出和初步比较。技术说明中的历史多轮交替实验采用各表注明的计时方法，桌面 Compare 则用于交互查看当前画面。

## 桌面查看与交互

### 行星设计与模拟

默认项目为太阳和八大行星。点击画面中的行星、全景名称或目录项可进入近景；从一颗行星切到另一颗时经过全景过渡。`Back to system` 返回全景。已经处于同一星球近景时，再点其名称会保留当前状态。近景右键拖动旋转视角，滚轮缩放；镜头过渡期间暂停画面选点和雕刻。

右侧 `Design` 提供各行星已实现的外观参数和可折叠的 `Sculpt brush`。局部雕刻仅在 Design 页且关闭 Inspect/Compare 时生效。`Show baseline` 临时预览默认外观，不撤销编辑；`Reset this body` 会重置该星球设计。地球、火星使用高度图，其他天体包含艺术化形状与纹理设计，适合教学可视化和外观设计。

`Orbit / Physics` 编辑物理参数和初始条件。`Run simulation` 在镜头回到全景后推进 REBOUND 模拟；Simulation 面板可暂停、选择时刻、查看参考轨道和碰撞事件。碰撞演示使用临时地球/火星副本，退出演示恢复原 DIY 项目；演示期间 Save 保存原项目。

参数编辑、雕刻和重置共享 Undo/Redo，支持 Ctrl/Cmd+Z、Ctrl/Cmd+Shift+Z 或 Ctrl+Y；输入框保留文字编辑的撤销行为。撤销跨星球修改时会选中受影响的行星。记录有数量与内存上限，关闭程序后不保留撤销栈。

`Save` 正式更新项目 JSON，`Load` 加载已有项目；有未保存修改时会提示。修改后可生成项目旁的恢复草稿，下次启动可选择恢复或丢弃；恢复后仍需 Save 才更新正式文件。未指定路径时优先使用 `my-solar-system.json`，不存在则新建默认项目。

### 像素检查与比较

顶部采样菜单提供 `1x`、`4x SSAA`、`4x MSAA`。开启 `Inspect` 后点击画面，右侧进入 Pixel，但 Details 仍需手动展开。直接进入 Pixel 页也会启用取样；手动关闭 Inspect 后会保持关闭并提示重新开启，不会因为停留在 Pixel 页而雕刻地形。

`Compare` 在同一快照上并排比较左右采样方法并暂停播放。点击任一侧，左右放大图同步局部坐标；详细记录与最终 Samples 来自实际点击的一侧。Region 的 9×9、17×17、25×25 表示放大邻域大小；像素内的样本数量由采样方法决定。选中画面后可用方向键微调像素位置。

`Details → Processing history` 按实际顺序展示当前帧选中像素的处理。关闭某一级后，其下级恢复收起；换像素、星球或采样方式时重置。重复点击当前近景星球保留检查内容。

### 测速与后台预览

`Frame cost` 显示 Method、Shades、Time，统计整个视图。画面稳定约 0.25 秒后自动开始：每种方法预热 3 次、正式测量 11 次，取 CPU 渲染耗时中位数；左右交替先后顺序。正式测量关闭详细过程记录，结束后用不计时渲染恢复记录。移动像素或改变放大范围保留有效计时，修改实际场景或采样设置则重新测量。

普通视图交互时临时采用宽高减半的单采样预览，停下后恢复所选质量；Compare、Inspect 和 Pixel 分析保持固定分析分辨率。CPU 渲染在后台执行，图像尚未跟上视角时暂缓画面选点，防止用旧图误操作。当前性能表展示整体 CPU 渲染时间和着色次数。

## 实现与验证边界

球体和环带三角形由 CPU 处理近裁剪、投影、覆盖、透视正确插值、纹理与光照；轨道使用线段处理，大气使用独立射线。云层与星环保留各自透明混合规则。透明层的合成结果与各层处理顺序有关。

单采样在中心着色，SSAA 在各合格样本着色，MSAA 为每个三角形片段/像素共享一次完整着色；一个像素可能包含多个三角形和效果。细纹理与阴影仍可能出现局部跳变。颜色在线性空间合成，最后转换为显示颜色。完整过程只跟踪当前选中的一个像素，最多 4096 条事件。

本轮文档整理在 macOS 上重新完成 Release 构建，桌面配置 9/9、无界面配置 6/6 自动测试通过；命令行帮助、单图与状态导出、批量对比导出均实际运行通过。构建复用了本机已有依赖源码，桌面启动程序已构建并核对路径。本轮验证覆盖构建、自动测试和命令行导出入口；首次联网下载与人工界面复测待补充。

技术说明保留开发阶段的专项覆盖/光栅器内存与数值检查、界面操作、固定场景图像和多轮计时结果。Windows/Linux 原生 GUI 待验证。

REBOUND 使用 GPL-3.0；依赖和素材授权需随项目一并遵守。项目采用教学比例和简化场景，用于图形学展示与分析。Mipmap、时间滤波及进一步的阴影过滤列为后续研究方向。
