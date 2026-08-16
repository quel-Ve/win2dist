# Auto-frost: 常用软件自动应用默认效果 — 设计文档

- **日期**: 2026-08-05
- **版本**: v2.7（目标）
- **状态**: 已批准（用户确认轮询 30s、每窗口只自动一次、Settings 内嵌 UI）

## 目标

选定的常用软件一旦打开（或已在运行时），自动应用默认效果：85% 透明度 + Acrylic 模糊。用户手动调整永不被抢回。

## 非目标（YAGNI）

- 逐窗口模糊半径（roadmap 未实现项，自动应用沿用当前全局半径）
- 事件驱动监视（SetWinEventHook）— 轮询 30s 足够
- 匹配窗口标题、命令行参数
- 默认效果随时间动态变化

## 架构

```
后台监视线程 (30s 轮询)
  → 热读 config.ini [AutoFrost]
  → EnumWindows → 每窗口取 exe 名（QueryFullProcessImageNameW，小写）
  → 匹配 exe 名 (+可选类名)
  → 命中且不在 g_modified → apply_transparency(DefaultAlpha)
                            + launch_overlay(hwnd)（若 DefaultBlur=1）
```

复用现有函数：`apply_transparency()` / `launch_overlay()` / ini API — 与热键路径完全一致，零新渲染逻辑。

## 组件

### 1. 配置 — config.ini 新增 `[AutoFrost]` 节

```ini
[AutoFrost]
Enabled=1
DefaultAlpha=217          ; 85% (0-255)
DefaultBlur=1             ; 是否叠加 acrylic 模糊
App_0=Obsidian.exe|
App_1=WindowsTerminal.exe|
App_2=msedge.exe|
App_3=explorer.exe|CabinetWClass
App_4=cloudmusic.exe|
App_5=WeChat.exe|
App_6=CherryStudio.exe|
App_7=Code.exe|
```

- 每项格式 `exe名|类名`，类名可空（格式与现有 `[Session]` 的 `title|cls|alpha|tint` 同风格，API 用 `GetPrivateProfileStringW` 系列）
- 首次运行无 `[AutoFrost]` 节 → 写入预置 8 项
- `explorer.exe` 配 `CabinetWClass` 特例：同进程含任务栏（`Shell_TrayWnd`）/桌面，类名辅助排除

### 2. 监视线程

- `CreateThread` 启动，每 30 秒一轮
- 每轮热读配置（30s 一次 GetPrivateProfileString 开销可忽略，UI 修改下一轮即生效，无需信号机制）
- 每窗口：可见 + 非 `WS_EX_TOOLWINDOW` + 有标题 → `GetWindowThreadProcessId` → `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` → `QueryFullProcessImageNameW` → 取 basename → 小写
- 匹配：exe 名精确匹配（小写）；配置类名非空时窗口类名须全等
- 命中且不在 `g_modified` → 自动应用
- 清理（每轮）：`g_modified` 中 `IsWindow` 失效的 HWND erase（防 HWND 复用 → 新窗口漏自动应用）；`g_overlays` 中进程已退出者 TerminateProcess + erase（防残留）
- 跳过：进程访问失败、空 exe 名

### 3. 线程安全

`g_modified` / `g_overlays` 现由主线程（热键/session）与监视线程共享：

- 新增全局 `CRITICAL_SECTION g_fxLock`（WinMain 初始化）
- 保护以下所有访问：`apply_transparency` 内 `g_modified.insert`、`launch_overlay`/`kill_overlay` 内 `g_overlays`、`restore_session`/`save_session`、`shutdown`、监视线程
- 注意：`apply_transparency` / `launch_overlay` 在锁外调用不变量仍成立（集合读写均在函数内加锁即可，不在调用方加）

### 4. Settings UI — 加 Auto-frost 节

窗口加高（290×260 → 290×430），`WM_CREATE` 新增控件：

- "Auto-frost" 分组标题
- **Enable Auto-frost** checkbox（`[AutoFrost].Enabled`）
- **Default transparency** 滑块 50–100%（`DefaultAlpha` 换算）
- **Blur** checkbox（`DefaultBlur`）
- 软件列表 LISTBOX（只读，显示 `exe (class)`）
- **[+ Add current app]** 按钮：取 `show_settings()` 打开瞬间捕获的 `GetForegroundWindow()`（打开 Settings 前用户激活的软件窗口）→ 其 exe 名入列；空/无效/是 win2blur 自身 → 忽略
- **[Remove selected]** 按钮：删除 LISTBOX 选中项
- Apply（IDOK）写回 `[AutoFrost]` 全节；Close 照旧

`SettingsParams` 扩展：加 `pEnabled / pDefaultAlpha / pDefaultBlur / pApps` 指针（指向主程序共享的自动配置结构）。

## 数据流

```
Settings Apply → 写 [AutoFrost] → config.ini
                                     ↑ 热读（每 30s）
后台线程 → 匹配窗口 → apply_transparency(alpha) + launch_overlay
                        → g_modified / g_overlays（锁保护）
```

## 错误处理

| 场景 | 处理 |
|------|------|
| 进程句柄打开失败（权限） | 跳过该窗口，不影响其他 |
| 配置损坏（缺项/非数字） | 用默认值（Enabled=0、alpha=217、blur=1），不崩溃 |
| ini 写失败 | 忽略（现有行为） |
| 列表超长 | 上限 32 项（EnumPrivateProfile 顺序读取） |

## 测试

手动验证（当前项目无测试框架，沿用热键的手测模式）：

1. 启动 win2blur → 打开 Obsidian → ≤30s 后自动 85% + 模糊
2. ALT+↑ 手动调回 100% → 下轮不被抢回
3. 关闭 Obsidian 重开 → 新窗口再次自动应用
4. 资源管理器打开 → 自动应用；任务栏/桌面无变化（类名排除）
5. Settings：取消勾选 Enable → 下一轮不再自动应用新窗口
6. 添加自定义 exe（如 notepad.exe）→ 打开记事本 → 自动应用
7. Keep & Exit / Restore & Exit 与自动应用无冲突（exit 时 g_modified 走现有路径）

## 构建与发布

- `native/build5/` MinGW 构建 win2blur.exe（无 DLL 变更，DWM hook 不动）
- 打包 `dist_release_2.7/`
