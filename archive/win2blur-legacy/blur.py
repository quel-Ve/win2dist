"""
Acrylic Blur — Windows 10/11 毛玻璃效果搭档 Window2Clear
==========================================================
Window2Clear 负责透明度，本工具负责背景模糊。
两者叠加 = 半透明 + 毛玻璃（Frosted Glass）。

用法:
  python acrylic_blur.py                          # 交互选择窗口
  python acrylic_blur.py --title "记事本"          # 按标题匹配
  python acrylic_blur.py --hwnd 0x12345           # 按句柄
  python acrylic_blur.py --list                   # 列出所有可见窗口

参数:
  --type     blur | acrylic | mica                 (默认 acrylic)
  --tint     0xAARRGGBB 格式的色调                  (默认 0x80FFFFFF)
  --borders  none | all                             (默认 none)
  --remove                                         移除毛玻璃效果

毛玻璃参数详解见文件末尾注释。
"""

import ctypes
from ctypes import wintypes
import argparse
import sys
import os

# 强制 UTF-8 输出（Windows 终端中文支持）
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.stderr.reconfigure(encoding='utf-8', errors='replace')

# 共享窗口工具
from window2clear.window_utils import find_by_title as _find_by_title
from window2clear.window_utils import list_all as _list_all
from window2clear.window_utils import get_title as _get_title

# ============================================================
# Windows API 定义
# ============================================================

user32 = ctypes.windll.user32
dwmapi = ctypes.windll.dwmapi
kernel32 = ctypes.windll.kernel32

# --- AccentState 枚举 (undocumented, from Windows SDK reverse) ---
ACCENT_DISABLED                   = 0
ACCENT_ENABLE_GRADIENT            = 1
ACCENT_ENABLE_TRANSPARENTGRADIENT = 2
ACCENT_ENABLE_BLURBEHIND          = 3   # Win7 风格模糊
ACCENT_ENABLE_ACRYLICBLURBEHIND   = 4   # Win10 1803+ 亚克力模糊 ★推荐
ACCENT_ENABLE_HOSTBACKDROP        = 5   # Win11 Mica 云母
ACCENT_INVALID_STATE              = 6

ACCENT_LABEL = {
    3: "Blur Behind (Win7 模糊)",
    4: "Acrylic (Win10 亚克力) ★",
    5: "Mica (Win11 云母)",
}

# --- AccentFlags ---
ACCENT_FLAG_NONE            = 0
ACCENT_FLAG_DRAW_LEFT_BORDER   = 0x20
ACCENT_FLAG_DRAW_TOP_BORDER    = 0x40
ACCENT_FLAG_DRAW_RIGHT_BORDER  = 0x80
ACCENT_FLAG_DRAW_BOTTOM_BORDER = 0x100
ACCENT_FLAG_DRAW_ALL_BORDERS   = 0x20 | 0x40 | 0x80 | 0x100


class AccentPolicy(ctypes.Structure):
    _fields_ = [
        ("AccentState",  ctypes.c_int),
        ("AccentFlags",  ctypes.c_int),
        ("GradientColor", ctypes.c_uint),   # ARGB: 0xAARRGGBB
        ("AnimationId",  ctypes.c_int),
    ]


class WindowCompositionAttributeData(ctypes.Structure):
    _fields_ = [
        ("Attribute",  ctypes.c_int),       # WCA_ACCENT_POLICY = 19
        ("Data",       ctypes.c_void_p),
        ("SizeOfData", ctypes.c_int),
    ]


WCA_ACCENT_POLICY = 19

# SetWindowCompositionAttribute — undocumented, 动态获取
SetWindowCompositionAttribute = user32.SetWindowCompositionAttribute
SetWindowCompositionAttribute.argtypes = [wintypes.HWND, ctypes.POINTER(WindowCompositionAttributeData)]
SetWindowCompositionAttribute.restype = wintypes.BOOL


def apply_accent(hwnd, accent_state, gradient_color=0x80FFFFFF, accent_flags=0):
    """
    给窗口应用毛玻璃效果。

    参数:
        hwnd: 窗口句柄 (int)
        accent_state: ACCENT_ENABLE_ACRYLICBLURBEHIND(4) 等
        gradient_color: 0xAARRGGBB 格式色调
            AA = 色调不透明度 (00=无色 → FF=完全着色)
            RR,GG,BB = 色调颜色
            常用值:
                0x80FFFFFF — 50% 白色 (最自然)
                0x99000000 — 60% 暗色 (暗模式)
                0x40FFFFFF — 25% 白色 (更透明)
                0xCCFFFFFF — 80% 白色 (更不透明)
                0x00000000 — 无色 (纯模糊，无染色)
        accent_flags: 边框标志组合
    """
    policy = AccentPolicy()
    policy.AccentState = accent_state
    policy.AccentFlags = accent_flags
    policy.GradientColor = gradient_color
    policy.AnimationId = 0

    data = WindowCompositionAttributeData()
    data.Attribute = WCA_ACCENT_POLICY
    data.SizeOfData = ctypes.sizeof(policy)
    data.Data = ctypes.cast(ctypes.pointer(policy), ctypes.c_void_p)

    result = SetWindowCompositionAttribute(hwnd, ctypes.byref(data))
    if not result:
        err = ctypes.get_last_error()
        print(f"[✗] SetWindowCompositionAttribute 失败: error {err}")
        return False
    return True


