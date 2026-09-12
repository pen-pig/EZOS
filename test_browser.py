# test_browser.py - QMP 自动化：'b' 热键开 Browser -> 小恐龙游戏冒烟测试
# 验证：Chrome 工具栏 + 白页面 + 恐龙 -> SPACE 开始 -> 帧泵动画（页面区全分辨率
#       帧间差异，仙人掌/碎石是 2-4px 细特征不能用粗采样）-> 蹲下(灰像素高度减半)
# 注意：不要在 GUI 里输入含热键字母的文本（'t' 开 Terminal）；只用 spc/down
import time, subprocess, sys
from test_tic import Qmp, load_ppm, QEMU
from test_features import count_color

PASS, FAIL = 0, 0
def check(name, ok, info=""):
    global PASS, FAIL
    print(f"  [{'PASS' if ok else 'FAIL'}] {name} {info}")
    PASS, FAIL = PASS + (1 if ok else 0), FAIL + (0 if ok else 1)

def gray_bbox(px, w, h, x0, y0, x1, y1, tol=12):
    """页面区内 0x535353(≈80,80,80) 灰像素包围盒 (minx,miny,maxx,maxy,count)"""
    mnx = mny = 1 << 30; mxx = mxy = -1; n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y*w + x)*3
            if abs(px[i]-80) <= tol and abs(px[i+1]-80) <= tol and abs(px[i+2]-80) <= tol:
                n += 1
                if x < mnx: mnx = x
                if x > mxx: mxx = x
                if y < mny: mny = y
                if y > mxy: mxy = y
    return (mnx, mny, mxx, mxy, n) if n else None

def diff_px(a, b, w, x0, y0, x1, y1):
    """区域内逐像素差异数（step=1，细特征不漏）"""
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y*w + x)*3
            if a[i:i+3] != b[i:i+3]:
                n += 1
    return n

def find_toolbar(px, w, h):
    """Chrome 工具栏灰 0xDEE1E6(≈216,224,224) 定位窗口页面区 (x0,x1,ty)"""
    rows = []
    for y in range(0, h, 2):
        c = 0
        for x in range(0, w, 4):
            i = (y*w + x)*3
            if abs(px[i]-216) <= 12 and abs(px[i+1]-224) <= 12 and abs(px[i+2]-224) <= 12:
                c += 1
        if c > w//16:
            rows.append((y, c))
    if not rows:
        return None
    ty = rows[0][0]
    # 该行上工具栏像素的水平范围
    xs = [x for x in range(0, w, 2)
          if abs(px[(ty*w+x)*3]-216) <= 12 and abs(px[(ty*w+x)*3+1]-224) <= 12
          and abs(px[(ty*w+x)*3+2]-224) <= 12]
    return (min(xs), max(xs), ty)

def page_bottom(px, w, h, wx0, wx1, ty):
    """页面区底边：从工具栏向下找最后一行 Majority-white 行。
    窗口是 2x 缩放（页面可到 ty+600），不能硬编码高度"""
    last = ty + 60
    for y in range(ty + 50, min(h, ty + 700)):
        cnt = tot = 0
        for x in range(wx0 + 40, wx1 - 40, 4):
            i = (y*w + x)*3
            tot += 1
            if px[i] >= 246 and px[i+1] >= 246 and px[i+2] >= 246:
                cnt += 1
        if cnt > tot * 0.6:
            last = y
    return last

