import os, socket, subprocess, sys, time, threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4499
SERIAL_PORT = 4500
AHCI_IMG = os.path.join(HERE, "ahci.img").replace("\\", "/")
DISK_COPY = os.path.join(HERE, "disk_ahci.img").replace("\\", "/")

def make_blank(path, mb):
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)

class SR:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self._run, daemon=True).start()
    def _run(self):
        while self.running:
            try:
                d = self.sock.recv(8192)
            except Exception:
                break
            if not d:
                break
            with self.lock:
                self.buf += d
    def snap(self):
        with self.lock:
            return self.buf.decode("latin1", "replace")

if __name__ == "__main__":
    make_blank(AHCI_IMG, 64)
    if os.path.isfile(os.path.join(ROOT, "disk.img")):
        import shutil
        shutil.copyfile(os.path.join(ROOT, "disk.img"), DISK_COPY)
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + DISK_COPY,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "format=raw,if=none,file=" + AHCI_IMG + ",id=dahci",
        "-device", "ide-hd,drive=dahci,bus=ahci.0",
        "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 立即连接串口（带重试），避免错过早期 AHCI 初始化日志
    sr = None
    for _ in range(50):
        try:
            sr = SR(SERIAL_PORT)
            break
        except OSError:
            time.sleep(0.2)
    if sr is None:
        print("serial connect failed")
        proc.kill()
        sys.exit(1)
    time.sleep(30)
    out = sr.snap()
    with open(os.path.join(HERE, "ahci_serial.log"), "w") as f:
        f.write(out)
    print("WROTE ahci_serial.log, len=", len(out))
    sr.running = False
    try: sr.sock.close()
    except Exception: pass
    proc.kill()