def remove_accent(hwnd):
    """移除毛玻璃效果，恢复窗口正常外观。"""
    return apply_accent(hwnd, ACCENT_DISABLED, 0, 0)


# ============================================================
# 窗口查找（委托给 window_utils，保留别名向后兼容）
# ============================================================

def find_window_by_title(title_substr):
    """按标题模糊匹配，返回 [(hwnd, title), ...]。（向后兼容包装）"""
    return _find_by_title(title_substr)


def list_all_windows():
    """列出所有可见且有标题的窗口。（向后兼容包装）"""
    return _list_all()


def get_window_title(hwnd):
    """获取窗口标题。（向后兼容包装）"""
    return _get_title(hwnd)


# ============================================================
# CLI
# ============================================================

def parse_color(s):
    """解析 ARGB 颜色字符串。支持 0x 前缀或纯十六进制。"""
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        s = s[2:]
    return int(s, 16)


def main():
    parser = argparse.ArgumentParser(
        description="Windows 毛玻璃效果 — 搭档 Window2Clear",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s                         交互式选择窗口
  %(prog)s --title "Typora"        给 Typora 加毛玻璃
  %(prog)s --hwnd 0x12345          按句柄
  %(prog)s --list                  列出所有窗口
  %(prog)s --title "Typora" --remove  移除效果
  %(prog)s --title "Typora" --tint 0x99000000 --type blur
        """
    )
    parser.add_argument('--title', '-t', help='窗口标题（模糊匹配）')
    parser.add_argument('--hwnd', help='窗口句柄（十六进制）')
    parser.add_argument('--list', '-l', action='store_true', help='列出所有可见窗口')
    parser.add_argument('--type', dest='accent_type',
                        choices=['blur', 'acrylic', 'mica'],
                        default='acrylic',
                        help='模糊类型 (默认: acrylic)')
    parser.add_argument('--tint', type=parse_color, default=0x80FFFFFF,
                        help='色调 ARGB (默认: 0x80FFFFFF = 50%% 白色)')
    parser.add_argument('--borders', choices=['none', 'all'], default='none',
                        help='是否显示窗口边框 (默认: none)')
    parser.add_argument('--remove', '-r', action='store_true',
                        help='移除毛玻璃效果')

    args = parser.parse_args()

    # 类型映射
    type_map = {
        'blur':    ACCENT_ENABLE_BLURBEHIND,
        'acrylic': ACCENT_ENABLE_ACRYLICBLURBEHIND,
        'mica':    ACCENT_ENABLE_HOSTBACKDROP,
    }
    accent_state = type_map[args.accent_type]
    accent_flags = ACCENT_FLAG_DRAW_ALL_BORDERS if args.borders == 'all' else ACCENT_FLAG_NONE

    if args.remove:
        accent_state = ACCENT_DISABLED

    # --- 列出窗口 ---
    if args.list:
        windows = list_all_windows()
        print(f"{'HWND':>10}  {'标题'}")
        print("-" * 60)
        for hwnd, title in sorted(windows, key=lambda x: x[1].lower()):
            print(f"0x{hwnd:08X}  {title}")
        return

    # --- 查找窗口 ---
    target_hwnd = None
    target_title = ""

    if args.hwnd:
        target_hwnd = parse_color(args.hwnd)
        target_title = get_window_title(target_hwnd)
    elif args.title:
        matches = find_window_by_title(args.title)
        if not matches:
            print(f"[✗] 未找到标题包含 '{args.title}' 的窗口")
            sys.exit(1)
        if len(matches) == 1:
            target_hwnd, target_title = matches[0]
        else:
            print(f"找到 {len(matches)} 个匹配窗口：")
            for i, (hwnd, title) in enumerate(matches, 1):
                print(f"  [{i}] 0x{hwnd:08X} — {title}")
            choice = input("选哪个？输入序号: ").strip()
            try:
                target_hwnd, target_title = matches[int(choice) - 1]
            except (ValueError, IndexError):
                print("[✗] 无效选择")
                sys.exit(1)
    else:
        # 交互模式
        windows = list_all_windows()
        print(f"共 {len(windows)} 个可见窗口。输入关键词过滤（回车显示全部）：")
        keyword = input("> ").strip()
        if keyword:
            windows = [(h, t) for h, t in windows if keyword.lower() in t.lower()]
            if not windows:
                print(f"[✗] 无匹配窗口")
                sys.exit(1)

        for i, (hwnd, title) in enumerate(windows[:50], 1):
            print(f"  [{i}] 0x{hwnd:08X} — {title}")
        if len(windows) > 50:
            print(f"  ...(共 {len(windows)} 个，仅显示前 50)")

        choice = input(f"选哪个？(1-{min(len(windows), 50)}): ").strip()
        try:
            target_hwnd, target_title = windows[int(choice) - 1]
        except (ValueError, IndexError):
            print("[✗] 无效选择")
            sys.exit(1)

    # --- 应用效果 ---
    if target_hwnd:
        if args.remove:
            success = remove_accent(target_hwnd)
            if success:
                print(f"[✓] 已移除毛玻璃: {target_title}")
        else:
            success = apply_accent(target_hwnd, accent_state, args.tint, accent_flags)
            if success:
                label = ACCENT_LABEL.get(accent_state, str(accent_state))
                print(f"[✓] 毛玻璃已应用: {target_title}")
                print(f"    类型: {label}")
                print(f"    色调: 0x{args.tint:08X}")
                print(f"    边框: {'是' if accent_flags else '否'}")
                print(f"    ── 与 Window2Clear 透明度叠加 = 半透明毛玻璃 ✓")


# ============================================================
# 毛玻璃效果参数详解
# ============================================================
"""
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  可调参数完整说明
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. AccentState（模糊类型）:
   ┌──────────┬─────┬─────────────────────────────────┐
   │ 类型     │ ID  │ 效果                            │
   ├──────────┼─────┼─────────────────────────────────┤
   │ Blur     │  3  │ Win7 时代背景模糊，较弱的散射     │
   │ Acrylic  │  4  │ Win10 1803+ 亚克力 ★推荐        │
   │          │     │ 三层合成: 背景模糊 + 亮度调节      │
   │          │     │ + 色调覆盖。最接近 macOS 毛玻璃   │
   │ Mica     │  5  │ Win11 云母，只采样壁纸一次        │
   │          │     │ 性能最优但模糊度不如 Acrylic      │
   └──────────┴─────┴─────────────────────────────────┘

2. GradientColor（色调，0xAARRGGBB）:
   AA = Alpha，控制色调不透明度（不是窗口透明度！窗口透明度由 Window2Clear 控制）
   RR = 红, GG = 绿, BB = 蓝

   常用预设:
   ┌──────────────┬─────────────────────────────────┐
   │ 值           │ 效果                            │
   ├──────────────┼─────────────────────────────────┤
   │ 0x80FFFFFF   │ 50% 白 — 最自然的毛玻璃（默认）  │
   │ 0x00000000   │ 无色纯模糊 — 最透明              │
   │ 0xCCFFFFFF   │ 80% 白 — 乳白，像磨砂玻璃        │
   │ 0x40FFFFFF   │ 25% 白 — 隐约有光感              │
   │ 0x99000000   │ 60% 暗色 — 暗模式               │
   │ 0xCC222222   │ 深灰 — 极暗模式                 │
   │ 0x80FFEEDD   │ 暖色模糊 — 护眼色调             │
   └──────────────┴─────────────────────────────────┘

   AA 越高 → 内容越不透明（文字更清晰，背景更少暴露）
   AA 越低 → 内容越透明（背景更清晰，文字更难辨认）

3. AccentFlags（边框）:
   - none:  无额外边框（默认，干净）
   - all:   四边绘制 1px 边框（颜色由 GradientColor 决定）
   也可以单独指定某一边: LEFT | TOP | RIGHT | BOTTOM

4. 与 Window2Clear 的叠加关系:
   ┌──────────────────────────────────────────┐
   │  Window2Clear 控制  │  本工具控制         │
   ├─────────────────────┼────────────────────┤
   │  窗口整体透明度      │  背景模糊度         │
   │  (0-100%)          │  (Blur/Acrylic/Mica)│
   │  SetLayeredWindow   │  SetWindowCompo-    │
   │  Attributes         │  sitionAttribute    │
   └─────────────────────┴────────────────────┘
   叠加效果 = 你透过半透明窗口看到经过模糊处理的背景

5. 系统级限制（无法通过 API 调整）:
   - 模糊半径: 由 DWM 内部控制，约 30px 高斯模糊
   - 亮度混合: Acrylic 自动做亮度/噪点混合，无法关闭
   - 性能模式: 省电模式会自动禁用模糊

6. 已知兼容性:
   - ✓ Win10 1803+ (Acrylic)
   - ✓ Win11 (Acrylic + Mica)
   - ✗ Win7/8 (仅 Blur 可用，且效果较弱)
   - ✓ 与 Window2Clear 透明度叠加测试通过
   - ⚠ Mica 在部分 Win32 窗口可能不生效
"""


if __name__ == '__main__':
    main()
