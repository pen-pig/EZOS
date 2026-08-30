# test_gnome.py - QMP 自动化：theme 命令切 Ubuntu GNOME -> exit 直接进桌面 -> 验证顶栏/壁纸/强调色/热键
import time, subprocess, sys
from test_tic import Qmp, load_ppm, count_color, QEMU

PASS, FAIL = 0, 0
def check(name, ok, info=""):
    global PASS, FAIL
    print(f"  [{'PASS' if ok else 'FAIL'}] {name} {info}")
    PASS, FAIL = PASS + (1 if ok else 0), FAIL + (0 if ok else 1)

def strip_count(px, w, y0, y1, r, g, b, tol=12):
    n = 0
    for y in range(y0, y1):
        for x in range(0, w, 2):
            i = (y*w + x)*3
            if abs(px[i]-r)<=tol and abs(px[i+1]-g)<=tol and abs(px[i+2]-b)<=tol:
                n += 1
    return n * 2

def avg_color(px, w, y0, y1):
    r = g = b = n = 0
    for y in range(y0, y1, 4):
        for x in range(0, w, 8):
            i = (y*w + x)*3
            r += px[i]; g += px[i+1]; b += px[i+2]; n += 1
    return r//n, g//n, b//n

def main():
    proc = subprocess.Popen([QEMU, "-vga","std",
        "-drive","format=raw,file=os-image.bin",
        "-drive","format=raw,file=disk.img",
        "-qmp","tcp:127.0.0.1:4444,server,nowait",
        "-display","none","-serial","none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        q = Qmp()
        print("[QMP] connected")
        time.sleep(2)

        # ---- 1. shell 里切 Ubuntu GNOME 主题（idx 6），再 exit 直接进桌面 ----
        q.type_text("theme 6\n"); time.sleep(0.8)
        q.type_text("exit\n"); time.sleep(3)
        q.screendump("g_gnome.ppm")
        w, h, gn = load_ppm("g_gnome.ppm")
        print(f"[STEP] desktop {w}x{h}")
        if w < 640:
            print("FAIL: 未进入图形模式"); return 1
        th = 32 if h >= 768 else (28 if h >= 400 else 18)

        # ---- 2. 验证 GNOME 主题特征 ----
        # 2a. 顶部黑栏 0x1D1D1D
        topbar = strip_count(gn, w, 4, th-4, 0x1D, 0x1D, 0x1D)
        check("GNOME 顶部黑栏", topbar > w*(th-8)*0.5, f"(topbar={topbar})")

        # 2b. aubergine 壁纸渐变（上部暗紫 0x2C001E -> 下部亮紫 0x5E2750）
        top_bg = avg_color(gn, w, th+20, th+80)
        bot_bg = avg_color(gn, w, h-120, h-60)
        check("aubergine 壁纸(上部暗紫)", top_bg[0] < 90 and top_bg[2] > top_bg[1] + 8, f"(avg={top_bg})")
        check("aubergine 壁纸(下部亮紫)", bot_bg[0] < 140 and bot_bg[2] > bot_bg[1] + 8, f"(avg={bot_bg})")

        # 2c. Ubuntu 橙 0xE95420（桌面右下角 logo 圆环 / 图标强调）
        orange = count_color(gn, w, h, 0xE9, 0x54, 0x20, tol=20)
        check("Ubuntu 橙强调色出现", orange > 100, f"(orange={orange})")

        # 2d. 底部不再是任务栏（壁纸延伸到底）
        bot_dark = strip_count(gn, w, h-th+4, h-4, 0x1D, 0x1D, 0x1D)
        check("底部无任务栏(壁纸到底)", bot_dark < w*(th-8)*0.3, f"(bot_dark={bot_dark})")

        # ---- 3. GNOME 下热键 't' 开 Terminal 仍正常 ----
        q.type_text("t"); time.sleep(1.5)
        q.screendump("g_term.ppm")
        _, _, tm = load_ppm("g_term.ppm")
        term_bg = count_color(tm, w, h, 0xF0, 0xF0, 0xF0, tol=10)
        check("GNOME 下 Terminal 打开", term_bg > 20000, f"(bg={term_bg})")

        # ---- 4. Terminal 里执行 theme 0 切回，GUI 内即时生效（顶栏消失变底栏）----
        # 注：帧缓冲为 16bpp RGB565，screendump 后颜色有量化偏移
        #     GNOME 顶栏 0x1D1D1D -> (24,28,24)，Win10 底栏 0x171717(alpha235) -> (8,20,16)
        q.type_text("theme 0\n"); time.sleep(1.5)
        q.screendump("g_back.ppm")
        _, _, bk = load_ppm("g_back.ppm")
        topbar2 = strip_count(bk, w, 4, th-4, 0x1D, 0x1D, 0x1D)
        botbar2 = strip_count(bk, w, h-th+4, h-4, 0x10, 0x14, 0x10, tol=10)
        check("GUI 内 theme 0 切回 Win10(顶栏消失)", topbar2 < w*(th-8)*0.3, f"(top={topbar2})")
        check("Win10 底部任务栏出现", botbar2 > w*(th-8)*0.3, f"(bot={botbar2})")

        print(f"\n===== 结果: {PASS} PASS / {FAIL} FAIL =====")
        return 0 if FAIL == 0 else 1
    finally:
        proc.kill()

if __name__ == "__main__":
    sys.exit(main())
