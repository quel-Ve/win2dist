"""
毛玻璃效果拆解演示 — 独立方块窗口
================================
不依赖 Window2Clear，一步一展示。
用 tkinter 创建一个带文字的方块窗口，
依次演示 Windows 毛玻璃的各个层级。

用法:
  python -m window2clear.demo
  python -m window2clear.demo --step 1   直接从第 N 步开始
"""

import tkinter as tk
import ctypes
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# 复用 blur.py 的 Windows API
from window2clear.blur import (
    apply_accent,
    remove_accent,
    ACCENT_DISABLED,
    ACCENT_ENABLE_BLURBEHIND,
    ACCENT_ENABLE_ACRYLICBLURBEHIND,
)

# ============================================================
# Windows API (tkinter 需要 FindWindowW)
# ============================================================
user32 = ctypes.windll.user32


# ============================================================
# Demo 窗口
# ============================================================
STEPS = [
    # (标题, AccentState, GradientColor, 说明)
    ("Step 0 — 原始窗口（无效果）", ACCENT_DISABLED, 0x00000000,
     "一个普通的 tkinter 方块窗口，什么都没有。\n"
     "这就是基准状态。"),

    ("Step 1 — 启用 Blur Behind（ACCENT_ENABLE_BLURBEHIND=3）", ACCENT_ENABLE_BLURBEHIND, 0x00000000,
     "梯度色=0x00000000，纯模糊无染色。\n"
     "应该能看到窗口后面的内容被 DWM 高斯模糊了。\n"
     "如果看不到任何变化 → Blur Behind 在你的系统上不可用。\n"
     "常见原因：DWM 组合未开启、远程桌面、或旧显卡驱动。"),

    ("Step 2 — Blur + 50% 白色染色（0x80FFFFFF）", ACCENT_ENABLE_BLURBEHIND, 0x80FFFFFF,
     "现在加了一层 50% 透明度的白色盖在模糊上。\n"
     "窗口应该看起来像磨砂玻璃——背景模糊 + 泛白。\n"
     "如果 Step1 没模糊，这一步只会看到白色半透明（无模糊）。"),

    ("Step 3 — Blur + 80% 白色染色（0xCCFFFFFF）", ACCENT_ENABLE_BLURBEHIND, 0xCCFFFFFF,
     "更浓的白色——几乎看不到背景了。\n"
     "如果前面有模糊，这里应该是很强的磨砂感。"),

    ("Step 4 — 切换为 Acrylic（ACCENT_ENABLE_ACRYLICBLURBEHIND=4）", ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x80FFFFFF,
     "Acrylic = Win10 1803+ 的亚克力效果。\n"
     "与 Blur 的区别：Acrylic 多了一层亮度调节 + 噪点纹理。\n"
     "如果 Acrylic 不支持，窗口会回退到无效果。\n"
     "在省电模式下 Windows 会自动禁用 Acrylic！"),

    ("Step 5 — Acrylic + 无色（0x00000000）", ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x00000000,
     "纯亚克力模糊，不加任何色调。\n"
     "这是最接近 macOS 毛玻璃的效果。\n"
     "如果还是看不到模糊 → 你的系统环境禁用了 DWM 合成效果。"),

    ("Step 6 — 移除所有效果（恢复原始）", ACCENT_DISABLED, 0x00000000,
     "回到了 Step 0 的状态。\n"
     "演示结束。结论：用数字键 0-6 可随时切换。"),
]

TIPS = """
┌────────────────── 诊断清单 ──────────────────┐
│ 如果所有步骤都看不到模糊:                      │
│   □ Win+R → services.msc → Desktop Window     │
│     Manager Session Manager → 确保正在运行     │
│   □ 设置 → 个性化 → 颜色 → 透明效果 → 开启     │
│   □ 电源选项 → 非省电模式                      │
│   □ 远程桌面/虚拟机 → DWM 合成可能被禁用        │
│                                               │
│ 如果只有 Acrylic 没效果(Step 4-5):             │
│   □ Win10 < 1803 → 不支持 Acrylic             │
│   □ 电池模式 → Windows 自动关闭 Acrylic        │
│                                               │
│ 透明度与毛玻璃的关系:                          │
│   Window2Clear 用的是 SetLayeredWindowAttributes │
│   控制窗口整体透明(opacity 0-255)               │
│   Acrylic/Blur 是背景模糊，独立功能             │
│   两者叠加时透明度太高会让模糊不明显             │
└───────────────────────────────────────────────┘
"""


