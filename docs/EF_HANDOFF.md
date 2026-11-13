# E/F 基础交付与接入步骤

本分支基于只有 README 的仓库创建，未实现 CPU 光栅器，也没有用假图像冒充真实采样。公共类型是提案，须 A/B/C/D 审查后合并。当前代码为无第三方依赖的 C++17 基础模块和 Python 导出工具。

## 已提供
- CameraController：轨道旋转、距离缩放、按物体 ID 跟随；世界空间射线球体拾取，返回最近正交点对应 ID。
- compare：同一 const Snapshot 顺序调用三种模式并检查图像尺寸。Renderer 必须无外部可变场景依赖。
- benchmark：预热、重复、steady_clock 回调总耗时、中位数和最近秩 P95，强制关闭 trace。计时包含回调内分配，不包括返回后校验，不代表已测得内核各阶段耗时。
- PNG：无损 RGBA8、从上到下；CSV 逐帧数据和 JSON 参数导出。Python 工具需要调用方传入真实数据，目前未与 C++ 自动连接。
- CMake/CTest 和独立测试。测试回调仅验证调度和数据契约，不证明渲染算法正确。

## 推荐集成顺序与接口责任
1. A/B/C/D 审查 include/cg/ef.hpp：右手系/Y 向上建议、角度弧度、单位、快照几何/材质所有权仍需确定。现在 Snapshot 只含时间、相机和拾取球体，不能作为完整生产渲染快照。
2. A 接入场景、时间暂停/倍速和身体 ID/中心/半径；相机 follow 在场景更新后调用。时间控件尚未实现。
3. B/C 提供不可变几何材质与 render 回调；Frame 的 rgba 是显示 sRGB RGBA8，线性 resolve 由 D 在转换前完成。
4. D 接入 Single/SSAA4/MSAA4、真实统计和 PixelTrace。禁止从显示颜色反推物体 ID；目前未定义 PixelTrace，避免替 D 决定逐样本记录结构。
5. E 在目标 Mac 接入 GLFW/Metal/ImGui，先显示测试纹理再上传 Frame。固定内部大小；展示缩放用最近邻；点击需处理画面偏移、缩放与 Retina 比例，再生成世界射线。当前未包含窗口后端和屏幕射线生成。
6. 实际渲染可用后接入三个真实预设：全景、土星环边缘、远处行星。快照保存完整相机、时刻、几何材质配置及资源版本，再实现回放；当前不能声称可完整回放。
7. F 将真实逐帧结果交给 tools/export.py。机器、OS、编译器、优化选项、线程数、提交版本、预热/重复数及全部快照参数写入 metadata。正式实验用 Release，分开报告内核、上传和界面耗时。

## 构建与检查
在项目根目录运行（需 CMake 和 C++17 编译器）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python -m unittest discover -s tests -p "test_*.py" -v
```

本机若无 CMake，可直接编译基础测试：

```sh
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude tests/ef_tests.cpp -o ef_tests
./ef_tests
```

macOS GUI 验收需有 Xcode Command Line Tools、GLFW 和 Dear ImGui 的确认版本，再接入 Objective-C++ Metal 后端。当前项目没有这些依赖，不能宣称窗口或发布包完成。

## 是否上传 GitHub
建议上传本功能分支给其他成员审查，避免大家同时创建不同公共类型；不要直接覆盖 main。当前没有提交或推送。确认内容后在根目录运行：

```sh
git add .gitignore CMakeLists.txt include tests tools docs
git commit -m "Add E/F foundation and integration checklist"
git push -u origin feature/ef-foundation
```

在 GitHub 创建 Pull Request，先请 A/B/C/D 看数据结构和 Renderer 接口。通过后再合并；这不是完整 E/F 交付，窗口、真实渲染对接、PixelTrace、回放及目标机发布仍待完成。

## 本次实际验证结果
- Python 3.13：3 项导出测试全部通过（PNG 解压内容、CRC、尺寸拒绝、CSV/JSON）。
- C++：尝试调用本机 MinGW g++，启动时报无效的系统 DLL 重定位，未生成程序；不能声称编译或测试通过。
- CMake/CTest：本机未安装 CMake，未运行。
- Metal/ImGui GUI：未实现、未验证；需目标 macOS 环境及依赖接入。