def main():
    proc = subprocess.Popen([QEMU, "-icount","shift=auto",
        "-vga","std",
        "-drive","format=raw,file=os-image.bin",
        "-drive","format=raw,file=disk.img",
        "-qmp","tcp:127.0.0.1:4444,server,nowait",
        "-display","none","-serial","none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        q = Qmp()
        print("[QMP] connected")
        time.sleep(2)
        q.type_text("exit\n"); time.sleep(3)   # exit 直接进桌面
        q.type_text("b"); time.sleep(1.5)      # 'b' 热键启动 Browser
        q.screendump("br_idle.ppm")
        w, h, idle = load_ppm("br_idle.ppm")
        print(f"[STEP] desktop {w}x{h}")
        if w < 640:
            print("FAIL: 未进入图形模式"); return 1

        tb = find_toolbar(idle, w, h)
        if not tb:
            check("浏览器窗口打开(工具栏定位)", False)
            print(f"\n===== 结果: {PASS} PASS / {FAIL} FAIL =====")
            return 1
        wx0, wx1, ty = tb
        py0 = ty + 50          # 工具栏 24px + 边距，页面区
        pb = page_bottom(idle, w, h, wx0, wx1, ty)   # 页面区底边（2x 缩放窗口）
        print(f"[STEP] window toolbar x[{wx0},{wx1}] y={ty} page y[{py0},{pb}]")
        white = count_color(idle, w, h, 248, 252, 248, tol=12,
                            x0=wx0, y0=py0, x1=wx1, y1=pb)
        dino = count_color(idle, w, h, 80, 80, 80, tol=12,
                           x0=wx0, y0=py0, x1=wx1, y1=pb)
        check("浏览器窗口打开(工具栏定位)", True)
        check("白色页面区", white > 50000, f"(white={white})")
        check("待机画面恐龙渲染", dino > 200, f"(dino={dino})")

        # SPACE 开始 -> 帧泵动画（页面区全分辨率帧间差异）。
        # 2x 缩放精灵体积小 + 带状 blit 中部纯白区不回传，正常 1s 间隔
        # diff ≈ 600-1500（分数/云/地面/恐龙腿），阈值取 500 防偶发
        q.type_text(" "); time.sleep(2.5)      # 等仙人掌进场（首障碍 ~1.7s）
        q.screendump("br_a.ppm"); _, _, a = load_ppm("br_a.ppm")
        time.sleep(1.0)
        q.screendump("br_b.ppm"); _, _, b = load_ppm("br_b.ppm")
        d = diff_px(a, b, w, wx0, py0, wx1, pb + 1)
        check("游戏运行帧间动画", d > 500, f"(diff={d})")

        # 仙人掌进场：右侧灰像素较开局明显增多（恐龙固定在左侧 x≈wx0+40）
        q.screendump("br_c.ppm"); _, _, c = load_ppm("br_c.ppm")
        mid = (wx0 + wx1) // 2
        gr_right = count_color(c, w, h, 80, 80, 80, tol=12,
                               x0=mid, y0=py0, x1=wx1, y1=pb)
        check("障碍物(仙人掌)进场", gr_right > 30, f"(right-gray={gr_right})")

        # 蹲下：灰像素包围盒高度应明显变小（行高 2px -> 1px，身高减半）。
        # sendkey 会自动松开（截图前已起身），必须手动按下->截图->松开
        bb_stand = gray_bbox(c, w, h, wx0, py0, wx0+160, pb + 1)
        q.cmd("input-send-event", events=[{"type":"key","data":{
            "down":True,"key":{"type":"qcode","data":"down"}}}])
        time.sleep(0.4)
        q.screendump("br_duck.ppm")
        q.cmd("input-send-event", events=[{"type":"key","data":{
            "down":False,"key":{"type":"qcode","data":"down"}}}])
        _, _, dk = load_ppm("br_duck.ppm")
        bb_duck = gray_bbox(dk, w, h, wx0, py0, wx0+160, pb + 1)
        ok = bb_stand and bb_duck and \
             (bb_stand[3]-bb_stand[1]) > (bb_duck[3]-bb_duck[1]) + 8
        info = f"(stand_h={bb_stand[3]-bb_stand[1] if bb_stand else '?'} " \
               f"duck_h={bb_duck[3]-bb_duck[1] if bb_duck else '?'})"
        check("下键蹲下(身高减半)", ok, info)

        print(f"\n===== 结果: {PASS} PASS / {FAIL} FAIL =====")
        return 0 if FAIL == 0 else 1
    finally:
        proc.kill()

if __name__ == "__main__":
    sys.exit(main())
