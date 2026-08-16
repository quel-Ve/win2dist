# win2blur v3.0: 逐窗口 profile + 焦点差异化 + 鼠标跟随模糊圆 — 设计文档

- **日期**: 2026-08-07
- **版本**: v3.0（目标，基于 v2.8 beta）
- **状态**: 已批准（用户头脑风暴确认：全 overlay 路线；后台自动降档；圆外保持窗口现状；发散项收编）

## 目标

1. **逐窗口 profile** — AutoFrost 应用列表升级为每应用完整参数（效果模式 + 透明度 + 模糊半径 + tint + 鼠标圆开关），应用启动自动套用
2. **焦点差异化** — 焦点窗口保持 profile 全效果，后台窗口自动降档（radius×0.6 / tint×0.5 / wash×0.6），焦点切换 300ms 平滑过渡
3. **鼠标跟随模糊圆** — 鼠标所在窗口内，50px 圆内模糊半径更高（默认×2），圆边界连续曲线过渡，圆外保持窗口现状；圆半径/倍率/过渡带宽可在托盘 Settings 调；闲置 1.5s 圆渐隐
4. 性能自适应 → **v3.1**（与液晶体玻璃一起）

## 非目标（YAGNI / 明确排除）

- ❌ 液晶体玻璃 / Mica 动态取色 / 暗色感知 tint → v3.1
- ❌ 聚光模式（圆外压暗）— 头脑风暴未入选
- ❌ 预设画廊 UI（Settings profile 下拉）— 未入选；profile 复制 config 文件即可
- ❌ 性能自适应降级（GPU/电池感知）→ v3.1
- ❌ DWM 注入逐窗口半径（`CVisual::GetHwnd` 反查，+0xE30F0，未验证高风险）— 全 overlay 路线不做；Route F 全局半径滑块保持现状
- ❌ 事件驱动监视（AutoFrost 沿用 v2.7 轮询）

## 架构

```
tray_app.exe（指挥层，改动）
 ├─ config.ini [Apps] profile 表 → 启动/新窗口时调度 (复用 autofrost 监控)
 ├─ 守护: overlay 进程崩溃自动重启 (新增)
 └─ Settings 对话框: profile 编辑器 + 鼠标圆滑块 + 焦点倍率

crisp_overlay.exe（v3.0 统一模糊引擎，每窗口一个实例，大改）
 ├─ 参数: <hwnd> <wash> <radius> <tint> <circle_on> [焦点降档由 overlay 自行跟踪]
 ├─ 缓存帧: 双隐藏捕获一次 → 缓存; 参数变化只重模糊缓存帧 → 零闪烁
 ├─ 焦点 WinEvent hook (EVENT_SYSTEM_FOREGROUND) + 300ms ease 参数渐变
 └─ 鼠标圆: 双半径 box blur + 径向 smoothstep mask 合成

acrylic_overlay.exe（不变）— Acrylic 模式窗口，焦点降档仅 tint
Route F DWM 注入（不变）— 全局模糊半径滑块照旧
```

### 核心机制：缓存帧 + 重参数

- 双隐藏捕获（v2.8 已有）→ 缓存 W×H BGRA 帧
- **鼠标移动 / 焦点切换 / 滑块拖动 = 只重跑模糊 + 合成，不重新捕获** → 零闪烁
- 仅窗口移动/缩放 → 重捕获（既有 1 帧闪烁行为）
- 模糊在 1/4 降采样上进行，放大回原尺寸（CPU 优化）

## 逐窗口 Profile（config.ini）

```ini
[Apps]
Notepad.exe = mode=acrylic alpha=80 radius=12 tint=0  circle=off
Cherry Studio.exe = mode=crisp  alpha=65 radius=40 tint=20 circle=on
Taskmgr.exe = mode=crisp  alpha=75 radius=25 tint=10 circle=on
```

- 与 v2.8 AutoFrost 表合并；旧条目（无 radius/tint/mode 字段）按默认值兼容
- 应用启动自动套用（autofrost 轮询沿用）；手动调整 = 会话内覆盖（v2.8 session 机制），重启回 profile
- Settings：列表选中项展开参数编辑（mode / alpha / radius / tint / circle）

## 焦点差异化

| 模式 | 焦点窗口 | 后台窗口（全局倍率，默认 ×0.6，可关） |
|------|----------|---------------------------------------|
| crisp | profile 全参数 | radius×0.6, tint×0.5, wash×0.6 |
| acrylic | profile tint | 仅 tint×0.5（DWM 半径固定，诚实限制） |

- overlay 内跟踪 `GetForegroundWindow() == g_target`；切换 → 目标参数 + 当前参数逐帧 lerp，**300ms ease**
- 无 overlay 的 Acrylic 模式窗口：tray 侧前景事件 → `SetWindowCompositionAttribute` 改 GradientColor（即时，零闪烁）

## 鼠标圆（crisp overlay 内）

- 参数：半径（默认 50px，Settings 滑块 0=关）、圆内半径倍率（默认 ×2）、过渡带宽（默认 30px）
- 算法：缓存帧 → base 模糊 r1 → 圆内模糊 r2 → 径向 smoothstep mask 混合（连续曲线过渡）
- 激活条件：鼠标在窗口内；**闲置 1.5s 圆 300ms 渐隐**，鼠标恢复
- 未开毛玻璃窗口：圆外 wash=0 → 圆内局部模糊（与"圆外保持窗口现状"一致）

## 错误处理 / 性能 / 测试

- 每窗口 1 帧缓存（W×H×4）+ 1/4 降采样模糊；多窗口 CPU ≈ 单窗口×N，N 上限由用户控制（profile 覆盖窗口数）
- overlay 独立进程：崩溃隔离 + tray 守护重启（守护需防崩溃循环：30s 内重启 >3 次则停并托盘气泡提示）
- 测试：参数组合冒烟脚本（launch overlay 各参数档截图对比）；焦点切换/鼠标圆/闲置渐隐手动清单；多窗口压力（5+ overlay）
- 不新增依赖，Win10 目标不变

## 部署

- 遵循 [[win2blur-dist-autoupdate]]：构建后复制 `build5/win2blur.exe → win2blur/dist/` + `dist_release_3.0/`（发布包含 crisp_overlay.exe 独立 exe）
- 版本 tag 跟随 commit
