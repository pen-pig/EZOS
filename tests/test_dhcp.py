# -*- coding: utf-8 -*-
"""test_dhcp.py - DHCP 客户端 E2E（RFC 2131 简化：D/O/R/A）

判定走**串口（COM1）**按子串断言（沿用 nvme/sysvol 那套），协议字段走
Python 侧的自建 DHCP 服务器（`-netdev socket` 直连，完全掌控报文）。
端口 A 4521(QMP)/4522(serial)/4527(netdev)、B 4523/4524/4528、C 4525/4526/4529。

要证明的三件事（缺一件就算没做成）：
  1. 报文格式对：DISCOVER 源 0.0.0.0、目的 255.255.255.255、UDP 68->67、
     magic cookie 对、**IP 与 UDP 校验和都对**（错一个字节对端就丢弃，
     所以必须算，不能"反正内核不校验"）
  2. 状态机真的走了两轮：REQUEST 的 xid 与 DISCOVER 相同，option 50 =
     服务器下发的 IP、option 54 = 服务器 ID、option 61 = 01+MAC
  3. 结果真的应用了：屏幕打出 got/mask/router/dns/lease，`nic` 也认，
     并且换到新地址后 ping 网关真的通（不是只改了个变量）

铁律「两组结果必须相反」+「警惕弱断言」：
  A（正常应答）：上面的 1-3 全成立，且**绝不**出现 "no response"/"NAK"
  B（服务器静默）：必须打 "DHCP: no response (timeout)"，
     **绝不**出现 "DHCP: got"，且 nic 里 IP 仍是缺省 10.0.2.15
     —— 证明"成功"不是打印逻辑恒真
  C（REQUEST 被 NAK）：必须打 "DHCP: NAK"，**绝不**出现 "DHCP: got"，
     IP 仍是缺省 —— 证明 ACK/NAK 两条分支分别走得通，不是只看 got

  D（T1 续租）：4s 租约，过了 T1(2s) 必须**自己**发 REQUEST 续上
     （ciaddr 填好、单播给原服务器、不带 option 50/54），不用再敲 dhcp
  E（续租被无视）：服务器不理续租 -> 推进到 T2 广播、租期到作废并重新
     DISCOVER；**绝不**出现续租成功 —— 与 D 结果相反，证明状态机真的
     在按 T1/T2/到期推进，而不是"续上了一次"就万事大吉

用法：python tests/test_dhcp.py   （退出码 0 = 全通过）
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from test_nvme import (SerialReader, Qmp, flat, wait_for, wait_port_free,  # noqa: E402
                       kill_all_qemu, run_cmd)

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.img")

PORT_A = (4521, 4522, 4527)     # (QMP, serial, netdev)
PORT_B = (4523, 4524, 4528)
PORT_C = (4525, 4526, 4529)
PORT_D = (4551, 4552, 4557)     # D：T1 自动续租
PORT_E = (4553, 4554, 4558)     # E：服务器不理续租 -> T2 广播 -> 过期重来
BOOT_WAIT = 150

WORK = {
    "A": os.path.join(HERE, "dhcp_a.img").replace("\\", "/"),
    "B": os.path.join(HERE, "dhcp_b.img").replace("\\", "/"),
    "C": os.path.join(HERE, "dhcp_c.img").replace("\\", "/"),
    "D": os.path.join(HERE, "dhcp_d.img").replace("\\", "/"),
    "E": os.path.join(HERE, "dhcp_e.img").replace("\\", "/"),
}

GUEST_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
HOST_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x02])
BCAST_MAC = b"\xff" * 6
BCAST_IP = bytes([255, 255, 255, 255])
ZERO_IP = bytes([0, 0, 0, 0])

SERVER_IP = bytes([10, 0, 2, 2])
OFFER_IP = bytes([10, 0, 2, 66])       # 与缺省 10.0.2.15 不同，换没换看得出
MASK = bytes([255, 255, 255, 0])
ROUTER = bytes([10, 0, 2, 2])
DNS = bytes([10, 0, 2, 3])
LEASE = 3600
LEASE_SHORT = 4          # D/E 组：4 秒租约 -> T1=2s、T2=3.5s（不然要等几分钟）

MAGIC = 0x63825363


# ---------------- 以太网/IPv4/UDP 工具 ----------------

def cksum(data):
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) | data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def udp_checksum(src, dst, udp):
    pseudo = src + dst + bytes([0, 17]) + struct.pack(">H", len(udp))
    body = udp[:6] + b"\x00\x00" + udp[8:]
    if len(body) & 1:
        body += b"\x00"
    c = cksum(pseudo + body)
    return 0xFFFF if c == 0 else c


def parse_dhcp(frame):
    """从以太帧里解出 DHCP 报文字段。返回 dict 或 None（不是 DHCP 包）。"""
    if len(frame) < 34:
        return None
    if struct.unpack(">H", frame[12:14])[0] != 0x0800:
        return None
    ip = frame[14:]
    if len(ip) < 20 or ip[9] != 17:
        return None
    ihl = (ip[0] & 0x0F) * 4
    if len(ip) < ihl + 8:
        return None
    src_ip, dst_ip = ip[12:16], ip[16:20]
    udp = ip[ihl:]
    sport, dport, ulen, usum = struct.unpack(">HHHH", udp[:8])
    if sport != 68 or dport != 67 or ulen < 8 + 240:
        return None
    if len(udp) < ulen:
        return None
    seg = udp[:ulen]
    bootp = seg[8:]

    magic_ok = struct.unpack(">I", bootp[236:240])[0] == MAGIC
    opts = {}
    i = 240
    while i + 1 < len(bootp):
        code = bootp[i]
        if code == 0:
            i += 1
            continue
        if code == 255:
            break
        olen = bootp[i + 1]
        if i + 2 + olen > len(bootp):
            break
        opts[code] = bytes(bootp[i + 2:i + 2 + olen])
        i += 2 + olen

    return {
        "src_ip": bytes(src_ip), "dst_ip": bytes(dst_ip),
        "mac_dst": bytes(frame[0:6]),
        "ciaddr": bytes(bootp[12:16]),     # 非 0 = 已绑定（续租形态）
        "sport": sport, "dport": dport,
        "xid": struct.unpack(">I", bootp[4:8])[0],
        "flags": struct.unpack(">H", bootp[10:12])[0],
        "chaddr": bytes(bootp[28:34]),
        "msg_type": opts[53][0] if 53 in opts and opts[53] else 0,
        "opt50": opts.get(50), "opt54": opts.get(54), "opt61": opts.get(61),
        "magic_ok": magic_ok,
        "ip_sum_ok": cksum(ip[:ihl]) == 0,
        # 未计算（0）也算不过关：本内核明确算了 UDP 校验和
        "udp_sum_ok": usum != 0 and usum == udp_checksum(src_ip, dst_ip, seg),
    }


def build_reply(msg_type, xid, chaddr, yiaddr, lease=LEASE):
    """构造 OFFER/ACK/NAK：以太网广播 + IP 源 SERVER_IP 目的 255.255.255.255。"""
    b = bytearray(300)
    b[0] = 2                                   # BOOTREPLY
    b[1] = 1
    b[2] = 6
    b[4:8] = struct.pack(">I", xid)
    b[10:12] = struct.pack(">H", 0x8000)       # broadcast flag
    if msg_type != 6:                          # NAK 不给地址
        b[16:20] = yiaddr
    b[20:24] = SERVER_IP                       # siaddr
    b[28:34] = chaddr
    b[236:240] = struct.pack(">I", MAGIC)

    o = 240
    b[o:o + 3] = bytes([53, 1, msg_type]); o += 3
    if msg_type != 6:
        b[o:o + 6] = bytes([1, 4]) + MASK; o += 6
        b[o:o + 6] = bytes([3, 4]) + ROUTER; o += 6
        b[o:o + 6] = bytes([6, 4]) + DNS; o += 6
        b[o:o + 6] = bytes([51, 4]) + struct.pack(">I", lease); o += 6
    b[o:o + 6] = bytes([54, 4]) + SERVER_IP; o += 6
    b[o] = 255

    udp = struct.pack(">HHHH", 67, 68, 8 + len(b), 0) + bytes(b)
    udp = udp[:6] + struct.pack(">H", udp_checksum(SERVER_IP, BCAST_IP, udp)) + udp[8:]
    total = 20 + len(udp)
    ip = struct.pack(">BBHHHBBH", 0x45, 0, total, 0x4001, 0, 64, 17, 0) + \
         SERVER_IP + BCAST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return BCAST_MAC + HOST_MAC + b"\x08\x00" + ip + udp


def arp_reply_to(frame):
    """ARP reply：sha/spa/tha/tpa 全部取自请求（不硬编码 guest IP）。
    ARP 载荷从 frame[14] 起：sha[8:14] spa[14:18] tha[18:24] tpa[24:28]。"""
    arp = frame[14:]
    sha, spa, tpa = arp[8:14], arp[14:18], arp[24:28]
    a = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2)
    return sha + HOST_MAC + b"\x08\x06" + a + HOST_MAC + tpa + sha + spa


def icmp_reply_to(frame, req_ic):
    """echo reply：IP 方向按请求翻转，以太目的取请求的源 MAC。"""
    ip = frame[14:]
    ic = bytes([0, 0]) + req_ic[2:]
    ic = ic[:2] + struct.pack(">H", cksum(ic)) + ic[4:]
    total = 20 + len(ic)
    out = struct.pack(">BBHHHBBH", 0x45, 0, total, 0x4001, 0, 64, 1, 0) + \
          ip[16:20] + ip[12:16]
    out = out[:10] + struct.pack(">H", cksum(out)) + out[12:]
    return frame[6:12] + HOST_MAC + b"\x08\x00" + out + ic


# ---------------- 自建 DHCP 服务器（跑在 netdev socket 对端） ----------------

class DhcpServer(threading.Thread):
    """mode: 'ok' 正常应答 / 'silent' 收下不回 / 'nak' 对 REQUEST 回 NAK
              / 'renew' 短租约，续租照常 ACK
              / 'renew_silent' 短租约，但**不理**续租（ciaddr != 0 的 REQUEST）"""

    def __init__(self, port, mode, lease=LEASE):
        threading.Thread.__init__(self)
        self.daemon = True
        self.port = port
        self.mode = mode
        self.lease = lease
        self.running = True
        self.lock = threading.Lock()
        self.events = []
        self.buf = b""
        self.sock = None
        self.pings = 0
        self.ping_src = None

    def stop(self):
        self.running = False
        try:
            if self.sock:
                self.sock.close()
        except Exception:
            pass

    def seen(self, msg_type):
        with self.lock:
            return [e for e in self.events if e["msg_type"] == msg_type]

    def run(self):
        try:
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        except Exception:
            return
        self.sock.settimeout(0.3)
        while self.running:
            frame = self._recv()
            if not frame:
                continue
            if len(frame) < 34:
                continue
            et = struct.unpack(">H", frame[12:14])[0]
            if et == 0x0806:                            # ARP：让 ping 能解析网关
                if self.mode in ("ok", "renew", "renew_silent"):
                    a = frame[14:]
                    if len(a) >= 28 and struct.unpack(">H", a[6:8])[0] == 1 \
                            and a[24:28] == SERVER_IP:
                        self._send(arp_reply_to(frame))
                continue
            if et != 0x0800:
                continue
            ip = frame[14:]
            if len(ip) < 20:
                continue
            if ip[9] == 1:                              # ICMP echo request
                ihl = (ip[0] & 0x0F) * 4
                ic = ip[ihl:]
                if len(ic) >= 8 and ic[0] == 8 and ic[1] == 0 \
                        and self.mode != "silent":
                    self.pings += 1
                    self.ping_src = bytes(ip[12:16])    # 实际用的源地址
                    self._send(icmp_reply_to(frame, ic))
                continue
            if ip[9] != 17:
                continue
            d = parse_dhcp(frame)
            if not d:
                continue
            with self.lock:
                self.events.append(d)
            if self.mode == "silent":
                continue
            if d["msg_type"] == 1:                      # DISCOVER -> OFFER
                self._send(build_reply(2, d["xid"], d["chaddr"], OFFER_IP,
                                       self.lease))
            elif d["msg_type"] == 3:                    # REQUEST -> ACK / NAK
                if self.mode == "nak":
                    self._send(build_reply(6, d["xid"], d["chaddr"], OFFER_IP))
                    continue
                # renew_silent：只答应"未绑定"的 REQUEST，续租一律装死
                if self.mode == "renew_silent" and d["ciaddr"] != ZERO_IP:
                    continue
                self._send(build_reply(5, d["xid"], d["chaddr"], OFFER_IP,
                                       self.lease))

    def _send(self, frame):
        if len(frame) < 60:
            frame = frame + b"\x00" * (60 - len(frame))
        try:
            self.sock.sendall(struct.pack(">I", len(frame)) + frame)
        except Exception:
            pass

    def _recv(self):
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if 14 <= n <= 65535:
                    if len(self.buf) >= 4 + n:
                        frame = self.buf[4:4 + n]
                        self.buf = self.buf[4 + n:]
                        return frame
                else:                                    # 裸帧兜底
                    frame = self.buf
                    self.buf = b""
                    return frame
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            except Exception:
                return None
            if not chunk:
                return None
            self.buf += chunk


# ---------------- QEMU 生命周期 ----------------

def boot(work, ports):
    qmp_port, ser_port, net_port = ports
    if os.path.isfile(DISK):
        shutil.copyfile(DISK, work)
    return subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + work,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % ser_port,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port,
         "-netdev", "socket,id=n0,listen=127.0.0.1:%d" % net_port,
         "-device", "rtl8139,netdev=n0,mac=52:54:00:12:34:56"],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def teardown(proc, qmp, serial, srv, qmp_port):
    for x in (srv,):
        try:
            if x:
                x.stop()
        except Exception:
            pass
    try:
        if qmp:
            qmp.quit()
    except Exception:
        pass
    try:
        if serial:
            serial.close()
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except Exception:
        kill_all_qemu()
    wait_port_free(qmp_port, 15)


def shell_up(serial, qmp):
    if not wait_for(serial, "TASK: preemptive", BOOT_WAIT):
        return False
    time.sleep(3.0)
    return True


# ---------------- 三组用例 ----------------

def case_ok():
    """A：服务器正常应答 -> 真拿到地址，且换址后 ping 网关通"""
    res = []
    proc = boot(WORK["A"], PORT_A)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_A[1])
        qmp = Qmp(PORT_A[0])
        srv = DhcpServer(PORT_A[2], "ok")
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL A: shell did not come up")
            return [("shell up", False)]

        # 等**结果行**而不是 "dhcp:" —— "DHCP: discovering..." 一打印就有了，
        # 拿它当成功标志是弱断言（任何分支都会立刻匹配）。
        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("lease acquired", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])
        body = flat(serial.tail_from(0))

        res.append(("got offered address 10.0.2.66", "dhcp: got 10.0.2.66" in body))
        res.append(("netmask parsed", "dhcp: mask 255.255.255.0" in body))
        res.append(("router parsed", "dhcp: router 10.0.2.2" in body))
        res.append(("dns parsed", "dhcp: dns 10.0.2.3" in body))
        res.append(("lease parsed (3600s)", "dhcp: lease 3600s" in body))
        res.append(("no timeout/NAK branch taken",
                    "dhcp: no response" not in body and "dhcp: nak" not in body))

        # 协议侧：不是"碰巧打对了字"，报文本身得对
        disc = srv.seen(1)
        req = srv.seen(3)
        res.append(("DISCOVER received", len(disc) >= 1))
        res.append(("REQUEST received", len(req) >= 1))
        if disc:
            d = disc[0]
            res.append(("DISCOVER src 0.0.0.0", d["src_ip"] == ZERO_IP))
            res.append(("DISCOVER dst 255.255.255.255", d["dst_ip"] == BCAST_IP))
            res.append(("UDP 68 -> 67", d["sport"] == 68 and d["dport"] == 67))
            res.append(("magic cookie valid", d["magic_ok"]))
            res.append(("IP header checksum valid", d["ip_sum_ok"]))
            res.append(("UDP checksum valid", d["udp_sum_ok"]))
            res.append(("broadcast flag set", d["flags"] & 0x8000 != 0))
            res.append(("client id = 01 + MAC", d["opt61"] == b"\x01" + GUEST_MAC))
        if disc and req:
            res.append(("REQUEST xid matches DISCOVER",
                        req[0]["xid"] == disc[0]["xid"]))
            res.append(("REQUEST asks for the offered IP (opt50)",
                        req[0]["opt50"] == OFFER_IP))
            res.append(("REQUEST names the server (opt54)",
                        req[0]["opt54"] == SERVER_IP))

        # 应用侧：nic 认账 + 换址后真的能通信
        ok2, out2 = run_cmd(qmp, serial, "nic", "dhcp: ip 10.0.2.66", 25.0)
        res.append(("`nic` shows the lease", ok2))
        ok3, out3 = run_cmd(qmp, serial, "ping 10.0.2.2", "reply from", 35.0)
        res.append(("ping gateway works on the new address", ok3))
        res.append(("gateway actually saw ICMP echoes", srv.pings >= 1))
        # 真正的"地址生效"：屏幕上的租约来自 g_lease，写进 rtl8139 的字节序
        # 搞反时屏幕仍然显示 10.0.2.66，但实际发出的是 66.2.0.10。
        # 只有查实际发包才抓得到（这条就是为那个 bug 补的）。
        res.append(("outgoing packets really use the leased address",
                    srv.ping_src == OFFER_IP))
        if not ok3:
            print("---- ping tail ----\n" + out3[-800:])
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_A[0])


def case_silent():
    """B：服务器装死 -> 超时，绝不伪造成功；地址保持缺省"""
    res = []
    proc = boot(WORK["B"], PORT_B)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_B[1])
        qmp = Qmp(PORT_B[0])
        srv = DhcpServer(PORT_B[2], "silent")
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL B: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: no response", 90.0)
        res.append(("reported timeout", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])
        body = flat(serial.tail_from(0))
        res.append(("no bogus success", "dhcp: got" not in body))
        res.append(("server really stayed silent", len(srv.events) >= 1))

        ok2, out2 = run_cmd(qmp, serial, "nic", "ip: 10.0.2.15", 25.0)
        res.append(("address unchanged (still 10.0.2.15)", ok2))
        if not ok2:
            print("---- nic tail ----\n" + out2[-800:])
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_B[0])


def case_nak():
    """C：REQUEST 被 NAK -> 报 NAK 且不应用地址"""
    res = []
    proc = boot(WORK["C"], PORT_C)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_C[1])
        qmp = Qmp(PORT_C[0])
        srv = DhcpServer(PORT_C[2], "nak")
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL C: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: nak", 90.0)
        res.append(("reported NAK", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])
        body = flat(serial.tail_from(0))
        res.append(("no bogus success", "dhcp: got" not in body))
        res.append(("server really sent NAK", len(srv.seen(3)) >= 1))

        ok2, out2 = run_cmd(qmp, serial, "nic", "ip: 10.0.2.15", 25.0)
        res.append(("address unchanged (still 10.0.2.15)", ok2))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_C[0])


def case_renew():
    """D：4s 租约 -> 过了 T1(2s) 必须**自己**续租（不用再敲 dhcp）

    反向组是 E。这里要证明的不只是"又发了一个包"，而是续租的形态对：
    ciaddr 填着自己的地址、源 IP 也是它（不再是 0.0.0.0）、单播给原服务器、
    且按 RFC 2131 表 5 **不带** option 50/54。
    """
    res = []
    proc = boot(WORK["D"], PORT_D)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_D[1])
        qmp = Qmp(PORT_D[0])
        srv = DhcpServer(PORT_D[2], "renew", lease=LEASE_SHORT)
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL D: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got the first lease", ok))
        res.append(("exactly one REQUEST so far", len(srv.seen(3)) == 1))

        # 睡过 T1（2s）：期间没人敲命令
        ok, out = run_cmd(qmp, serial, "sleep 5000", "done", 200.0)
        res.append(("slept past T1", ok))
        res.append(("reported the renewal", "dhcp: renewed" in out))
        res.append(("never said the lease expired", "lease expired" not in out))

        reqs = srv.seen(3)
        res.append(("a second REQUEST went out unprompted", len(reqs) >= 2))
        renew = [r for r in reqs if r["ciaddr"] == OFFER_IP]
        res.append(("the renewal is a bound REQUEST (ciaddr filled)", bool(renew)))
        if renew:
            r = renew[0]
            res.append(("sent from the leased address", r["src_ip"] == OFFER_IP))
            res.append(("RENEWING goes unicast to the server",
                        r["dst_ip"] == SERVER_IP))
            res.append(("...and to the server's MAC", r["mac_dst"] == HOST_MAC))
            res.append(("omits option 50 (requested IP)", r["opt50"] is None))
            res.append(("omits option 54 (server id)", r["opt54"] is None))
            res.append(("still carries option 61 (client id)",
                        r["opt61"] == b"\x01" + GUEST_MAC))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_D[0])


def case_renew_silent():
    """E：服务器不理续租 -> 必须推进到 T2 广播，租期到作废并重新 DISCOVER

    与 D 结果相反：这里"续租"绝不能成功。若状态机停在 RENEWING 不动，
    "lease expired" 就不会出现，第二条也就无从谈起。
    """
    res = []
    proc = boot(WORK["E"], PORT_E)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_E[1])
        qmp = Qmp(PORT_E[0])
        srv = DhcpServer(PORT_E[2], "renew_silent", lease=LEASE_SHORT)
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL E: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got the first lease", ok))

        ok, out = run_cmd(qmp, serial, "sleep 6000", "done", 240.0)
        res.append(("slept past the whole lease", ok))
        res.append(("gave the address up at expiry", "dhcp: lease expired" in out))

        reqs = srv.seen(3)
        bound = [r for r in reqs if r["ciaddr"] == OFFER_IP]
        res.append(("tried to renew (bound REQUEST)", len(bound) >= 1))
        res.append(("rebinding went out as broadcast",
                    any(r["dst_ip"] == BCAST_IP for r in bound)))
        res.append(("restarted from DISCOVER after expiry",
                    len(srv.seen(1)) >= 2))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_E[0])


def main():
    print("== test_dhcp: DHCP client (D/O/R/A) ==")
    kill_all_qemu()
    total = 0
    failed = 0
    for name, fn in (("A normal lease", case_ok),
                     ("B silent server", case_silent),
                     ("C NAK", case_nak),
                     ("D T1 renewal", case_renew),
                     ("E renewal ignored -> expiry", case_renew_silent)):
        print("-- %s --" % name)
        res = fn()
        for label, ok in res:
            total += 1
            if not ok:
                failed += 1
            print("   %s %s" % ("PASS" if ok else "FAIL", label))
    print("\n%d/%d passed" % (total - failed, total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
