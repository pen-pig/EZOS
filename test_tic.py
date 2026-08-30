# test_tic.py - QMP 自动化：desktop -> 单击 TicTac 图标 -> 井字棋渲染验证
# 用 QMP input-send-event 发鼠标事件（比 HMP mouse_move 可靠），带光标定位重试
import socket, json, time, subprocess, sys, os
from collections import Counter

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"

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
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.f.readline()
        return json.loads(line) if line.strip() else None

    def cmd(self, execute, **args):
        obj = {"execute": execute}
        if args: obj["arguments"] = args
        self.f.write((json.dumps(obj) + "\n").encode())
        self.f.flush()
        # 读到 return/error 为止
        while True:
            resp = self._read()
            if resp is None: return None
            if "return" in resp or "error" in resp:
                if "error" in resp: print(f"  [QMP err] {execute}: {resp['error']}")
                return resp

    def hmp(self, cmdline):
        return self.cmd("human-monitor-command", **{"command-line": cmdline}).get("return", "")

    def sendkey(self, keys, hold=40):
        evs = []
        for k in keys:
            evs.append({"type":"key","data":{"down":True,"key":{"type":"qcode","data":k}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(hold/1000)
        evs = []
        for k in keys:
            evs.append({"type":"key","data":{"down":False,"key":{"type":"qcode","data":k}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(hold/1000)

    KEYMAP = {'a':'a','b':'b','c':'c','d':'d','e':'e','x':'x','i':'i','t':'t',
              's':'s','k':'k','o':'o','p':'p','q':'q','5':'5',' ':'spc',
              '\n':'ret','\r':'ret'}
    def type_text(self, text, delay=0.05):
        for ch in text:
            kc = self.KEYMAP.get(ch)
            if kc is None:
                print(f"  [warn] no keymap for {ch!r}"); continue
            self.sendkey([kc], 30)
            time.sleep(delay)

    def mouse_rel(self, dx, dy, step=50, gap=0.03):
        import math
        n = max(1, math.ceil(max(abs(dx), abs(dy)) / step))
        sx, sy = dx / n, dy / n
        ax, ay = round(sx), round(sy)
        for _ in range(n):
            evs = [{"type":"rel","data":{"axis":"x","value":ax}},
                   {"type":"rel","data":{"axis":"y","value":ay}}]
            self.cmd("input-send-event", events=evs)
            time.sleep(gap)

    def mouse_click(self):
        self.cmd("input-send-event", events=[{"type":"btn","data":{"down":True,"button":"left"}}])
        time.sleep(0.12)
        self.cmd("input-send-event", events=[{"type":"btn","data":{"down":False,"button":"left"}}])
        time.sleep(0.15)

    def screendump(self, name):
        self.cmd("screendump", **{"filename": name} if False else {"arguments": {}}) if False else None
        # screendump 参数名是 filename（QMP）
        self.hmp(f"screendump {name}")
        time.sleep(0.4)

def load_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def find_cursor(prev_px, cur_px, w, h, old_pos):
    """diff 相邻两帧找光标新位置：排除旧位置簇，返回新位置簇中心"""
    pts = []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            i = (y*w + x)*3
            if prev_px[i:i+3] != cur_px[i:i+3]:
                pts.append((x, y))
    if not pts: return None
    cs = []
    for x, y in pts:
        for c in cs:
            if abs(c['x0']-x) <= 30 and abs(c['y0']-y) <= 30:
                c['x0']=min(c['x0'],x); c['x1']=max(c['x1'],x)
                c['y0']=min(c['y0'],y); c['y1']=max(c['y1'],y); c['n']+=1
                break
        else:
            cs.append({'x0':x,'x1':x,'y0':y,'y1':y,'n':1})
    # 排除旧位置附近的簇（光标擦除），剩下的最大簇即新位置
    def near_old(c):
        cx, cy = (c['x0']+c['x1'])//2, (c['y0']+c['y1'])//2
        return abs(cx-old_pos[0]) <= 40 and abs(cy-old_pos[1]) <= 40
    cs = [c for c in cs if not near_old(c)]
    if not cs: return None
    cs.sort(key=lambda c: c['n'], reverse=True)
    c = cs[0]
    return ((c['x0']+c['x1'])//2, (c['y0']+c['y1'])//2, c['n'])

def count_color(px, w, h, r, g, b, tol=14):
    n = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            i = (y*w + x)*3
            if abs(px[i]-r)<=tol and abs(px[i+1]-g)<=tol and abs(px[i+2]-b)<=tol:
                n += 1
    return n * 4  # 1/4 采样还原

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
        q.type_text("exit\n"); time.sleep(1.5)
        q.type_text("desktop\n"); time.sleep(3)
        q.screendump("shot_desktop.ppm")
        w, h, base = load_ppm("shot_desktop.ppm")
        print(f"[STEP] desktop {w}x{h}")
        if w < 640:
            print("FAIL: 未进入图形模式"); return 1

        # TicTac 图标 idx5: 1280x1024 下 rect(122,143,106,106), 中心(175,196)
        # 通用计算: icon rect 随分辨率缩放
        sx, sy = w/640, h/480
        icon_sz = int(w*40/640); box = icon_sz + 26; gap = int(w*8/640)
        x0 = int(w*8/640); y0 = int(h*10/480)
        idx = 5; row, col = idx//4, idx%4
        ix = x0 + col*(box+gap); iy = y0 + row*(box+gap)
        target = (ix + box//2, iy + box//2)
        print(f"[STEP] TicTac icon rect ({ix},{iy},{box},{box}), target {target}")

        cur_pos = (w//2, h//2)  # 桌面启动光标居中
        prev_probe = base
        clicked = False
        for attempt in range(8):
            dx, dy = target[0]-cur_pos[0], target[1]-cur_pos[1]
            in_icon = (ix+8 <= cur_pos[0] <= ix+box-8 and iy+8 <= cur_pos[1] <= iy+box-8)
            if abs(dx) <= 12 and abs(dy) <= 12 or (in_icon and attempt >= 2):
                q.mouse_click()
                time.sleep(1.5)
                q.screendump("shot_tic_empty.ppm")
                _, _, px = load_ppm("shot_tic_empty.ppm")
                bg = count_color(px, w, h, 232, 232, 232)
                grid = count_color(px, w, h, 96, 96, 96)
                print(f"[CLICK] attempt {attempt}: board bg={bg} grid={grid}")
                if grid > 100:
                    clicked = True; break
                # 未检测到棋盘，可能没点中——重试
                cur_pos = (w//2, h//2)
                continue
            print(f"[MOVE] attempt {attempt}: {cur_pos} -> {target} (d={dx},{dy})")
            q.mouse_rel(dx, dy)
            time.sleep(0.3)
            q.screendump("shot_probe.ppm")
            _, _, probe = load_ppm("shot_probe.ppm")
            newpos = find_cursor(prev_probe, probe, w, h, cur_pos)
            prev_probe = probe
            if newpos and newpos[2] >= 20:
                cur_pos = (newpos[0], newpos[1])
                print(f"  cursor now at {cur_pos}")
            else:
                print("  cursor not detected (no move?)")
        if not clicked:
            print("FAIL: 无法点击 TicTac 图标")
            return 1

        # 落子：按 5（中心）
        q.type_text("5"); time.sleep(1.5)
        q.screendump("shot_tic_move.ppm")
        _, _, mv = load_ppm("shot_tic_move.ppm")
        grid2 = count_color(mv, w, h, 96, 96, 96)
        bg2 = count_color(mv, w, h, 232, 232, 232)
        diffn = sum(1 for y in range(0,h,4) for x in range(0,w,4)
                    if mv[(y*w+x)*3:(y*w+x)*3+3] != base[(y*w+x)*3:(y*w+x)*3+3])
        print(f"[MOVE5] grid={grid2} bg={bg2} vs-desktop-diff={diffn*16}")

        # 退出：q 退出对局（显示终局画面），再按任意键返回桌面
        q.type_text("q"); time.sleep(1)
        q.screendump("shot_tic_end.ppm")
        q.type_text(" "); time.sleep(1.5)
        q.screendump("shot_tic_quit.ppm")

        ok = grid2 > 100 and bg2 > 2000
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        proc.kill()

if __name__ == "__main__":
    sys.exit(main())
