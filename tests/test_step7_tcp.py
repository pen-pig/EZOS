# -*- coding: utf-8 -*-
"""test_step7_tcp.py - 步骤 7.4 TCP 可靠性 E2E（主动 connect / 乱序重组 / 重传）

为什么不用 slirp（user-net）：slirp 会把 guest 的包按标准栈回 ACK，
没法制造"对端不 ACK"和"乱序到达"这两种场景。这里用 `-netdev socket`
把 guest 网卡直接接到 Python 对端（4 字节大端长度前缀 + 以太帧），
帧级完全由脚本编排：

  [connect]  guest SYN → 我们回 SYN+ACK → guest ACK，断言主动打开完成
  [reasm]    故意**后发前一段、先发后一段**（seq 逆序）：
             先 seq=N+4 "WORLD"，再 seq=N "HELL"。
             guest 必须把先到的乱序段缓存起来，等缺口补上后按序交给应用；
             应用把收到的内容回显，我们断言回显恰好是 "HELLWORLD"
  [rtx]      对端故意不 ACK guest 的回显段，断言同一 seq 的段出现 ≥2 次
             （重传定时器真的在工作），随后再 ACK 让它停下

用法：python tests/test_step7_tcp.py        （退出码 0 = 全部断言通过）
前置：ninja 已产出 os-image.bin / disk.img（disk.img 需含 NETCLI.ELF）
端口：netdev 4476（QEMU listen，脚本连），monitor 55666（sendkey 键入）
"""
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
NET_PORT = 4476
MON_PORT = 55666
BOOT_WAIT_S = 10.0

GUEST_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
GUEST_IP = bytes([10, 0, 2, 15])
HOST_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x02])
HOST_IP = bytes([10, 0, 2, 2])
HOST_PORT = 7002

FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10
M32 = 0xFFFFFFFF

ECHO_MUST_BE = b"HELLWORLD"        # "HELL" 后到 + "WORLD" 先到 → 重组后必须这样


# ==================== 帧构造/解析 ====================

def cksum(data):
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) | data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


_ip_id = 0x2000


def tcp_frame(src_ip, dst_ip, sport, dport, seq, ack, flags, payload=b""):
    global _ip_id
    tcp = struct.pack(">HHIIBBHHH", sport, dport, seq, ack,
                      (5 << 4), flags, 4096, 0, 0) + payload
    pseudo = src_ip + dst_ip + bytes([0, 6]) + struct.pack(">H", len(tcp))
    raw = pseudo + tcp + (b"\x00" if len(tcp) & 1 else b"")
    tcp = tcp[:16] + struct.pack(">H", cksum(raw)) + tcp[18:]

    _ip_id = (_ip_id + 1) & 0xFFFF
    total = 20 + len(tcp)
    ip = struct.pack(">BBHHHBBH", 0x45, 0, total, _ip_id, 0, 64, 6, 0) + \
         src_ip + dst_ip
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return GUEST_MAC + HOST_MAC + b"\x08\x00" + ip + tcp   # dst=guest, src=host


def arp_reply():
    # ARP 头：HTYPE(2) PTYPE(2) HLEN(1) PLEN(1) OPER(2)
    a = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2)
    # 以太网头是 (dst, src)：dst 必须是 guest MAC，否则 RTL8139 的物理匹配
    # 过滤（RCR.APM + AB）直接丢帧——踩过：写反后 guest 一帧都收不到，
    # 只有"收到 IPv4 帧就学习源 MAC"的兜底逻辑会掩盖它。
    return GUEST_MAC + HOST_MAC + b"\x08\x06" + a + HOST_MAC + HOST_IP + \
           GUEST_MAC + GUEST_IP


def parse(frame):
    """→ (proto, ip_hdr, tcp_hdr, payload) 或非 IP/TCP 返回 None"""
    if len(frame) < 34:
        return None
    et = struct.unpack(">H", frame[12:14])[0]
    if et == 0x0806:
        return ("arp", frame[14:], None, b"")
    if et != 0x0800:
        return None
    ip = frame[14:]
    if len(ip) < 20 or ip[9] != 6:
        return ("ip", ip, None, b"")
    ihl = (ip[0] & 0x0F) * 4
    t = ip[ihl:]
    doff = (t[12] >> 4) * 4
    return ("tcp", ip, t, t[doff:])


# ==================== QEMU 驱动 ====================

def key_name(ch):
    if ch == " ":
        return "spc"
    if ch == ".":
        return "dot"
    if ch.isdigit():
        return ch
    if ch.isupper():
        return "shift-" + ch.lower()
    return ch


