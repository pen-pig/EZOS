# -*- coding: utf-8 -*-
"""test_netapp.py - 网络上层演示（ping + httpd）E2E

  [ping]  guest 跑 `ping 10.0.2.2`：用 `-netdev socket` 直连 Python 对端
          （绕过 slirp，完全掌控 ICMP），对端应答 ARP 并对每个 echo request
          回一个同 id/seq 的 echo reply；断言收到 4 个 request 且 guest 屏幕
          上打出 ≥3 条 reply（rtt 有效）、无 timeout、无 panic
  [httpd] guest 跑 `httpd`（slirp + hostfwd 8080→80）：宿主连上去发 GET，
          断言 200 响应含 EZOS 页面，guest 主动 FIN；屏幕出现 "served"

端口：ping netdev 4477 / monitor 55668；httpd hostfwd 8080 / monitor 55669。
用法：python tests/test_netapp.py    （退出码 0 = 全部通过）
"""
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ezocr  # noqa: E402

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
BOOT_WAIT_S = 10.0

GUEST_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
GUEST_IP = bytes([10, 0, 2, 15])
HOST_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x02])
HOST_IP = bytes([10, 0, 2, 2])

FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10


def key_name(ch):
    if ch == " ":
        return "spc"
    if ch == ".":
        return "dot"
    if ch == "/":
        return "slash"
    if ch.isdigit():
        return ch
    if ch.isupper():
        return "shift-" + ch.lower()
    return ch


class Guest(object):
    """QEMU 实例：monitor 键入 + 可选 netdev socket / hostfwd"""

    def __init__(self, net_port=None, mon_port=None, extra_net=()):
        self.net_port = net_port
        self.mon_port = mon_port
        self.extra_net = list(extra_net)
        self.proc = None
        self.mon = None
        self.net = None
        self.buf = b""

    def start(self):
        argv = [QEMU, "-icount", "shift=auto", "-display", "none",
                "-drive", "format=raw,file=os-image.bin",
                "-drive", "format=raw,file=disk.img"] + self.extra_net
        if self.net_port is not None:
            argv += ["-netdev", "socket,id=n0,listen=127.0.0.1:%d" % self.net_port,
                     "-device", "rtl8139,netdev=n0"]
        argv += ["-monitor", "tcp:127.0.0.1:%d,server,nowait" % self.mon_port]
        self.proc = subprocess.Popen(argv, cwd=ROOT,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        dl = time.time() + 15
        while True:
            try:
                self.mon = socket.create_connection(("127.0.0.1", self.mon_port), timeout=2)
                break
            except OSError:
                if time.time() > dl:
                    self.quit()
                    raise AssertionError("monitor did not come up")
                time.sleep(0.2)
        self.mon.settimeout(0.05)
        self._drain()
        if self.net_port is not None:
            dl = time.time() + 15
            while True:
                try:
                    self.net = socket.create_connection(("127.0.0.1", self.net_port), timeout=2)
                    break
                except OSError:
                    if time.time() > dl:
                        self.quit()
                        raise AssertionError("netdev socket did not accept")
                    time.sleep(0.2)
            self.net.settimeout(0.2)

    def _drain(self):
        try:
            while True:
                if not self.mon.recv(4096):
                    break
        except socket.timeout:
            pass

    def mon_cmd(self, cmd):
        self.mon.sendall((cmd + "\n").encode())
        time.sleep(0.02)
        self._drain()

    def type_line(self, line):
        for ch in line:
            self.mon_cmd("sendkey " + key_name(ch))
        self.mon_cmd("sendkey ret")

    def screen_text(self, shot):
        self.mon_cmd("screendump " + shot)
        time.sleep(0.8)
        with open(os.path.join(ROOT, shot), "rb") as f:
            head = f.read(64).split(b"\n", 2)[1].decode()
        if head != "720 400":
            return ""
        return ezocr.ocr_text(os.path.join(ROOT, shot), os.path.join(ROOT, "kernel"))

    def send_frame(self, frame):
        if len(frame) < 60:                    # 以太网最小帧长，runt 会被丢
            frame = frame + b"\x00" * (60 - len(frame))
        self.net.sendall(struct.pack(">I", len(frame)) + frame)

    def recv_frame(self, timeout=0.4):
        self.net.settimeout(timeout)
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if 14 <= n <= 65535:
                    if len(self.buf) >= 4 + n:
                        frame = self.buf[4:4 + n]
                        self.buf = self.buf[4 + n:]
                        return frame
                else:                          # 裸帧模式兜底
                    frame = self.buf
                    self.buf = b""
                    return frame
            try:
                chunk = self.net.recv(65536)
                if not chunk:
                    return None
                self.buf += chunk
            except socket.timeout:
                return None

    def quit(self):
        try:
            if self.mon:
                self.mon_cmd("quit")
        except OSError:
            pass
        for s in (self.net, self.mon):
            try:
                if s:
                    s.close()
            except OSError:
                pass
        if self.proc is not None:
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def cksum(data):
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) | data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def arp_reply():
    a = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2)
    return GUEST_MAC + HOST_MAC + b"\x08\x06" + a + HOST_MAC + HOST_IP + \
           GUEST_MAC + GUEST_IP


