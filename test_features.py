# test_features.py - 新功能端到端测试
# Phase 1: 内核 shell Tab 补全 + 命令历史（文本模式 OCR 验证）
# Phase 2: GUI 任务栏窗口按钮（最小化/恢复）
# Phase 3: 桌面图标双击启动井字棋（音效不影响渲染）
# Phase 4: 双击标题栏最大化/还原
# Phase 5: Ubuntu GNOME 主题 + 左侧 Dock 渲染 + Dock 点击启动应用
import socket, json, time, subprocess, sys, re
from collections import Counter

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"

# ---------- VGA 文本模式 OCR (80x25, 8x16 字体) ----------
_font = None
def load_font():
    global _font
    if _font is not None: return _font
    with open('kernel/vga_font.h', 'r', encoding='utf-8', errors='ignore') as f:
        src = f.read()
    nums = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', src)]
    _font = {ch: nums[ch*16:(ch+1)*16] for ch in range(256)}
    return _font

def load_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def ocr_text(path):
    """解码 VGA 文本模式截图为 80x25 文本"""
    w, h, px = load_ppm(path)
    font = load_font()
    cols, rows = 80, 25
    cw, chh = w // cols, h // rows
    blank = {code for code, glyph in font.items() if all(b == 0 for b in glyph)}
    lines = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            best, best_score = '?', -1
            for code, glyph in font.items():
                score = 0
                for gy in range(16):
                    rowbits = glyph[gy]
                    yy = r*chh + gy
                    for gx in range(8):
                        xx = c*cw + gx
                        i = (yy*w + xx)*3
                        lum = (px[i]+px[i+1]+px[i+2])//3
                        if bool(rowbits & (0x80 >> gx)) == (lum > 96):
                            score += 1
                if score > best_score:
                    best_score, best = score, code
            if best in blank:
                line += ' '
            else:
                line += chr(best) if 32 <= best < 127 else '.'
        lines.append(line)
    return '\n'.join(lines)

# ---------- QMP 客户端 ----------
class Qmp:
    def __init__(self, host="127.0.0.1", port=4444):
        for _ in range(20):
            try:
                self.sock = socket.create_connection((host, port), timeout=2)
                break
            except OSError:
                time.sleep(0.3)
        else:
            raise RuntimeError("QMP connect failed")
        self.f = self.sock.makefile("rwb")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.f.readline()
        return json.loads(line) if line.strip() else None

    def cmd(self, execute, **args):
        obj = {"execute": execute}
        if args: obj["arguments"] = args
        self.f.write((json.dumps(obj) + "\n").encode())
        self.f.flush()
        while True:
            resp = self._read()
            if resp is None: return None
            if "return" in resp or "error" in resp:
                if "error" in resp: print(f"  [QMP err] {execute}: {resp['error']}")
                return resp

    def hmp(self, cmdline):
        return self.cmd("human-monitor-command", **{"command-line": cmdline}).get("return", "")

    def sendkey(self, key, hold=35):
        for down in (True, False):
            evs = [{"type":"key","data":{"down":down,"key":{"type":"qcode","data":key}}}]
            self.cmd("input-send-event", events=evs)
            time.sleep(hold/1000)

    def type_text(self, text, delay=0.06):
        KM = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
        KM.update({'\n':'ret','\r':'ret','\t':'tab',' ':'spc',
                   '.':'dot','/':'slash','-':'minus'})
        for ch in text:
            kc = KM.get(ch)
            if kc is None:
                print(f"  [warn] no keymap {ch!r}"); continue
            self.sendkey(kc)
            time.sleep(delay)

    def key(self, name):   # up/down/left/right/pgup 等
        self.sendkey(name, 45)

    def mouse_rel(self, dx, dy, step=50, gap=0.04):
        """精确相对移动：按比例分配每步增量，总和严格等于 (dx,dy)，
        避免取整累积误差导致光标漂移（曾使 Phase4 双击偏出标题栏）"""
        import math
        n = max(1, math.ceil(max(abs(dx), abs(dy)) / step))
        sx = sy = 0
        for i in range(1, n + 1):
            tx, ty = round(dx * i / n), round(dy * i / n)
            ax, ay = tx - sx, ty - sy
            sx, sy = tx, ty
            evs = [{"type":"rel","data":{"axis":"x","value":ax}},
                   {"type":"rel","data":{"axis":"y","value":ay}}]
            self.cmd("input-send-event", events=evs)
            time.sleep(gap)

    def mouse_click(self):
        self.cmd("input-send-event", events=[{"type":"btn","data":{"down":True,"button":"left"}}])
        time.sleep(0.12)
        self.cmd("input-send-event", events=[{"type":"btn","data":{"down":False,"button":"left"}}])
        time.sleep(0.15)

    def mouse_dclick(self):
        """快速双击：两次按下间隔 ~150ms，满足 OS 400ms 双击判定"""
        for _ in range(2):
            self.cmd("input-send-event", events=[{"type":"btn","data":{"down":True,"button":"left"}}])
            time.sleep(0.05)
            self.cmd("input-send-event", events=[{"type":"btn","data":{"down":False,"button":"left"}}])
            time.sleep(0.05)
        time.sleep(0.15)

    def screendump(self, name):
        self.hmp(f"screendump {name}")
        time.sleep(0.4)