class BlurDemo:
    def __init__(self, start_step=0):
        self.step = start_step
        self.root = tk.Tk()
        self.root.title("毛玻璃拆解演示")
        self.root.geometry("520x620+800+250")
        self.root.configure(bg='#1e1e1e')
        self.root.resizable(False, False)

        # 确保窗口创建后再操作 hwnd
        self.root.update_idletasks()
        self.hwnd = self._get_hwnd()

        # UI
        self._build_ui()
        self._apply_step(self.step)
        self._bind_keys()

    def _get_hwnd(self):
        self.root.update()
        # 通过窗口标题获取 hwnd
        hwnd = user32.FindWindowW(None, "毛玻璃拆解演示")
        return hwnd

    def _build_ui(self):
        # 标题
        title = tk.Label(
            self.root, text="🔍 毛玻璃效果拆解演示",
            font=("Microsoft YaHei UI", 16, "bold"),
            fg='#e0e0e0', bg='#1e1e1e'
        )
        title.pack(pady=(20, 5))

        # 步骤标签
        self.step_label = tk.Label(
            self.root, text="",
            font=("Cascadia Code", 11),
            fg='#4fc3f7', bg='#252525',
            justify=tk.LEFT, anchor='w', padx=10, pady=6,
            wraplength=480
        )
        self.step_label.pack(fill=tk.X, padx=20, pady=(5, 0))

        # 说明文字
        self.desc_label = tk.Label(
            self.root, text="",
            font=("Microsoft YaHei UI", 10),
            fg='#b0b0b0', bg='#252525',
            justify=tk.LEFT, anchor='w', padx=10, pady=8,
            wraplength=480
        )
        self.desc_label.pack(fill=tk.X, padx=20, pady=(5, 0))

        # 状态指示器
        self.status_frame = tk.Frame(self.root, bg='#1e1e1e')
        self.status_frame.pack(fill=tk.X, padx=20, pady=(10, 5))

        self.status_box = tk.Label(
            self.status_frame, text="",
            font=("Cascadia Code", 10, "bold"),
            fg='#1e1e1e', bg='#555555',
            width=48, height=2,
            justify=tk.CENTER
        )
        self.status_box.pack()

        # 按键提示
        keys_frame = tk.Frame(self.root, bg='#1e1e1e')
        keys_frame.pack(fill=tk.X, padx=20, pady=(10, 0))

        tk.Label(
            keys_frame, text="按键切换",
            font=("Microsoft YaHei UI", 9),
            fg='#888888', bg='#1e1e1e'
        ).pack(side=tk.LEFT)

        for i in range(len(STEPS)):
            btn = tk.Label(
                keys_frame,
                text=str(i),
                font=("Cascadia Code", 10, "bold"),
                fg='#cccccc', bg='#333333',
                width=3, height=1,
                cursor='hand2'
            )
            btn.pack(side=tk.LEFT, padx=1)
            btn.bind('<Button-1>', lambda e, s=i: self._apply_step(s))

        # 诊断清单
        self.tips_label = tk.Label(
            self.root, text=TIPS,
            font=("Cascadia Code", 8),
            fg='#666666', bg='#1e1e1e',
            justify=tk.LEFT, anchor='w',
        )
        self.tips_label.pack(fill=tk.BOTH, padx=20, pady=(15, 10))

    def _apply_step(self, step_idx):
        self.step = step_idx
        title, accent_state, gradient, desc = STEPS[step_idx]

        self.root.title(title)
        self.step_label.config(text=title)

        # 提取第一行作为摘要
        summary = desc.strip().split('\n')[0] if desc else ""
        self.desc_label.config(text=desc)

        # 状态盒
        state_names = {
            0: "OFF — 无效果",
            3: "BLUR — DWM 高斯模糊",
            4: "ACRYLIC — Win10 亚克力",
        }
        state_name = state_names.get(accent_state, f"STATE={accent_state}")
        color_hex = f"0x{gradient:08X}"
        self.status_box.config(
            text=f"{state_name}  │  色调: {color_hex}  │  按 0-{len(STEPS)-1} 切换"
        )

        # 颜色指示: disabled=灰, blur=蓝, acrylic=青
        colors = {0: '#444444', 3: '#1565c0', 4: '#00838f'}
        self.status_box.config(bg=colors.get(accent_state, '#555'))

        # 应用效果
        self.root.update_idletasks()
        hwnd = self._get_hwnd()
        if hwnd:
            apply_accent(hwnd, accent_state, gradient)
        else:
            print("[!] 无法获取窗口句柄，请检查窗口是否已创建")

    def _bind_keys(self):
        for i in range(len(STEPS)):
            self.root.bind(str(i), lambda e, s=i: self._apply_step(s))
        self.root.bind('<Escape>', lambda e: self.root.destroy())
        self.root.bind('q', lambda e: self.root.destroy())

    def run(self):
        self.root.mainloop()


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--step', type=int, default=0)
    args = p.parse_args()

    demo = BlurDemo(start_step=args.step)
    demo.run()
