# -*- coding: utf-8 -*-
"""test_step7_sock.py - 步骤 7.3 socket 层 E2E（宿主侧对端 + pcap 断言）

每个阶段独立拉起一个 QEMU（user-net + rtl8139 + hostfwd + filter-dump），
经 monitor sendkey 向 shell 键入 exec 命令，从数据盘启动用户态测试程序，
宿主用标准 socket 与 guest 对打：

  [udp] exec NETECHO.ELF ：宿主发 1 个 UDP 数据报到 127.0.0.1:7000，
                          断言原样回显；netecho 只回显一个数据报后退出
  [tcp] exec NETTCP.ELF  ：完整三次握手 + 单段数据回显 + 四次挥手
                          （guest 主动 FIN），宿主侧读到干净 EOF

pcap 断言（filter-dump 抓 guest 侧网线；guest 发出的帧即 EZOS 协议栈原作，
slirp 不会改写 guest→host 方向的帧内容）：
  - guest 应答过 ARP request（hostfwd 转发前 slirp 必先 ARP）
  - guest 全部分片的 IP 头校验和、TCP/UDP 伪首部校验和重算一致
  - TCP：SYN → SYN+ACK(ack=对端 ISN+1) → ACK 齐全；回显段 seq == ISN+1
    且 ack == 对端 seq+len；guest FIN 的 seq 正确；对端 ACK FIN →
    对端 FIN → guest 最终 ACK；全程无 RST

用法（在 src 根目录）：
    python tests/test_step7_sock.py           # 跑 udp + tcp 两个阶段
    python tests/test_step7_sock.py udp       # 只跑 UDP
    python tests/test_step7_sock.py tcp       # 只跑 TCP

前置：ninja -f build.ninja 已产出 os-image.bin / disk.img / user/*.elf
      （disk.img 由 gen_diskimg.py 生成，自动嵌入 NETECHO/NETTCP.ELF）
退出码 0 = 全部断言通过。
注：开机等待 BOOT_WAIT_S 是保守值；机器慢导致键入落空时把它调大。
"""
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
GUEST_IP = "10.0.2.15"
UDP_PORT = 7000
TCP_PORT = 7001
MON_PORT = 55555                 # QEMU human monitor（TCP；两阶段串行，复用无冲突）
BOOT_WAIT_S = 10.0               # 开机到 shell 提示符的保守等待
KEY_DELAY_S = 0.02               # 相邻 sendkey 的间隔

UDP_DGRAM = b"EZOS step7.3 UDP echo ping 0123456789 abcdefg"
TCP_PAYLOAD = b"EZOS step7.3 TCP echo: the quick brown fox jumps 0123456789\n"

TCP_FIN, TCP_SYN, TCP_RST, TCP_PSH, TCP_ACK = 0x01, 0x02, 0x04, 0x08, 0x10
M32 = 0xFFFFFFFF


# ==================== QEMU 驱动 ====================

def key_name(ch):
    """字符 → QEMU monitor sendkey 的键名（US 布局）"""
    if ch == " ":
        return "spc"
    if ch == "\n":
        return "ret"
    if ch == ".":
        return "dot"
    if ch == "-":
        return "minus"
    if ch == "/":
        return "slash"
    if ch == "_":
        return "shift-minus"
    if ch.isdigit():
        return ch
    if ch.isupper():
        return "shift-" + ch.lower()
    return ch