PASS = []
FAIL = []
def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}")

def count_color(px, w, h, r, g, b, tol=14, x0=0, y0=0, x1=None, y1=None):
    if x1 is None: x1 = w
    if y1 is None: y1 = h
    n = 0
    for y in range(y0, min(y1, h), 2):
        for x in range(x0, min(x1, w), 2):
            i = (y*w + x)*3
            if abs(px[i]-r)<=tol and abs(px[i+1]-g)<=tol and abs(px[i+2]-b)<=tol:
                n += 1
    return n * 4

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

        # ===== Phase 1: 内核 shell Tab 补全 + 历史 =====
        print("== Phase 1: shell Tab 补全 + 历史 ==")
        q.type_text("hel"); time.sleep(0.3)
        q.key("tab"); time.sleep(0.5)
        q.screendump("f_tab.ppm")
        txt = ocr_text("f_tab.ppm")
        check("Tab 补全 hel->help", "> help" in txt)
        q.type_text("\n"); time.sleep(1.5)   # 执行 help

        q.key("up"); time.sleep(0.5)         # 历史回填
        q.screendump("f_hist.ppm")
        txt = ocr_text("f_hist.ppm")
        check("Up 键历史回填 help", "> help" in txt)
        q.type_text("\n"); time.sleep(0.3)   # 不再执行，占位回车 -> 会再跑一次 help

        q.type_text("c"); time.sleep(0.3)
        q.key("tab"); time.sleep(0.5)        # 多匹配列出
        q.screendump("f_multi.ppm")
        txt = ocr_text("f_multi.ppm")
        check("多匹配列出 clear/cls", "clear" in txt and "cls" in txt)
        q.type_text("\n"); time.sleep(0.3)

        # ===== Phase 2: GUI 任务栏窗口按钮 =====
        print("== Phase 2: 任务栏窗口按钮 ==")
        q.type_text("exit\n"); time.sleep(3)   # exit 直接进入图形桌面
        q.type_text("t"); time.sleep(1.5)    # 热键打开 Terminal
        q.screendump("f_term.ppm")
        w, h, base = load_ppm("f_term.ppm")
        term_white = count_color(base, w, h, 240, 240, 240)
        check("Terminal 窗口打开", term_white > 20000, f"(white={term_white})")

        # 光标移到任务栏 Terminal 按钮中心 (88, 1008)
        cur = (w//2, h//2)
        q.mouse_rel(88-cur[0], 1008-cur[1])
        time.sleep(0.4)
        q.screendump("f_probe.ppm")
        _, _, probe = load_ppm("f_probe.ppm")
        # 找光标新位置（排除旧位置簇）
        pts = [(x,y) for y in range(0,h,2) for x in range(0,w,2)
               if base[(y*w+x)*3:(y*w+x)*3+3] != probe[(y*w+x)*3:(y*w+x)*3+3]]
        cs = []
        for x,y in pts:
            for c in cs:
                if abs(c['x0']-x)<=30 and abs(c['y0']-y)<=30:
                    c['x0']=min(c['x0'],x);c['x1']=max(c['x1'],x)
                    c['y0']=min(c['y0'],y);c['y1']=max(c['y1'],y);c['n']+=1;break
            else:
                cs.append({'x0':x,'x1':x,'y0':y,'y1':y,'n':1})
        cs = [c for c in cs if not (abs((c['x0']+c['x1'])//2-cur[0])<=40 and abs((c['y0']+c['y1'])//2-cur[1])<=40)]
        cs.sort(key=lambda c:c['n'], reverse=True)
        if cs:
            # 光标热点在箭头左上角（gw_cursor_draw 以 mx,my 为左上角绘制），
            # 取簇的左上角而非中心，否则引入 (+4,+7) 系统性偏移
            cur = (cs[0]['x0'], cs[0]['y0'])
        print(f"  cursor at {cur}, clicking taskbar button")

        # 点击任务栏按钮 -> 最小化
        q.mouse_click(); time.sleep(1)
        q.screendump("f_min.ppm")
        _, _, minpx = load_ppm("f_min.ppm")
        term_white2 = count_color(minpx, w, h, 240, 240, 240)
        check("点击任务栏按钮最小化窗口", term_white2 < term_white // 4,
              f"(white {term_white} -> {term_white2})")

        # 再点击 -> 恢复
        q.mouse_click(); time.sleep(1)
        q.screendump("f_restore.ppm")
        _, _, rspx = load_ppm("f_restore.ppm")
        term_white3 = count_color(rspx, w, h, 240, 240, 240)
        check("再点击恢复窗口", term_white3 > term_white // 2,
              f"(white -> {term_white3})")

        # ===== Phase 3: 桌面图标双击启动井字棋（音效后渲染正常）=====
        print("== Phase 3: 桌面图标双击启动井字棋（含音效） ==")
        # 先最小化 Terminal 露出桌面图标（窗口会覆盖图标并截走点击）
        q.mouse_click(); time.sleep(1)
        q.screendump("f_min2.ppm")
        _, _, min2 = load_ppm("f_min2.ppm")
        term_white_m = count_color(min2, w, h, 240, 240, 240)
        check("最小化 Terminal 露出桌面", term_white_m < term_white // 4,
              f"(white -> {term_white_m})")
        # 双击 TicTac 图标（图标 6，行 1 列 2）
        icon_sz = int(w*40/640); box = icon_sz + 26; gap = int(w*8/640)
        x0 = int(w*8/640); y0 = int(h*10/480)
        ix = x0 + (6%4)*(box+gap); iy = y0 + (6//4)*(box+gap)
        target = (ix+box//2, iy+box//2)
        q.mouse_rel(target[0]-cur[0], target[1]-cur[1]); cur = target
        time.sleep(0.4)
        q.mouse_dclick(); time.sleep(1.5)
        q.screendump("f_tic.ppm")
        _, _, tic = load_ppm("f_tic.ppm")
        grid = count_color(tic, w, h, 96, 96, 96)
        check("双击图标启动井字棋（网格渲染+音效路径）", grid > 2000, f"(grid={grid})")
        q.type_text("5"); time.sleep(1.2)
        q.type_text("q"); time.sleep(0.8)
        q.type_text(" "); time.sleep(1)
        # 游戏退出时关闭宿主 Terminal 窗口，桌面无窗口无焦点

        # ===== Phase 4: 双击标题栏最大化/还原 =====
        print("== Phase 4: 双击标题栏最大化/还原 ==")
        q.type_text("t"); time.sleep(1.5)   # 无焦点热键重开 Terminal（干净提示符）
        # Terminal 窗口默认位置 (scale_x(30), scale_y(24))，标题栏高 12
        tw_x = int(w*30/640); tw_y = int(h*24/480)
        tb_pt = (tw_x + 150, tw_y + 6)
        q.mouse_rel(tb_pt[0]-cur[0], tb_pt[1]-cur[1]); cur = tb_pt
        time.sleep(0.4)
        q.screendump("f_tbase.ppm")
        _, _, tbase = load_ppm("f_tbase.ppm")
        white0 = count_color(tbase, w, h, 240, 240, 240)
        q.mouse_dclick(); time.sleep(1)
        q.screendump("f_max.ppm")
        _, _, maxpx = load_ppm("f_max.ppm")
        white1 = count_color(maxpx, w, h, 240, 240, 240)
        check("双击标题栏最大化（窗口铺满桌面）", white1 > white0 * 1.4,
              f"(white {white0} -> {white1})")
        # 最大化后标题栏贴屏幕顶部（y 0..12），再双击还原
        q.mouse_rel(0, 6 - tb_pt[1]); cur = (tb_pt[0], 6)
        time.sleep(0.3)
        q.mouse_dclick(); time.sleep(1)
        q.screendump("f_unmax.ppm")
        _, _, unmax = load_ppm("f_unmax.ppm")
        white2 = count_color(unmax, w, h, 240, 240, 240)
        check("再次双击标题栏还原窗口", abs(white2 - white0) < white0 * 0.3,
              f"(white {white1} -> {white2})")

        # ===== Phase 5: Ubuntu GNOME 主题 + 左侧 Dock =====
        print("== Phase 5: GNOME 主题 + Dock ==")
        # Terminal 焦点中且输入行干净：theme 6 切换 Ubuntu GNOME（顶栏 + 左侧 Dock）
        q.type_text("theme 6\n"); time.sleep(1.5)
        q.screendump("f_gnome.ppm")
        _, _, gnom = load_ppm("f_gnome.ppm")
        top_black = count_color(gnom, w, h, 0, 0, 0, tol=10, y0=0, y1=32)
        check("GNOME 顶部黑栏", top_black > 30000, f"(black={top_black})")
        # 点击顶栏 Terminal 任务按钮（x 92..172）最小化，露出 Dock
        q.mouse_rel(132 - cur[0], 16 - cur[1]); cur = (132, 16)
        time.sleep(0.4)
        q.mouse_click(); time.sleep(1)
        q.screendump("f_dock.ppm")
        _, _, dock = load_ppm("f_dock.ppm")
        dock_w = int(w*52/640)
        dark = count_color(dock, w, h, 30, 28, 28, x0=0, x1=dock_w, y0=32)
        check("Ubuntu Dock 渲染（左侧深色栏）", dark > 20000, f"(dark={dark})")
        blue = count_color(dock, w, h, 0, 120, 215, x0=0, x1=dock_w, y0=32)
        check("Dock 图标渲染（File Manager 蓝）", blue > 2000, f"(blue={blue})")
        orange = count_color(dock, w, h, 230, 84, 32, x0=0, x1=dock_w, y0=32)
        check("Dock 运行指示点（Terminal）", orange > 8, f"(orange={orange})")
        # 点击 Dock 第 1 个图标 -> 启动 File Manager
        icon_c = (int(dock_w*0.5), int(h*12/480) + 32 + int(h*48/480)//2)
        q.mouse_rel(icon_c[0]-cur[0], icon_c[1]-cur[1]); cur = icon_c
        time.sleep(0.4)
        q.mouse_click(); time.sleep(1.5)
        q.screendump("f_files.ppm")
        _, _, files = load_ppm("f_files.ppm")
        bg0 = count_color(dock, w, h, 247, 247, 247)
        bg1 = count_color(files, w, h, 247, 247, 247)
        check("点击 Dock 图标启动 File Manager", bg1 - bg0 > 150000,
              f"(bg {bg0} -> {bg1})")

        print(f"\n===== 结果: {len(PASS)} PASS / {len(FAIL)} FAIL =====")
        if FAIL:
            print("FAILED:", FAIL)
        return 0 if not FAIL else 1
    finally:
        proc.kill()

if __name__ == "__main__":
    sys.exit(main())
