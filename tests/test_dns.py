# -*- coding: utf-8 -*-
"""test_dns.py - DNS 解析器 E2E（RFC 1035，只查 A 记录）

判定走**串口（COM1）**按子串断言（沿用 nvme/sysvol/dhcp 那套）；
协议字段走 Python 侧自建服务器（`-netdev socket` 直连）。
端口 A 4531(QMP)/4532(serial)/4537(netdev)、B 4533/4534/4538、C 4535/4536/4539。

要证明的四件事：
  1. 查询报文对：QNAME 是标准 label 编码（3www7example3com0，不是裸字符串）、
     QTYPE=1、QCLASS=1、UDP 目的端口 53、UDP 校验和算对、响应按 ID 匹配
  2. 应答解析对：应答里的域名用**压缩指针**（0xC00C）也必须能跳过 ——
     解析器若只会跳 label，这里就会解析失败（真实服务器几乎都压缩）
  3. CNAME 能跳过继续找到 A 记录
  4. **跨网段走网关**：DNS 服务器在别的网段时，ARP 必须问**网关**，
     以太网目的 MAC 是网关的，而 IP 头里的目的仍是 DNS 服务器
     —— ARP 直接问 8.8.8.8 是新手错误，没人会应答

铁律「两组结果必须相反」：
  A（同网段，服务器正常）：1-3 全成立，拿到 93.184.216.34
  B（跨网段 8.8.8.8）：同样解析成功，且断言 ARP 只问了网关 10.0.2.2、
     **一次都没问 8.8.8.8**，DNS 帧的 IP 目的仍是 8.8.8.8
  C（失败三分支）：无服务器 -> "no server"；手动配了但服务器回 NXDOMAIN
     -> "no A record"；服务器装死 -> "timeout"；**全程绝不出现 " = "**
     —— 没有这一组，"解析成功"可能只是打印逻辑恒真

  D（缓存）：第二次 lookup 必须走缓存、**服务器上只有一个查询**；
     `dns flush` 之后再问会重新发；TTL=1s 的名字睡过 1s 必须失效重查
     —— 没有 D，"解析成功"看不出答案是不是从缓存来的
  E（多服务器）：option 6 下发两台，第一台装死 -> 必须回退到第二台，
     且**先问过第一台**（没问过就谈不上回退）
  F（ping 接域名）：`ping www.example.com` 先解析再 ping 通；解析失败
     的名字（NXDOMAIN）必须报解析失败且**一个 ICMP 都不发**

用法：python tests/test_dns.py   （退出码 0 = 全通过）
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

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
from test_dhcp import (HOST_MAC, SERVER_IP, OFFER_IP, MASK, BCAST_MAC,  # noqa: E402
                       cksum, udp_checksum, arp_reply_to, icmp_reply_to)

QEMU = qemu_exe()
IMG = image_path()
DISK = disk_path()

PORT_A = alloc_port(3)  # (QMP, serial, netdev)
PORT_B = alloc_port(3)
PORT_C = alloc_port(3)
PORT_D = alloc_port(3)  # 缓存（命中/flush/TTL 过期）
PORT_E = alloc_port(3)  # 多服务器回退
PORT_F = alloc_port(3)  # ping 接域名
BOOT_WAIT = 150

WORK = {
    "A": os.path.join(HERE, "dns_a.vhd").replace("\\", "/"),
    "B": os.path.join(HERE, "dns_b.vhd").replace("\\", "/"),
    "C": os.path.join(HERE, "dns_c.vhd").replace("\\", "/"),
    "D": os.path.join(HERE, "dns_d.vhd").replace("\\", "/"),
    "E": os.path.join(HERE, "dns_e.vhd").replace("\\", "/"),
    "F": os.path.join(HERE, "dns_f.vhd").replace("\\", "/"),
}

GW_IP = bytes([10, 0, 2, 2])            # 网关（也是 Python 侧的 MAC 主人）
# DHCP 服务器故意**不是**网关：否则 guest 在 DHCP 阶段就顺手把
# (网关 IP -> MAC) 学进 ARP 缓存，后面查 DNS 时根本不会发 ARP，
# "ARP 问的是网关"这条断言就成了空断言。
DHCP_SRV_IP = bytes([10, 0, 2, 9])
DNS_LOCAL = bytes([10, 0, 2, 3])        # A 组：同网段 DNS
DNS_REMOTE = bytes([8, 8, 8, 8])        # B 组：跨网段 DNS
DNS_BACKUP = bytes([10, 0, 2, 4])      # E 组：option 6 里的第二个服务器
LEASE = 3600

A_RECORD = bytes([93, 184, 216, 34])    # www.example.com 的"答案"
CNAME_IP = bytes([10, 1, 2, 3])         # cname.test 转一圈后的答案


# ---------------- DNS 报文工具 ----------------

def enc_name(name):
    """'a.b' -> 01 61 01 62 00"""
    out = b""
    for lab in name.split("."):
        out += bytes([len(lab)]) + lab.encode("latin1")
    return out + b"\x00"


def dec_qname(p, i):
    """解码（未压缩的）QNAME；返回 (name, 结束偏移)"""
    labels = []
    while i < len(p):
        l = p[i]
        if l == 0:
            return ".".join(labels), i + 1
        if l & 0xC0:                      # 查询里不该出现指针
            return ".".join(labels), i
        labels.append(p[i + 1:i + 1 + l].decode("latin1"))
        i += 1 + l
    return ".".join(labels), i


def build_dns_reply(q, answers, rcode=0, ttl=300):
    """q = 查询报文；answers = [(type, rdata_bytes), ...]。
    问题区原样回显；应答的域名一律用指向问题区的压缩指针 0xC00C ——
    真实服务器都这么干，解析器必须能处理。"""
    qend = dec_qname(q, 12)[1] + 4       # name + QTYPE + QCLASS
    flags = 0x8180 | (rcode & 0x0F)      # QR=1 RD=1 RA=1
    out = q[0:2] + struct.pack(">HHHHH", flags, 1, len(answers), 0, 0) + q[12:qend]
    for typ, rdata in answers:
        out += b"\xc0\x0c" + struct.pack(">HHIH", typ, 1, ttl, len(rdata)) + rdata
    return out


def build_dhcp_reply(msg_type, xid, chaddr, yiaddr, dns_ip, router, src_ip):
    b = bytearray(300)
    b[0] = 2
    b[1] = 1
    b[2] = 6
    b[4:8] = struct.pack(">I", xid)
    b[10:12] = struct.pack(">H", 0x8000)
    b[16:20] = yiaddr
    b[20:24] = src_ip
    b[28:34] = chaddr
    b[236:240] = struct.pack(">I", 0x63825363)
    o = 240
    b[o:o + 3] = bytes([53, 1, msg_type]); o += 3
    b[o:o + 6] = bytes([1, 4]) + MASK; o += 6
    b[o:o + 6] = bytes([3, 4]) + router; o += 6
    # option 6 可以带**多个** DNS 服务器（4 字节一个）；给列表就全部下发
    servers = dns_ip if isinstance(dns_ip, (list, tuple)) else [dns_ip]
    blob = b"".join(bytes(s) for s in servers)
    b[o:o + 2 + len(blob)] = bytes([6, len(blob)]) + blob; o += 2 + len(blob)
    b[o:o + 6] = bytes([51, 4]) + struct.pack(">I", LEASE); o += 6
    b[o:o + 6] = bytes([54, 4]) + src_ip; o += 6
    b[o] = 255

    udp = struct.pack(">HHHH", 67, 68, 8 + len(b), 0) + bytes(b)
    udp = udp[:6] + struct.pack(">H", udp_checksum(src_ip, b"\xff" * 4, udp)) + udp[8:]
    total = 20 + len(udp)
    ip = struct.pack(">BBHHHBBH", 0x45, 0, total, 0x4001, 0, 64, 17, 0) + \
         src_ip + b"\xff" * 4
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return BCAST_MAC + HOST_MAC + b"\x08\x00" + ip + udp


# ---------------- 局域网服务器：DHCP + ARP + DNS（+ ICMP 顺带） ----------------

class LanServer(threading.Thread):
    """dns_ip 为 None 时只做 DHCP（不参与 DNS）；silent/nx 名字集合控制失败分支"""

    def __init__(self, port, dns_ip=None, silent=(), nx=(), serve_dhcp=True,
                 silent_srv=(), ttl_map=None):
        """dns_ip：单个 IP 或列表（列表 -> DHCP option 6 下发多个）。
        silent：不回答的名字；nx：回 NXDOMAIN 的名字；
        silent_srv：装死的**服务器 IP**（多服务器回退用）；
        ttl_map：{名字: TTL 秒}，默认 300。"""
        threading.Thread.__init__(self)
        self.daemon = True
        self.port = port
        self.dns_ip = dns_ip
        self.silent = set(silent)
        self.nx = set(nx)
        self.serve_dhcp = serve_dhcp
        self.silent_srv = set(bytes(s) for s in silent_srv)
        self.ttl_map = ttl_map or {}
        self.running = True
        self.lock = threading.Lock()
        self.queries = []        # dict: name/qtype/qclass/dport/sum_ok/id/ip_dst/mac_dst
        self.arp_asked = []      # 被 ARP 询问过的 IP 列表
        self.dns_replied = 0
        self.buf = b""
        self.sock = None

    def stop(self):
        self.running = False
        try:
            if self.sock:
                self.sock.close()
        except Exception:
            pass

    def names(self):
        with self.lock:
            return [q["name"] for q in self.queries]

    def count(self, name):
        with self.lock:
            return sum(1 for q in self.queries if q["name"] == name)

    def count_to(self, ip):
        with self.lock:
            return sum(1 for q in self.queries if q["ip_dst"] == bytes(ip))

    def first(self, name):
        with self.lock:
            for q in self.queries:
                if q["name"] == name:
                    return q
        return None

    def run(self):
        try:
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        except Exception:
            return
        self.sock.settimeout(0.3)
        while self.running:
            frame = self._recv()
            if not frame or len(frame) < 34:
                continue
            et = struct.unpack(">H", frame[12:14])[0]
            if et == 0x0806:
                a = frame[14:]
                if len(a) >= 28 and struct.unpack(">H", a[6:8])[0] == 1:
                    with self.lock:
                        self.arp_asked.append(bytes(a[24:28]))
                    # 网关/DNS/任何本机地址都由这一块 MAC 应答
                    self._send(arp_reply_to(frame))
                continue
            if et != 0x0800:
                continue
            ip = frame[14:]
            if len(ip) < 20:
                continue
            if ip[9] == 1:
                ihl = (ip[0] & 0x0F) * 4
                ic = ip[ihl:]
                if len(ic) >= 8 and ic[0] == 8 and ic[1] == 0:
                    self._send(icmp_reply_to(frame, ic))
                continue
            if ip[9] != 17:
                continue
            ihl = (ip[0] & 0x0F) * 4
            udp = ip[ihl:]
            if len(udp) < 8:
                continue
            sport, dport, ulen, usum = struct.unpack(">HHHH", udp[:8])
            if dport == 67 and self.serve_dhcp:
                seg = udp[:ulen]
                bootp = seg[8:]
                xid = struct.unpack(">I", bootp[4:8])[0]
                opts = {}
                i = 240
                while i + 1 < len(bootp):
                    c = bootp[i]
                    if c == 0:
                        i += 1
                        continue
                    if c == 255:
                        break
                    ol = bootp[i + 1]
                    opts[c] = bytes(bootp[i + 2:i + 2 + ol])
                    i += 2 + ol
                mt = opts[53][0] if 53 in opts else 0
                dns_ip = self.dns_ip if self.dns_ip else DNS_LOCAL
                if mt == 1:
                    self._send(build_dhcp_reply(2, xid, bootp[28:34], OFFER_IP,
                                                dns_ip, GW_IP, DHCP_SRV_IP))
                elif mt == 3:
                    self._send(build_dhcp_reply(5, xid, bootp[28:34], OFFER_IP,
                                                dns_ip, GW_IP, DHCP_SRV_IP))
                continue
            if dport != 53:
                continue
            seg = udp[:ulen]
            q = seg[8:]
            if len(q) < 12:
                continue
            name, _ = dec_qname(q, 12)
            qend = dec_qname(q, 12)[1]
            qtype = struct.unpack(">H", q[qend:qend + 2])[0] if qend + 4 <= len(q) else 0
            qclass = struct.unpack(">H", q[qend + 2:qend + 4])[0] if qend + 4 <= len(q) else 0
            with self.lock:
                self.queries.append({
                    "name": name, "qtype": qtype, "qclass": qclass,
                    "sport": sport, "dport": dport,
                    "id": struct.unpack(">H", q[0:2])[0],
                    "ip_dst": bytes(ip[16:20]), "ip_src": bytes(ip[12:16]),
                    "mac_dst": bytes(frame[0:6]),
                    "sum_ok": usum != 0 and usum == udp_checksum(ip[12:16], ip[16:20], seg),
                })
            if name in self.silent:
                continue
            if bytes(ip[16:20]) in self.silent_srv:
                continue                       # 这台服务器装死（回退测试）
            if name in self.nx:
                ans, rcode = [], 3
            elif name == "cname.test":
                ans, rcode = [(5, enc_name("alias.test")), (1, CNAME_IP)], 0
            else:
                ans, rcode = [(1, A_RECORD)], 0
            self.dns_replied += 1
            self._reply(frame, udp, q, ans, rcode, bytes(ip[16:20]),
                        self.ttl_map.get(name, 300))

    def _reply(self, req_frame, req_udp, q, ans, rcode, srv_ip, ttl=300):
        """回一个 DNS 应答：源 IP 是查询**发往**的那台服务器（可能跨网段），
        目的取请求的源。TTL 可按名字定制（测缓存过期）。"""
        body = build_dns_reply(q, ans, rcode, ttl)
        src = srv_ip
        client_ip = req_frame[26:30]
        client_port = struct.unpack(">H", req_udp[0:2])[0]
        udp = struct.pack(">HHHH", 53, client_port, 8 + len(body), 0) + body
        udp = udp[:6] + struct.pack(">H", udp_checksum(src, client_ip, udp)) + udp[8:]
        total = 20 + len(udp)
        ip = struct.pack(">BBHHHBBH", 0x45, 0, total, 0x4001, 0, 64, 17, 0) + \
             src + client_ip
        ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
        self._send(frame_of(req_frame, ip, udp))

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
                else:
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


def frame_of(req_frame, ip, udp):
    """以太头：源 MAC 用网关的，目的 MAC 用请求的源 MAC"""
    return req_frame[6:12] + HOST_MAC + b"\x08\x00" + ip + udp


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
    try:
        if srv:
            srv.stop()
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

def case_local():
    """A：同网段 DNS，正常应答 + CNAME + 压缩指针"""
    res = []
    proc = boot(WORK["A"], PORT_A)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_A[1])
        qmp = Qmp(PORT_A[0])
        srv = LanServer(PORT_A[2], dns_ip=DNS_LOCAL)
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL A: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got a lease first", ok))
        ok, out = run_cmd(qmp, serial, "lookup www.example.com",
                          "dns: www.example.com = 93.184.216.34", 30.0)
        res.append(("resolved www.example.com", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])

        ok, out = run_cmd(qmp, serial, "lookup cname.test",
                          "dns: cname.test = 10.1.2.3", 30.0)
        res.append(("skips CNAME and finds the A record", ok))

        q = srv.first("www.example.com")
        res.append(("query reached the server", q is not None))
        if q:
            res.append(("QTYPE=1 (A)", q["qtype"] == 1))
            res.append(("QCLASS=1 (IN)", q["qclass"] == 1))
            res.append(("UDP dst port 53", q["dport"] == 53))
            res.append(("UDP checksum valid", q["sum_ok"]))
            res.append(("queried from the leased address", q["ip_src"] == OFFER_IP))
            res.append(("asked the DHCP-provided server", q["ip_dst"] == DNS_LOCAL))
        # QNAME 编码：裸字符串会被服务器解成空名，这里必须有真实的点分名
        res.append(("QNAME encoded as labels (not a bare string)",
                    "www.example.com" in srv.names()))
        res.append(("server answered both queries", srv.dns_replied >= 2))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_A[0])


def case_remote():
    """B：DNS 在别的网段 -> ARP 必须问网关，IP 目的仍是 8.8.8.8"""
    res = []
    proc = boot(WORK["B"], PORT_B)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_B[1])
        qmp = Qmp(PORT_B[0])
        srv = LanServer(PORT_B[2], dns_ip=DNS_REMOTE)
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL B: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got a lease first", ok))
        ok2, out2 = run_cmd(qmp, serial, "dns", "dns: server 8.8.8.8", 25.0)
        res.append(("DNS server came from DHCP option 6", ok2))

        ok, out = run_cmd(qmp, serial, "lookup www.example.com",
                          "dns: www.example.com = 93.184.216.34", 30.0)
        res.append(("resolved across subnets", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])

        q = srv.first("www.example.com")
        res.append(("query reached the remote server", q is not None))
        if q:
            res.append(("IP destination is still 8.8.8.8", q["ip_dst"] == DNS_REMOTE))
            res.append(("ethernet destination is the gateway MAC",
                        q["mac_dst"] == HOST_MAC))
        res.append(("ARP asked the gateway 10.0.2.2", GW_IP in srv.arp_asked))
        res.append(("ARP never asked 8.8.8.8 (unreachable)",
                    DNS_REMOTE not in srv.arp_asked))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_B[0])


def case_failures():
    """C：三个失败分支，全程不许出现一次成功"""
    res = []
    proc = boot(WORK["C"], PORT_C)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_C[1])
        qmp = Qmp(PORT_C[0])
        srv = LanServer(PORT_C[2], dns_ip=DNS_LOCAL, serve_dhcp=False,
                        silent=("to.test",), nx=("nx.test",))
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL C: shell did not come up")
            return [("shell up", False)]

        # 1) 没跑 dhcp、也没手动配 -> 没有服务器可问
        ok, out = run_cmd(qmp, serial, "lookup a.test", "dns: no server", 25.0)
        res.append(("no server configured -> refuses", ok))

        # 2) 手动指定服务器
        ok, out = run_cmd(qmp, serial, "dns 10.0.2.3", "dns: server set", 20.0)
        res.append(("manual server accepted", ok))

        # 3) 服务器回 NXDOMAIN (rcode 3)
        ok, out = run_cmd(qmp, serial, "lookup nx.test", "dns: no a record", 25.0)
        res.append(("NXDOMAIN reported as no A record", ok))

        # 4) 服务器装死
        # 注意：icount 下每 1ms 一次 PIT 中断，2.5s 的虚拟等待实测要 ~25s
        # 实时，两次重试就是 ~50s —— 超时给 35s 会误判成"卡死"。
        ok, out = run_cmd(qmp, serial, "lookup to.test", "dns: timeout", 90.0)
        res.append(("silence reported as timeout", ok))
        if not ok:
            print("---- timeout branch tail ----\n" + out[-1000:])
        res.append(("server really stayed silent for it", "to.test" in srv.names()))

        body = flat(serial.tail_from(0))
        # 成功行长这样 "DNS: <name> = <ip>"；这三个名字一个都不许配上 " = "
        bogus = any(("%s = " % n) in body for n in ("a.test", "nx.test", "to.test"))
        res.append(("never claimed a successful resolve", not bogus))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_C[0])


def case_cache():
    """D：缓存三态 —— 命中不发包 / flush 后重新问 / TTL 过期后重新问

    反向组是第 3、4 条：如果缓存恒真（比如永远命中），flush 之后就不该
    再发查询；如果缓存恒假，第二次 lookup 就会在服务器上留第二个查询。
    """
    res = []
    proc = boot(WORK["D"], PORT_D)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_D[1])
        qmp = Qmp(PORT_D[0])
        srv = LanServer(PORT_D[2], dns_ip=DNS_LOCAL, ttl_map={"short.test": 1})
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL D: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got a lease first", ok))

        ok, out = run_cmd(qmp, serial, "lookup cache.test",
                          "dns: cache.test = 93.184.216.34", 30.0)
        res.append(("first lookup resolves", ok))
        res.append(("first lookup really went on the wire",
                    srv.count("cache.test") == 1))

        ok, out = run_cmd(qmp, serial, "lookup cache.test",
                          "dns: cache.test = 93.184.216.34 (cached)", 25.0)
        res.append(("second lookup says cached", ok))
        res.append(("cache hit sent no packet at all",
                    srv.count("cache.test") == 1))

        ok, out = run_cmd(qmp, serial, "dns flush", "dns: cache flushed", 20.0)
        res.append(("dns flush accepted", ok))
        ok, out = run_cmd(qmp, serial, "lookup cache.test",
                          "dns: cache.test = 93.184.216.34", 30.0)
        res.append(("after flush it asks the server again",
                    ok and "(cached)" not in out))
        res.append(("flush really re-queried", srv.count("cache.test") == 2))

        # TTL=1s 的名字：睡过 1s 之后必须失效（缓存若不看 TTL 这里会失败）
        ok, out = run_cmd(qmp, serial, "lookup short.test",
                          "dns: short.test = 93.184.216.34", 30.0)
        res.append(("short-TTL name resolves", ok))
        before = srv.count("short.test")
        ok, out = run_cmd(qmp, serial, "sleep 1500", "done", 90.0)
        res.append(("slept past the TTL", ok))
        ok, out = run_cmd(qmp, serial, "lookup short.test",
                          "dns: short.test = 93.184.216.34", 30.0)
        res.append(("expired entry resolves again", ok))
        res.append(("expired entry was re-queried (TTL honoured)",
                    srv.count("short.test") > before))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_D[0])


def case_multiserver():
    """E：option 6 给两台服务器，第一台装死 -> 必须回退到第二台"""
    res = []
    proc = boot(WORK["E"], PORT_E)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_E[1])
        qmp = Qmp(PORT_E[0])
        srv = LanServer(PORT_E[2], dns_ip=[DNS_LOCAL, DNS_BACKUP],
                        silent_srv=(DNS_LOCAL,))
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL E: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got a lease first", ok))
        ok, out = run_cmd(qmp, serial, "dns", "dns: server 10.0.2.3", 25.0)
        res.append(("option 6 delivered the first server", ok))
        ok, out = run_cmd(qmp, serial, "dns", "dns: server 10.0.2.4", 25.0)
        res.append(("option 6 delivered the second server", ok))

        ok, out = run_cmd(qmp, serial, "lookup www.example.com",
                          "dns: www.example.com = 93.184.216.34", 120.0)
        res.append(("resolved through the fallback server", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])
        res.append(("asked the first server first", srv.count_to(DNS_LOCAL) >= 1))
        res.append(("gave up on it and asked the second",
                    srv.count_to(DNS_BACKUP) >= 1))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_E[0])


def case_ping_host():
    """F：ping 接受域名（先解析再 ping）；解析失败就不许发 ICMP"""
    res = []
    proc = boot(WORK["F"], PORT_F)
    serial = qmp = srv = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_F[1])
        qmp = Qmp(PORT_F[0])
        srv = LanServer(PORT_F[2], dns_ip=DNS_LOCAL, nx=("nx.test",))
        srv.start()
        time.sleep(0.5)
        if not shell_up(serial, qmp):
            print("  FAIL F: shell did not come up")
            return [("shell up", False)]

        ok, out = run_cmd(qmp, serial, "dhcp", "dhcp: got", 40.0)
        res.append(("got a lease first", ok))

        ok, out = run_cmd(qmp, serial, "ping www.example.com",
                          "reply from 93.184.216.34", 60.0)
        res.append(("ping by hostname reaches the resolved address", ok))
        if not ok:
            print("---- serial tail ----\n" + out[-1200:])
        res.append(("printed the resolved address",
                    "ping: www.example.com = 93.184.216.34" in out))

        ok, out = run_cmd(qmp, serial, "ping 10.0.2.2",
                          "reply from 10.0.2.2", 60.0)
        res.append(("ping by literal IP still works", ok))

        ok, out = run_cmd(qmp, serial, "ping nx.test",
                          "ping: dns: no a record", 40.0)
        res.append(("unresolvable name reported as such", ok))
        res.append(("no ICMP went out for the bad name",
                    "reply from" not in out))
        return res
    finally:
        teardown(proc, qmp, serial, srv, PORT_F[0])


def main():
    print("== test_dns: DNS resolver (A record) ==")
    kill_all_qemu()
    total = failed = 0
    for name, fn in (("A same-subnet DNS", case_local),
                     ("B off-subnet DNS via gateway", case_remote),
                     ("C failure branches", case_failures),
                     ("D cache (hit / flush / TTL)", case_cache),
                     ("E multiple servers, fallback", case_multiserver),
                     ("F ping by hostname", case_ping_host)):
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