def icmp_reply(req_ip, req_ic):
    """从 echo request 构造 reply：换类型、换 IP 方向、重算两个校验和。
    注意 IP 总长 = 本包自己的头(20) + ICMP 长度——最初写成
    len(req_ip)+len(ic)（把请求的载荷算了两遍），guest 按总长校验直接丢帧。"""
    ic = bytes([0, 0]) + req_ic[2:]                       # type 0, 其余原样
    ic = ic[:2] + struct.pack(">H", cksum(ic)) + ic[4:]
    total = 20 + len(ic)
    ip = struct.pack(">BBHHHBBH", 0x45, 0, total, 0x4001, 0, 64, 1, 0) + \
         HOST_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return GUEST_MAC + HOST_MAC + b"\x08\x00" + ip + ic


# ==================== phase ping ====================

def phase_ping():
    print("== phase ping: guest `ping 10.0.2.2` ==")
    g = Guest(net_port=4477, mon_port=55668)
    g.start()
    try:
        time.sleep(BOOT_WAIT_S)
        g.type_line("ping 10.0.2.2")
        t0 = time.time()
        nreq = 0
        arp_done = False
        while time.time() - t0 < 20:
            f = g.recv_frame(0.4)
            if f is None:
                continue
            if len(f) < 34:
                continue
            et = struct.unpack(">H", f[12:14])[0]
            if et == 0x0806:
                a = f[14:]
                if len(a) >= 28 and struct.unpack(">H", a[6:8])[0] == 1 \
                        and a[24:28] == HOST_IP:
                    g.send_frame(arp_reply())
                    arp_done = True
                continue
            if et != 0x0800:
                continue
            ip = f[14:]
            if len(ip) < 20 or ip[9] != 1:
                continue
            ihl = (ip[0] & 0x0F) * 4
            ic = ip[ihl:]
            if len(ic) >= 8 and ic[0] == 8 and ic[1] == 0:
                g.send_frame(icmp_reply(ip, ic))
                nreq += 1
        assert arp_done, "guest never ARP-requested the peer"
        assert nreq >= 4, "expected 4 echo requests, got %d" % nreq
        print("  [OK] answered ARP + %d ICMP echo requests" % nreq)
        time.sleep(1.5)
        txt = g.screen_text("temp/netapp_ping.ppm").lower()
        assert "panic" not in txt, "kernel panic on screen"
        replies = txt.count("reply from")
        assert replies >= 3, "expected >=3 reply lines on screen, got %d:\n%s" % (replies, txt)
        assert "request timeout" not in txt, "guest printed timeout despite replies"
        print("  [OK] guest printed %d replies (ICMP round-trip works)" % replies)
    finally:
        g.quit()


# ==================== phase httpd ====================

def phase_httpd():
    print("== phase httpd: guest `httpd` + host GET ==")
    g = Guest(mon_port=55669,
              extra_net=["-netdev", "user,id=n0,hostfwd=tcp::8080-:80",
                         "-device", "rtl8139,netdev=n0"])
    g.start()
    try:
        time.sleep(BOOT_WAIT_S)
        g.type_line("httpd")
        s = None
        last = None
        for _ in range(12):                 # guest 进 accept 需要几秒
            try:
                s = socket.create_connection(("127.0.0.1", 8080), timeout=3)
                break
            except OSError as e:
                last = e
                time.sleep(1.0)
        assert s is not None, "cannot connect guest httpd: %s" % last
        print("  [OK] connected (slirp -> guest:80)")
        s.settimeout(10)
        s.sendall(b"GET / HTTP/1.0\r\nHost: ezos\r\n\r\n")
        data = b""
        while len(data) < 4096:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        assert data.startswith(b"HTTP/1.0 200 OK"), \
            "bad response head: %r" % data[:40]
        body = data.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in data else b""
        assert b"EZOS" in body and b"i686" in body, "unexpected body: %r" % body[:120]
        print("  [OK] HTTP 200 + EZOS page (%d bytes)" % len(data))
        time.sleep(1.5)                     # 给 guest 时间处理我方 ACK/FIN
        txt = g.screen_text("temp/netapp_httpd.ppm").lower()
        assert "panic" not in txt, "kernel panic on screen"
        assert "served" in txt, "guest did not print 'served':\n%s" % txt
        print("  [OK] guest printed 'served'")
        s.close()
    finally:
        g.quit()


def main():
    for pre in ("os-image.bin", "disk.img"):
        assert os.path.isfile(os.path.join(ROOT, pre)), pre + " missing - run ninja"
    assert os.path.isfile(QEMU), "QEMU not found"
    phase_ping()
    phase_httpd()
    print("PASS: net app E2E (ping + httpd)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