class QemuGuest(object):
    """无显示的 QEMU 实例：网卡 hostfwd + 抓包 + TCP monitor（sendkey 键入）"""

    def __init__(self, pcap):
        self.pcap = pcap
        self.proc = None
        self.mon = None

    def start(self):
        argv = [
            QEMU,
            "-icount", "shift=auto",       # 与 build.ninja run_qemu 一致（时钟近似实时）
            "-display", "none",
            "-drive", "format=raw,file=os-image.bin",
            "-drive", "format=raw,file=disk.img",
            "-netdev", "user,id=n0,hostfwd=tcp::%d-:%d,hostfwd=udp::%d-:%d"
                       % (TCP_PORT, TCP_PORT, UDP_PORT, UDP_PORT),
            "-device", "rtl8139,netdev=n0",
            "-object", "filter-dump,id=dump0,netdev=n0,file=" + self.pcap,
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        ]
        self.proc = subprocess.Popen(argv, cwd=ROOT,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        deadline = time.time() + 15
        while True:
            try:
                self.mon = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    self.quit()
                    raise AssertionError("QEMU monitor did not come up")
                time.sleep(0.2)
        self.mon.settimeout(0.05)
        self._drain(0.5)

    def _drain(self, timeout=0.05):
        self.mon.settimeout(timeout)
        try:
            while True:
                b = self.mon.recv(4096)
                if not b:
                    break
        except socket.timeout:
            pass

    def mon_cmd(self, cmd):
        self.mon.sendall((cmd + "\n").encode())
        time.sleep(KEY_DELAY_S)
        self._drain()

    def type_line(self, line):
        """把一行命令逐字符经 sendkey 敲进 guest 的 PS/2 键盘，回车结尾"""
        for ch in line:
            self.mon_cmd("sendkey " + key_name(ch))
        self.mon_cmd("sendkey ret")

    def quit(self):
        try:
            if self.mon:
                self.mon_cmd("quit")
        except OSError:
            pass
        if self.proc is not None:
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self.mon:
            try:
                self.mon.close()
            except OSError:
                pass
            self.mon = None


# ==================== pcap 解析与校验 ====================

def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def be32(b, o):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def ip_str(b):
    return "%d.%d.%d.%d" % tuple(b)


def load_pcap(path):
    """经典 libpcap（filter-dump 输出，linktype 1=EN10MB）→ 以太帧列表"""
    data = open(path, "rb").read()
    assert len(data) >= 24, "pcap too short"
    magic = data[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    elif magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    else:
        raise AssertionError("bad pcap magic: %r" % magic)
    link = struct.unpack(endian + "I", data[20:24])[0]
    assert link == 1, "expect ethernet linktype, got %d" % link
    frames = []
    off = 24
    while off + 16 <= len(data):
        _ts, _tu, incl, _orig = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        if off + incl > len(data):
            break
        frames.append(data[off:off + incl])
        off += incl
    return frames


def parse_ip(frame):
    """以太帧 → (截断到 IP 总长的 ip 字节, ihl, proto)；非 IPv4 返回 None"""
    if len(frame) < 14 or be16(frame, 12) != 0x0800:
        return None
    ip = frame[14:]
    if len(ip) < 20 or (ip[0] >> 4) != 4:
        return None
    ihl = (ip[0] & 0x0F) * 4
    if ihl < 20:
        return None
    tot = be16(ip, 2)
    if tot < ihl or tot > len(ip):
        return None
    return ip[:tot], ihl, ip[9]


def ip_csum_ok(ip, ihl):
    s = 0
    for o in range(0, ihl, 2):
        s += be16(ip, o)
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s == 0xFFFF


def l4_csum_ok(ip, ihl, proto):
    """含伪首部的 TCP/UDP 校验和重算（网络字节序逐 16 位求和）"""
    tot = be16(ip, 2)
    l4 = ip[ihl:tot]
    if len(l4) < 8:
        return False
    s = 0
    for o in (12, 14, 16, 18):          # src/dst IP
        s += be16(ip, o)
    s += proto + len(l4)
    data = l4 + (b"\x00" if len(l4) & 1 else b"")
    for o in range(0, len(data) - 1, 2):
        s += be16(data, o)
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s == 0xFFFF


def arp_reply_from_guest(frames):
    for f in frames:
        if len(f) < 42 or be16(f, 12) != 0x0806:
            continue
        a = f[14:]
        if be16(a, 6) == 2 and ip_str(a[14:18]) == GUEST_IP:
            return True
    return False


def analyze_udp(frames, payload):
    assert arp_reply_from_guest(frames), "no ARP reply from guest in pcap"
    print("  [OK] guest answered ARP request (hostfwd path resolved)")
    seen = 0
    for f in frames:
        d = parse_ip(f)
        if d is None:
            continue
        ip, ihl, proto = d
        if proto != 17 or ip_str(ip[12:16]) != GUEST_IP:
            continue
        u = ip[ihl:]
        assert be16(u, 0) == UDP_PORT, "guest UDP source port != %d" % UDP_PORT
        assert ip_csum_ok(ip, ihl), "guest IP header checksum bad"
        assert l4_csum_ok(ip, ihl, 17), "guest UDP checksum bad"
        if bytes(u[8:8 + len(payload)]) == payload:
            seen += 1
    assert seen >= 1, "guest UDP echo datagram not found in pcap"
    print("  [OK] guest UDP reply: checksums valid, payload matched")


def analyze_tcp(frames, payload):
    segs = []
    for f in frames:
        d = parse_ip(f)
        if d is None:
            continue
        ip, ihl, proto = d
        if proto != 6:
            continue
        t = ip[ihl:]
        if TCP_PORT not in (be16(t, 0), be16(t, 2)):
            continue
        segs.append({
            "guest": ip_str(ip[12:16]) == GUEST_IP,
            "seq": be32(t, 4),
            "ack": be32(t, 8),
            "flags": t[13],
            "payload": bytes(t[(t[12] >> 4) * 4:]),
            "ip": ip,
            "ihl": ihl,
        })
    assert segs, "no TCP segments on port %d in pcap" % TCP_PORT

    def find(pred, frm, what):
        for j in range(frm, len(segs)):
            if pred(segs[j]):
                return j
        raise AssertionError("pcap: missing " + what)

    # guest 发出的每个分片：IP + TCP 校验和重算一致
    n_guest = 0
    for s in segs:
        if not s["guest"]:
            continue
        n_guest += 1
        assert ip_csum_ok(s["ip"], s["ihl"]), "guest IP header checksum bad"
        assert l4_csum_ok(s["ip"], s["ihl"], 6), "guest TCP checksum bad"
    print("  [OK] guest IP+TCP checksums valid (%d segments)" % n_guest)

    # 三次握手
    j0 = find(lambda s: not s["guest"] and (s["flags"] & TCP_SYN)
              and not (s["flags"] & TCP_ACK), 0, "peer SYN")
    peer_isn = segs[j0]["seq"]
    j1 = find(lambda s: s["guest"] and (s["flags"] & TCP_SYN)
              and (s["flags"] & TCP_ACK), j0 + 1, "guest SYN+ACK")
    isn_g = segs[j1]["seq"]
    assert segs[j1]["ack"] == (peer_isn + 1) & M32, "SYN+ACK ack != peer ISN+1"
    find(lambda s: not s["guest"] and (s["flags"] & TCP_ACK)
         and not (s["flags"] & TCP_SYN), j1 + 1, "peer ACK (3rd handshake)")
    print("  [OK] three-way handshake (guest ISN=0x%08x)" % isn_g)

    # 数据回显：单段、seq/ack 连续
    j2 = find(lambda s: not s["guest"] and s["payload"] == payload,
              j1, "peer data segment")
    j3 = find(lambda s: s["guest"] and s["payload"] == payload,
              j2, "guest echo segment")
    assert segs[j3]["seq"] == (isn_g + 1) & M32, "guest echo seq != ISN+1"
    assert segs[j3]["ack"] == (peer_isn + 1 + len(payload)) & M32, \
        "guest echo ack != peer seq + len"
    print("  [OK] payload echoed in one segment, seq/ack continuous")

    # 四次挥手：guest 主动 FIN（nettcp 的 SC_CLOSE 路径）
    j4 = find(lambda s: s["guest"] and (s["flags"] & TCP_FIN), j3, "guest FIN")
    fin_seq = segs[j4]["seq"]
    assert fin_seq == (isn_g + 1 + len(payload)) & M32, "guest FIN seq wrong"
    find(lambda s: not s["guest"] and (s["flags"] & TCP_ACK)
         and s["ack"] == (fin_seq + 1) & M32, j4, "peer ACK of guest FIN")
    j5 = find(lambda s: not s["guest"] and (s["flags"] & TCP_FIN), j4, "peer FIN")
    peer_fin_seq = segs[j5]["seq"]
    find(lambda s: s["guest"] and (s["flags"] & TCP_ACK)
         and s["ack"] == (peer_fin_seq + 1) & M32, j5,
         "guest final ACK (four-way teardown complete)")
    print("  [OK] four-way teardown complete (guest FIN -> ACK -> peer FIN -> ACK)")

    for s in segs:
        assert not (s["flags"] & TCP_RST), "RST seen - connection aborted"
    print("  [OK] no RST in the whole session")


# ==================== 两个阶段 ====================

def phase_udp():
    print("== phase udp: exec NETECHO.ELF ==")
    pcap = os.path.join(ROOT, "tests", "netdump_udp.pcap").replace("\\", "/")
    if os.path.isfile(pcap):
        os.remove(pcap)
    q = QemuGuest(pcap)
    q.start()
    try:
        time.sleep(BOOT_WAIT_S)
        q.type_line("exec NETECHO.ELF")
        time.sleep(3.0)               # exec 装载 + bind + 进入 recvfrom
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.settimeout(8)
        reply = None
        for attempt in range(3):      # guest 未就绪时首发可能落空，重试
            u.sendto(UDP_DGRAM, ("127.0.0.1", UDP_PORT))
            try:
                reply, _addr = u.recvfrom(2048)
                break
            except socket.timeout:
                print("  (retry %d: no UDP reply yet)" % (attempt + 1))
        u.close()
        assert reply == UDP_DGRAM, "UDP echo mismatch: %r" % (reply,)
        print("  [OK] UDP datagram echoed to host")
    finally:
        q.quit()
    analyze_udp(load_pcap(pcap), UDP_DGRAM)


def phase_tcp():
    print("== phase tcp: exec NETTCP.ELF ==")
    pcap = os.path.join(ROOT, "tests", "netdump_tcp.pcap").replace("\\", "/")
    if os.path.isfile(pcap):
        os.remove(pcap)
    q = QemuGuest(pcap)
    q.start()
    try:
        time.sleep(BOOT_WAIT_S)
        q.type_line("exec NETTCP.ELF")
        time.sleep(3.0)
        s = None
        last_err = None
        for _ in range(10):           # guest accept 窗口 15s：早到被拒则重试
            try:
                s = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=3)
                break
            except OSError as e:
                last_err = e
                time.sleep(1.0)
        assert s is not None, "cannot connect guest TCP %d: %s" % (TCP_PORT, last_err)
        print("  [OK] connected (handshake completed)")
        s.settimeout(25)
        s.sendall(TCP_PAYLOAD)
        echo = b""
        while len(echo) < len(TCP_PAYLOAD):
            chunk = s.recv(4096)
            if not chunk:
                break
            echo += chunk
        assert echo == TCP_PAYLOAD, "TCP echo mismatch: %r" % (echo,)
        print("  [OK] payload echoed")
        tail = s.recv(1)              # guest FIN -> 宿主侧读到干净 EOF
        assert tail == b"", "expected EOF after guest FIN, got %r" % (tail,)
        s.close()
        print("  [OK] guest closed cleanly (FIN, host saw EOF)")
        # 给 guest 1.5s 处理我方 FIN 并回最终 ACK——close 后立刻 quit
        # 会在 guest 的 ACK 上网线之前杀掉 QEMU（pcap 缺最后一帧的竞态）
        time.sleep(1.5)
    finally:
        q.quit()
    frames = load_pcap(pcap)
    analyze_tcp(frames, TCP_PAYLOAD)
    assert arp_reply_from_guest(frames), "no ARP reply from guest in pcap"
    print("  [OK] guest answered ARP request (hostfwd path resolved)")


def main():
    only = (sys.argv[1] if len(sys.argv) > 1 else "all").lower()
    assert os.path.isfile(QEMU), "QEMU not found: %s" % QEMU
    for pre in ("os-image.bin", "disk.img"):
        assert os.path.isfile(os.path.join(ROOT, pre)), pre + " missing - run ninja first"
    for elf in ("netecho.elf", "nettcp.elf"):
        if not os.path.isfile(os.path.join(ROOT, "user", elf)):
            print("WARN user/%s missing - disk.img may lack it (run ninja)" % elf)
    if only in ("all", "udp"):
        phase_udp()
    if only in ("all", "tcp"):
        phase_tcp()
    print("PASS: step 7.3 socket E2E - all assertions passed")


if __name__ == "__main__":
    main()