class Guest(object):
    def __init__(self):
        self.proc = None
        self.net = None
        self.mon = None
        self.buf = b""

    def start(self):
        self.proc = subprocess.Popen([
            QEMU, "-icount", "shift=auto", "-display", "none",
            "-drive", "format=raw,file=os-image.bin",
            "-drive", "format=raw,file=disk.img",
            # QEMU 要求 listen=host:port（只给端口号会报 "doesn't contain ':'"）
            "-netdev", "socket,id=n0,listen=127.0.0.1:%d" % NET_PORT,
            "-device", "rtl8139,netdev=n0",
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        dl = time.time() + 15
        while True:                      # monitor（先起来）
            try:
                self.mon = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
                break
            except OSError:
                if time.time() > dl:
                    self.quit()
                    raise AssertionError("monitor did not come up")
                time.sleep(0.2)
        self.mon.settimeout(0.05)
        self._drain()

        dl = time.time() + 15            # netdev（QEMU listen，我们连）
        while True:
            try:
                self.net = socket.create_connection(("127.0.0.1", NET_PORT), timeout=2)
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

    def send_frame(self, frame):
        """按当前判定的分帧方式发送；首帧之前默认带长度前缀（QEMU 常用）。
        以太网最小帧长 60 字节（不含 CRC）：ARP 应答 42B、纯 ACK 54B 都是
        runt，不补齐会被对端/交换机直接丢掉——实测不补的话收不到 guest 的 SYN。"""
        if len(frame) < 60:
            frame = frame + b"\x00" * (60 - len(frame))
        self.net.sendall(struct.pack(">I", len(frame)) + frame)

    def recv_frame(self, timeout=0.5):
        """返回一个以太帧；超时返回 None。
        分帧自适应：QEMU 的 socket netdev 在某些版本带 4 字节大端长度前缀，
        某些版本直接吐裸帧——用"前缀值是否像一个合法帧长"来判断，两种都能跑。"""
        self.net.settimeout(timeout)
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if 14 <= n <= 65535:
                    if len(self.buf) >= 4 + n:
                        frame = self.buf[4:4 + n]
                        self.buf = self.buf[4 + n:]
                        return frame
                else:                       # 裸帧模式：一次 recv 就是一帧
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


# ==================== 主流程 ====================

def main():
    for pre in ("os-image.bin", "disk.img"):
        assert os.path.isfile(os.path.join(ROOT, pre)), pre + " missing - run ninja"
    assert os.path.isfile(QEMU), "QEMU not found"

    g = Guest()
    g.start()
    ok = []
    try:
        time.sleep(BOOT_WAIT_S)
        g.type_line("exec NETCLI.ELF")

        our_isn = 0x5000
        state = "wait_syn"
        guest_port = None
        guest_isn = None
        got_req = False
        echo_copies = []          # (seq, payload)
        acked_echo = False
        deadline = time.time() + 40
        sent_ooo = False
        arp_done = False

        while time.time() < deadline:
            f = g.recv_frame(0.4)
            if f is None:
                # 空闲时推进脚本动作：握手完成后立刻发乱序数据
                if state == "established" and not sent_ooo:
                    g.send_frame(tcp_frame(HOST_IP, GUEST_IP, HOST_PORT, guest_port,
                                           our_isn + 1 + 4, guest_isn + 1, ACK, b"WORLD"))
                    g.send_frame(tcp_frame(HOST_IP, GUEST_IP, HOST_PORT, guest_port,
                                           our_isn + 1, guest_isn + 1, ACK | PSH, b"HELL"))
                    sent_ooo = True
                    print("  [OK] sent out-of-order data (WORLD@N+4 first, HELL@N after)")
                continue

            p = parse(f)
            if p is None:
                continue
            kind, ip, tcp, payload = p

            if kind == "arp":
                a = ip
                if len(a) >= 28 and struct.unpack(">H", a[6:8])[0] == 1 and a[24:28] == HOST_IP:
                    g.send_frame(arp_reply())
                    if not arp_done:
                        arp_done = True
                        print("  [OK] guest ARP-requested the peer (active connect path)")
                continue

            if kind == "tcp" and tcp is not None:
                sport, dport = struct.unpack(">HH", tcp[0:4])
                seq, ack_no = struct.unpack(">II", tcp[4:12])
                flags = tcp[13]

                if flags & RST:
                    raise AssertionError("guest sent RST (flags=0x%02x)" % flags)

                if state == "wait_syn" and (flags & SYN) and not (flags & ACK) \
                        and dport == HOST_PORT:
                    guest_isn, guest_port = seq, sport
                    g.send_frame(tcp_frame(HOST_IP, GUEST_IP, HOST_PORT, guest_port,
                                           our_isn, seq + 1, SYN | ACK))
                    state = "wait_ack"
                    print("  [OK] guest SYN -> active connect (guest ISN=0x%08x)" % seq)
                    continue

                if state == "wait_ack" and (flags & ACK) and not (flags & SYN) \
                        and ack_no == (our_isn + 1) & M32:
                    state = "established"
                    print("  [OK] three-way handshake completed")
                    continue

                if state == "established":
                    if payload == b"REQ":
                        g.send_frame(tcp_frame(HOST_IP, GUEST_IP, HOST_PORT, guest_port,
                                               our_isn + 1 + 9, seq + len(payload), ACK))
                        got_req = True
                        continue
                    if payload and not acked_echo:
                        echo_copies.append((seq, payload))
                        # 故意不 ACK：逼出重传。等到看到 2 份（或 3.5s）再 ACK。
                        if len(echo_copies) >= 2 or \
                           (echo_copies and time.time() > deadline - 15):
                            g.send_frame(tcp_frame(HOST_IP, GUEST_IP, HOST_PORT, guest_port,
                                                   our_isn + 1 + 9,
                                                   (seq + len(payload)) & M32, ACK))
                            acked_echo = True
                        continue

            if acked_echo and echo_copies:
                break

        # ---- 断言 ----
        assert arp_done, "guest never ARP-requested the peer"
        assert state == "established", "handshake never completed (state=%s)" % state
        assert got_req, "guest request segment not seen"
        assert echo_copies, "guest never echoed the reassembled data"
        got = echo_copies[0][1]
        assert got == ECHO_MUST_BE, \
            "out-of-order reassembly wrong: got %r, want %r" % (got, ECHO_MUST_BE)
        print("  [OK] guest delivered out-of-order data in order: %r" % got)
        same = [s for s, _ in echo_copies].count(echo_copies[0][0])
        assert same >= 2, \
            "no retransmission: same seq seen %d time(s) (copies=%d)" % (same, len(echo_copies))
        print("  [OK] guest retransmitted unacked segment (%d copies, seq=0x%08x)"
              % (same, echo_copies[0][0]))
        ok.append(True)
    finally:
        g.quit()

    print("PASS: step 7.4 TCP reliability (connect / reorder / retransmit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
