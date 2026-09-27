/*
 * uhci.c - UHCI 主机控制器初始化 + 端口连接检测（真机点亮 H2-2a）
 *
 * 设计范围（与任务严格对齐）：
 *  - 只读 PCI 配置空间定位 UHCI（class 0x0C / subclass 0x03 / progif 0x00）。
 *  - 取 BAR0：I/O 空间，基址 = bar0 & 0xFFFC。若 bar0 == 0 说明 BIOS/固件
 *    没给它编程，本模块自己写 PCI 配置空间 0x10 分配一个 I/O 基址（默认
 *    0xC000，按 BAR 请求的大小对齐边界），并置 COMMAND 的 IO/BusMaster 位。
 *  - 全局复位（GRESET，UHCI 1.1 spec 强制的全局复位位），每步 klog。
 *  - .bss 静态 4KB 帧列表（1024 项全置 T=1 终止，4KB 对齐），喂给 FLBASEADD；
 *    与 AHCI 的 DMA 缓冲同区，已验证对设备 DMA 一致，不依赖 pmm 池。
 *  - 启动调度（RS=1，MAXP=0→64 字节包），确认 HCHALTED 跑起来。
 *  - 轮询 2 个端口（PORTSC@0x10/0x12）的 CCS/LSDA/PR 位并 klog。
 *  - 不注册中断、不碰传输、不枚举设备。
 *
 * 寄存器全部是 I/O 空间（UHCI 规范强制 I/O，非 MMIO），偏移按 UHCI 1.1：
 *   0x00 USBCMD    (16-bit)  0x02 USBSTS  (16-bit)  0x04 USBINTR (16-bit)
 *   0x06 FRNUM     (16-bit)  0x08 FLBASEADD(32-bit)  0x0C SOFMOD  (8-bit)
 *   0x10 PORTSC1   (16-bit)  0x12 PORTSC2 (16-bit)
 *
 * USBCMD 位（UHCI 1.1 spec 3.2.1）：
 *   bit0  RS      Run/Stop             (1=run)
 *   bit1  HCRESET Host Controller Reset
 *   bit2  GRESET  Global Reset         —— 本步复位用这个（复位所有下行口）
 *   bit7  MAXP    Max Packet Size      (0=64B 默认, 1=32B)
 *
 * USBSTS 位（UHCI 1.1 spec 3.2.2）：
 *   bit0  USBINT    USB Interrupt
 *   bit1  USBERRINT USB Error Interrupt
 *   bit2  RD        Resume Detect
 *   bit3  HSE       Host System Error
 *   bit4  HCPE      Host Controller Process Error
 *   bit5  HCH       Host Controller Halted (1=halted, 0=running)
 *   实测校准：GRESET 后读回 USBSTS=0x0020（bit5=1，halted），写 RS=1 启动
 *   调度后读回 0x0000（bit5=0，running）——与 bit5=HCH 完全吻合，故用 0x0020。
 *
 * PORTSC 位（UHCI 1.1 spec 3.2.4）：
 *   bit0 CCS (Current Connect Status)   bit1 CSC (Connect Status Change)
 *   bit8 LSDA (Low Speed Device Attached)  bit9 PR (Port Reset)
 *
 * 红线落实：
 *  - 手写格式化（内核无 sprintf）：行缓冲 128 字节，全部写入以上界为准，
 *    末尾保 '\0'，绝不越界。
 *  - 不可信字段上界：BAR size 必须 >=4 且 <=64KB，越界 fail closed 跳过；
 *    irq==0xFF 或 >=16 视为无效打印 N/A。
 *  - 资源中途失败回滚：帧列表为 .bss 静态区无需分配；PCI BAR 未编程时若基址
 *    越界或非法立即放弃该控制器，不泄漏、不继续。
 *
 * TODO(缓存一致性)：identity 映射为 WB 可缓存回写（PTE_RW）。帧列表/TD/QH
 * 都是 DMA 双向，真机上必须映射为 UC 或写完做 wbinvd/clflush，否则 HC 作为
 * 总线主控读到的是陈旧缓存、甚至读到未写回的 0x00000000（bit0=0 非终止，
 * HC 会去地址 0 取 TD，可能触发 Host System Error）。本步 QEMU 不模拟缓存
 * 一致性（设备直接读 guest RAM），故无碍；真机点亮前必须在传输步骤处理。
 */
#include "uhci.h"
#include "pci.h"
#include "pmm.h"
#include "dmesg.h"
#include "port.h"
#include "isr.h"
#include "paging.h"
#include "types.h"

/* ---------- 手写格式化助手（无 sprintf，参考 usb.c 写法） ---------- */

static void u_app_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12];
    int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}

static void u_app_hex(char *b, int *n, int lim, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) {
        if (*n < lim) b[(*n)++] = hx[(v >> s) & 0xF];
    }
}

static void u_app_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

/* ---------- I/O 寄存器访问（UHCI 强制 I/O 空间） ---------- */
static inline uint16_t u_inw(uint16_t base, uint16_t off) {
    return inw((uint16_t)(base + off));
}
static inline void u_outw(uint16_t base, uint16_t off, uint16_t v) {
    outw((uint16_t)(base + off), v);
}
static inline uint32_t u_inl(uint16_t base, uint16_t off) {
    return inl((uint16_t)(base + off));
}
static inline void u_outl(uint16_t base, uint16_t off, uint32_t v) {
    outl((uint16_t)(base + off), v);
}

/* 以 g_pit_ticks 为基准的忙等（1ms 粒度，参考 ahci.c） */
static void u_delay_ms(uint32_t ms) {
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < ms) { }
}

/* =========================================================================
 * H2-2b：UHCI 控制传输骨架（队列 QH/TD 链表调度 + 轮询完成/超时/回滚 +
 * 端口 reset + 最小 GET_DESCRIPTOR / SET_ADDRESS 接口）。不做设备枚举全链路，
 * 不解析 HID 报告（那是以后的小目标）。
 *
 * 传输结构全部落在 .bss（identity 映射 == 物理地址，16 字节对齐满足 UHCI
 * 对 TD/QH 的对齐要求），HC 作为总线主控直接读这块 guest RAM。QEMU 不模拟
 * 缓存一致性，无需 wbinvd（真机点亮前须在传输步处理，见文件头 TODO）。
 * ========================================================================= */

/* TD token PID（UHCI 1.1 spec 3.2.3） */
#define UHCI_PID_SETUP  0x2Du
#define UHCI_PID_OUT    0xE1u
#define UHCI_PID_IN     0x69u

/* TD Control/Status 位（UHCI 1.1 spec 3.2.3；与 Linux uhci-hcd / QEMU 一致）。
 * 注意 ACTIVE 在 bit23（非 bit22），下方的"错误"位整体相对旧注释上移 1 位：
 * bit16 保留、bit17 位填充、bit18 CRC/超时、bit19 NAK、bit20 Babble、
 * bit21 数据缓冲错误、bit22 STALL、bit23 ACTIVE。HC 只认 bit23 的 ACTIVE，
 * 写错位置会导致 HC 把 TD 视为已完成/非活跃而整体跳过（表现为 TD 永不执行）。 */
#define UHCI_TD_ACTIVE   0x00800000u   /* bit23 活跃（HC 清 0 表示完成） */
#define UHCI_TD_STALL    0x00400000u   /* bit22 STALL */
#define UHCI_TD_DBERR    0x00200000u   /* bit21 数据缓冲错误 */
#define UHCI_TD_BABBLE   0x00100000u   /* bit20 Babble */
#define UHCI_TD_NAK      0x00080000u   /* bit19 NAK */
#define UHCI_TD_CRCTO    0x00040000u   /* bit18 CRC/超时 */
#define UHCI_TD_BITSTUFF 0x00020000u   /* bit17 位填充错误 */
#define UHCI_TD_LS       0x04000000u   /* bit26 低速设备 */
#define UHCI_TD_SPD      0x20000000u   /* bit29 短包检测(IN) */
#define UHCI_TD_ERR3     0x18000000u   /* bit27-28 = 3 次重试 */

/* 任一致命错误位置位即视为传输失败（fail closed） */
#define UHCI_TD_FATAL    (UHCI_TD_BITSTUFF | UHCI_TD_CRCTO | UHCI_TD_BABBLE \
                          | UHCI_TD_DBERR | UHCI_TD_STALL)

#define UHCI_XFER_TO_MS  1000u        /* 单笔控制传输超时（毫秒，g_pit_ticks 基准） */

/* 指针 -> 物理地址（identity 映射，虚拟地址即物理地址） */
#define U32PTR(p)       ((uint32_t)(unsigned long)(p))

/* 控制传输结构（.bss，16 字节对齐）。单 QH + 3 个 TD（SETUP/DATA/STATUS），
 * 同一时刻只跑一笔控制传输，故这些缓冲进程级复用、无需逐次分配。
 * H2-2c：数据缓冲 64 -> 256 字节——配置描述符（HID 键盘 34B、复合设备更大）
 * 一次取全需要 >64 的窗口；UHCI 的 TD maxlen 是 11 位字段（0x7FF 表示 0），
 * 255 完全在编码范围内，且 HC 会把多个最大包自动拼进同一个 TD 缓冲，
 * 低速设备（8 字节/包）也无需软件分包。 */
static volatile uint32_t g_qh[8]       __attribute__((aligned(16))); /* 32B QH */
static volatile uint32_t g_td[3][8]    __attribute__((aligned(16))); /* 3×32B TD */
static volatile uint8_t  g_setup[8]    __attribute__((aligned(16)));
static volatile uint8_t  g_rbuf[256]   __attribute__((aligned(16)));
#define UHCI_XFER_MAX    256            /* 数据阶段缓冲上界 */

/* H2-2d：中断传输结构（中断 QH + 1 个中断 TD + 报告缓冲）。
 *
 * 调度骨架（关键，改这里前先想清楚）：
 *   帧列表(1024 项) -> 中断 QH --(水平链 Q=1)--> 控制 QH
 *                         |                         |
 *                     中断 TD(Q=0 纵向)          控制 TD 链(Q=0 纵向)
 *   两个 QH 都是常驻的：帧列表初始化后**永不再改**，控制/中断传输只改各自
 *   QH 的 element 指针（挂上/撤下一个 dword）。这样两种传输互不踩帧列表，
 *   也不必每次传输重写 4KB 帧列表（shell 每秒轮询上千次时这点很要紧）。
 *   两个队列都空时 HC 只是每帧走过两个终止项，零开销。 */
static volatile uint32_t g_iqh[8]      __attribute__((aligned(16)));
static volatile uint32_t g_itd[8]      __attribute__((aligned(16)));
static volatile uint8_t  g_ibuf[16]    __attribute__((aligned(16)));
#define UHCI_INTR_TO_MS  3u             /* 中断轮询超时（毫秒） */

/* H1c：bulk 传输结构（bulk QH 常驻 + TD 池 + 数据缓冲）。
 *
 * 调度骨架更新（改这里前先想清楚）：
 *   帧列表 -> 中断 QH --Q=1--> 控制 QH --Q=1--> bulk QH --水平链--> T
 * 三个 QH 都常驻：帧列表初始化后永不再改；控制/中断/bulk 传输只改各自 QH 的
 * element（挂上/撤下一个 dword）。bulk 挂在水平链最末端——UHCI 带宽模型里
 * bulk 正是"用完中断+控制剩下的带宽"，QEMU 全速 1ms 帧足够单帧跑完一笔。
 *
 * 全速（12Mbps）bulk 端点 mps 上限 64 字节（低速无 bulk）。512B 扇区按
 * mps=64 拆 8 包；TD 池按 16 备（mps>=32 都能单笔 512B），越界 fail closed。
 * 数据缓冲 512B 放 .bss（identity 映射区，虚拟==物理，HC DMA 直接用）。 */
static volatile uint32_t g_bqh[8]       __attribute__((aligned(16)));
static volatile uint32_t g_btd[16][8]   __attribute__((aligned(16)));
static volatile uint8_t  g_bulkbuf[512] __attribute__((aligned(16)));
#define UHCI_BULK_TD_MAX 16
#define UHCI_BULK_TO_MS  1000u          /* 一笔 bulk 传输超时（毫秒） */
#define UHCI_BULK_NAK_MAX 2000          /* 单 TD NAK 重试上界（约 2s） */

/* bulk 端点 DATA toggle：按 USB 地址索引、IN/OUT 各一份（一设备最多一对
 * bulk 端点）。与中断端点的 g_toggle 相互独立——不同端点的 toggle 序列
 * 本来就是独立的。ClearFeature(HALT)/BOT reset 后必须双清零（规范要求
 * toggle 回到 DATA0）。 */
static uint8_t g_btog_in[128];
static uint8_t g_btog_out[128];

/* 中断端点的 DATA toggle（按 USB 地址索引，每设备一个中断端点够用）。
 * UHCI 不做自动 toggle：TD token 的 bit19 必须由软件给，给错设备就丢包。 */
static uint8_t g_toggle[128];

/* 帧列表：1024 项，4KB 对齐（UHCI 要求）。放在 .bss（与 AHCI 的 DMA 缓冲同区，
 * 已验证对设备 DMA 一致），不依赖 pmm 池——pmm 页在本 QEMU 下经 FLBASEADD
 * 给 HC 做 DMA 读时未被证实一致，故改用 BSS 静态区。 */
static volatile uint32_t g_frame[1024] __attribute__((aligned(4096)));
#define UHCI_FRAME_PHYS  ((uint32_t)(unsigned long)(&g_frame[0]))

/* 探针共享句柄：uhci_setup_one 写入，uhci_control_probe 读取 */
static uint16_t g_ctrl_io    = 0;     /* 控制器 I/O 基址（0=未初始化） */
static int      g_ctrl_port  = -1;    /* 首个已连接端口（-1=无） */
static int      g_ctrl_ls    = 0;     /* 该端口是否低速设备 */

/* H2-2c：已连接端口表（静态，无动态分配）。uhci_setup_one 期间填充，
 * 供上层枚举模块 usbenum 逐个消费。g_plist_n 为有效条数。 */
typedef struct {
    uint16_t io;        /* 控制器 I/O 基址 */
    int      port;      /* 根口编号（0/1） */
    int      ls;        /* 低速设备（LSDA） */
} uhci_port_t;
static uhci_port_t g_plist[UHCI_MAX_PORTS];
static int         g_plist_n = 0;

int uhci_port_count(void) {
    return g_plist_n;
}

int uhci_port_get(int i, uint16_t *io_out, int *port_out, int *ls_out) {
    if (i < 0 || i >= g_plist_n) return -1;
    if (io_out)   *io_out   = g_plist[i].io;
    if (port_out) *port_out = g_plist[i].port;
    if (ls_out)   *ls_out   = g_plist[i].ls;
    return 0;
}

/* 通用控制传输：SETUP + DATA + STATUS 三阶段，链表挂到单一 QH 上跑。
 * 返回 0 成功，<0 失败（超时/硬件错误）；绝不静默挂死（超时上界 fail closed）。
 * 不可信字段上界：addr<=127、ep<=15、blen∈[0,UHCI_XFER_MAX]，越界直接放弃。
 * H2-2b 起对外导出（uhci.h）：上层枚举模块（usbenum）靠它走 USB 标准请求。 */
int uhci_control_xfer(uint16_t io, uint8_t addr, uint8_t ep,
                      const uint8_t *setup, int dir_in,
                      uint8_t *buf, int blen, int lowspeed, int *actlen) {
    (void)io;   /* 控制传输由 HC 走调度（帧列表->QH->TD），不经直接端口 IO */
    if (actlen) *actlen = 0;
    if (addr > 127 || ep > 15) return -1;          /* 不可信上界 */
    if (blen < 0 || blen > UHCI_XFER_MAX) return -1; /* 缓冲上界（防写越界） */
    if (blen > 0 && !buf) return -1;

    /* SETUP 包拷到对齐缓冲；数据缓冲：IN 先清零避免读到陈旧值，OUT 拷入用户数据 */
    for (int i = 0; i < 8; i++) g_setup[i] = setup[i];
    for (int i = 0; i < blen; i++) g_rbuf[i] = dir_in ? 0 : buf[i];

    /* 清零 3 个 TD（含 HW 字段与保留位），避免残留上一次传输的标志 */
    for (int t = 0; t < 3; t++)
        for (int k = 0; k < 8; k++) g_td[t][k] = 0;

    uint32_t ls = lowspeed ? UHCI_TD_LS : 0u;

    /* TD0 SETUP：PID=SETUP，toggle=DATA0（token bit19=0，配 QH carry=0） */
    g_td[0][3] = U32PTR(&g_setup[0]);
    g_td[0][2] = UHCI_PID_SETUP
               | ((uint32_t)addr << 8)
               | ((uint32_t)ep    << 15)
               | (0u << 19)                   /* DATA0 */
               | ((uint32_t)7u << 21);        /* maxlen 字段 = 8-1（SETUP 恒 8B） */
    g_td[0][1] = UHCI_TD_ACTIVE | UHCI_TD_ERR3 | ls;
    /* 链接用 vertical(Q=0)：TD→TD 必须按纵向链走，Q=1 会被 HC 当成另一个
     * QH 而跳过整个队列（QEMU 帧循环以 Q 位判定 QH/TD）。帧列表项才用 Q=1。
     * 无数据阶段(blen==0，如 SET_ADDRESS)时 SETUP 直接链到 STATUS(TD2)。 */
    g_td[0][0] = (blen > 0) ? U32PTR(&g_td[1]) : U32PTR(&g_td[2]);

    /* TD1 DATA：方向依 dir_in；toggle=DATA1。仅在有数据阶段(blen>0)时构建；
     * 无数据阶段(如 SET_ADDRESS)时 TD1 保持全 0（ACTIVE=0），不参与完成判定，
     * 由 TD0 直接链到 TD2（STATUS）。 */
    if (blen > 0) {
        uint32_t data_pid = dir_in ? UHCI_PID_IN : UHCI_PID_OUT;
        uint32_t dlen = (uint32_t)(blen - 1);   /* maxlen 字段 = len-1 */
        g_td[1][3] = U32PTR(&g_rbuf[0]);
        g_td[1][2] = data_pid
                   | ((uint32_t)addr << 8)
                   | ((uint32_t)ep    << 15)
                   | (1u << 19)                   /* DATA1 */
                   | (dlen << 21);                /* maxlen = len-1 */
        g_td[1][1] = UHCI_TD_ACTIVE
                   | (dir_in ? UHCI_TD_SPD : 0u)  /* IN 开短包检测 */
                   | UHCI_TD_ERR3 | ls;
        g_td[1][0] = U32PTR(&g_td[2]);            /* ->TD2 (vertical) */
    }

    /* TD2 STATUS：方向取数据阶段反（IN数据->OUT状态，OUT数据->IN状态）；
     * 无数据阶段时 STATUS 直接接在 SETUP 后。toggle 恒用 DATA1（控制传输规定
     * SETUP=DATA0，其后首包与状态阶段均 DATA1，QH carry 关闭时由 token 位决定）。 */
    uint32_t sts_pid = dir_in ? UHCI_PID_OUT : UHCI_PID_IN;
    g_td[2][3] = 0;
    g_td[2][2] = sts_pid
               | ((uint32_t)addr << 8)
               | ((uint32_t)ep    << 15)
               | (1u << 19)                   /* DATA1 */
               | (0x7FFu << 21);               /* maxlen=0 字节的 UHCI 编码 */
    g_td[2][1] = UHCI_TD_ACTIVE | UHCI_TD_ERR3 | ls;
    g_td[2][0] = 0x1u;                        /* T terminate，链表尾 */

    /* 挂接控制队列：控制 QH 的 element -> TD0（Q=0 纵向）。
     * H2-2d 起**不再改写帧列表**——帧列表常驻指向中断 QH，中断 QH 水平链到
     * 控制 QH（见 g_iqh 处的骨架注释），控制传输只动自己 QH 的 element。
     * H1c 起控制 QH 的水平链后面还挂着常驻 bulk QH，这里**绝不能碰
     * g_qh[0]**（曾把它清成 terminate，把 bulk QH 从链上摘掉——症状是
     * 控制传输全部正常而 bulk 传输一个包都发不出去）。 */
    g_qh[1] = U32PTR(&g_td[0]);                  /* 元素指针 -> TD0 (vertical) */

    /* 轮询完成（以 g_pit_ticks 1ms 为基准，超时 fail closed 不挂死） */
    uint32_t t0 = g_pit_ticks;
    int done = 0;
    while ((uint32_t)(g_pit_ticks - t0) < UHCI_XFER_TO_MS) {
        uint32_t s0 = g_td[0][1];
        uint32_t s1 = g_td[1][1];
        uint32_t s2 = g_td[2][1];
        if (((s0 & UHCI_TD_ACTIVE) == 0) &&
            ((s1 & UHCI_TD_ACTIVE) == 0) &&
            ((s2 & UHCI_TD_ACTIVE) == 0)) { done = 1; break; }
        /* 任何致命错误位出现即提前结束（fail closed，不再空等） */
        if ((s0 & UHCI_TD_FATAL) || (s1 & UHCI_TD_FATAL) || (s2 & UHCI_TD_FATAL))
            break;
    }
    /* 不论成功失败，把控制 QH 的 element 拉回终止（T），防 HC 反复重跑本链路 */
    g_qh[1] = 0x1u;

    if (!done) {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: xfer timeout addr=");
        u_app_dec(line, &li, lim, (uint32_t)addr);
        u_app_str(line, &li, lim, " ep=");
        u_app_dec(line, &li, lim, (uint32_t)ep);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    /* 检查致命硬件错误位（fail closed，不打 silent） */
    uint32_t s0 = g_td[0][1], s1 = g_td[1][1], s2 = g_td[2][1];
    if ((s0 & UHCI_TD_FATAL) || (s1 & UHCI_TD_FATAL) || (s2 & UHCI_TD_FATAL)) {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: xfer error TD0=0x");
        u_app_hex(line, &li, lim, s0, 8);
        u_app_str(line, &li, lim, " TD1=0x");
        u_app_hex(line, &li, lim, s1, 8);
        u_app_str(line, &li, lim, " TD2=0x");
        u_app_hex(line, &li, lim, s2, 8);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    /* 取数据阶段实际长度（control/status dword bit0-10）并拷回（IN） */
    /* UHCI 实际长度字段同样编码为 length-1；0x7FF 表示零字节。 */
    uint32_t al_field = s1 & 0x7FFu;
    int al = (al_field == 0x7FFu) ? 0 : (int)al_field + 1;
    if (actlen) *actlen = al;
    if (dir_in && buf && al > 0) {
        int n = al; if (n > blen) n = blen;          /* 上界保护 */
        for (int i = 0; i < n; i++) buf[i] = (uint8_t)g_rbuf[i];
    }
    return 0;
}

/*
 * 中断 IN 轮询（H2-2d：HID 键盘的报告通道）。语义见 uhci.h。
 *
 * 与设备约定：
 *  - 没有新报告时设备回 NAK —— 这不是错误，返回 0 且 *actlen=0。
 *  - DATA toggle 必须软件维护（UHCI TD token bit19），且**只有真正收到数据
 *    才翻转**：NAK/零长度传输不算完成一次数据包，翻了就会与设备失步，
 *    表现为"第一次能读到报告，之后永远读不到"。
 *  - 轮询窗口 UHCI_INTR_TO_MS（6ms）：低速设备每帧最多一次事务，1ms/帧，
 *    6ms 足够 HC 重试完；超时 fail closed 返回 -1，绝不挂死。
 */
int uhci_interrupt_in(uint16_t io, uint8_t addr, uint8_t ep,
                      uint8_t *buf, int blen, int lowspeed, int *actlen) {
    (void)io;   /* same as control xfer: routed by the schedule, not port IO */
    if (actlen) *actlen = 0;
    if (!buf || addr == 0u || addr > 127u) return -1;
    if ((ep & 0x80u) == 0u || (ep & 0x7Fu) > 15u) return -1;  /* 必须是 IN 端点 */
    if (blen < 1 || blen > (int)sizeof(g_ibuf)) return -1;    /* 缓冲上界 */

    uint32_t ls = lowspeed ? UHCI_TD_LS : 0u;
    uint32_t tog = g_toggle[addr] ? (1u << 19) : 0u;   /* DATA0/DATA1 */

    for (int i = 0; i < (int)sizeof(g_ibuf); i++) g_ibuf[i] = 0;

    g_itd[0] = 0x1u;                                    /* T：链表尾 */
    g_itd[1] = UHCI_TD_ACTIVE | UHCI_TD_SPD | UHCI_TD_ERR3 | ls;
    g_itd[2] = UHCI_PID_IN
             | ((uint32_t)addr << 8)
             | (((uint32_t)(ep & 0x7Fu)) << 15)
             | tog
             | (((uint32_t)(blen - 1)) << 21);          /* maxlen = len-1 */
    g_itd[3] = U32PTR(&g_ibuf[0]);

    /* 挂上：中断 QH 的 element -> 中断 TD（Q=0 纵向） */
    g_iqh[1] = U32PTR(&g_itd[0]);

    uint32_t t0 = g_pit_ticks;
    int done = 0;
    while ((uint32_t)(g_pit_ticks - t0) < UHCI_INTR_TO_MS) {
        uint32_t s = g_itd[1];
        if ((s & UHCI_TD_ACTIVE) == 0u) { done = 1; break; }
        if (s & UHCI_TD_FATAL) break;                   /* fail closed，不空等 */
    }
    /* 撤下：防 HC 反复重跑同一个已完成的 TD */
    g_iqh[1] = 0x1u;

    uint32_t st = g_itd[1];
    if (!done) return -1;
    if (st & UHCI_TD_FATAL) return -1;

    uint32_t al_field = st & 0x7FFu;
    int al = (al_field == 0x7FFu) ? 0 : (int)al_field + 1;
    if (al <= 0) return 0;                              /* NAK / 零长度：无数据 */

    g_toggle[addr] = (uint8_t)(g_toggle[addr] ^ 1u);    /* 收到数据才翻 toggle */
    int n = al;
    if (n > blen) n = blen;
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)g_ibuf[i];
    if (actlen) *actlen = n;
    return 0;
}

/*
 * 一笔 bulk 传输（H1c：USB Mass Storage 的 BOT 通道）。
 *
 * 方向由 ep 的方向位决定（0x01=OUT，0x81=IN），与 interrupt_in 一致。
 * 数据阶段按 mps 拆包逐包发：每包一个 TD（toggle 软件交替），TD 纵向链在
 * 常驻 bulk QH 下，传完撤下。语义要点：
 *  - NAK 不算错误：BOT 在处理 CBW/数据/CSW 时设备可长时间 NAK，TD 保持
 *    ACTIVE 由 HC 逐帧重试（QEMU 如此），真机若报"完成+NAK"则软件重激活，
 *    重试上界 UHCI_BULK_NAK_MAX（约 2s），超时 fail closed。
 *  - toggle 只在事务真正完成时翻转（NAK 不翻），且 IN/OUT 各自独立。
 *  - IN 短包（实收 < 请求）= 数据阶段提前结束，后续 TD 不再执行（BOT 的
 *    dCBWDataTransferLength 精确约定了数据量，少发即错误，按实际收到算）。
 *  - STALL = 端点挂起，返回 <0，由上层走 BOT Reset + ClearFeature(HALT)
 *    恢复（恢复后必须调 uhci_bulk_tog_reset 把 toggle 清回 DATA0）。
 * 返回 0 成功（*actlen=实际字节数），<0 失败。
 */
int uhci_bulk_xfer(uint16_t io, uint8_t addr, uint8_t ep,
                   uint8_t *buf, int blen, int mps,
                   int lowspeed, int *actlen) {
    (void)io;   /* 与控制传输相同：走常驻调度，不经直接端口 IO */
    if (actlen) *actlen = 0;
    if (addr == 0u || addr > 127u) return -1;
    if (blen < 0 || blen > (int)sizeof(g_bulkbuf)) return -1;   /* 缓冲上界 */
    if (blen > 0 && !buf) return -1;
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) return -1;
    if ((ep & 0x7Fu) == 0u || (ep & 0x7Fu) > 15u) return -1;   /* 非端点0 */

    int dir_in = (ep & 0x80u) ? 1 : 0;
    uint8_t *togp = dir_in ? &g_btog_in[addr] : &g_btog_out[addr];
    uint32_t tog = *togp ? (1u << 19) : 0u;

    /* 拆包：n = ceil(blen/mps)。TD 池上界校验（mps=8 且 512B 时 n=64>16，
     * fail closed——MSC 端点 mps 恒为 64，这只在坏配置下才会发生）。 */
    int n = 0;
    if (blen > 0) {
        n = (blen + mps - 1) / mps;
        if (n > UHCI_BULK_TD_MAX) return -1;
    }
    if (blen == 0) return 0;                     /* 零数据阶段：无事可做 */

    /* 数据搬进 DMA 缓冲（IN 清零防陈旧值，OUT 拷入待发数据） */
    for (int i = 0; i < blen; i++) g_bulkbuf[i] = dir_in ? 0 : buf[i];

    uint32_t ls = lowspeed ? UHCI_TD_LS : 0u;    /* 低速无 bulk，保守保留 */
    uint32_t pid = dir_in ? UHCI_PID_IN : UHCI_PID_OUT;
    int remain = blen;

    for (int i = 0; i < n; i++) {
        int want = (remain > mps) ? mps : remain;
        remain -= want;
        for (int k = 0; k < 8; k++) g_btd[i][k] = 0;
        g_btd[i][3] = U32PTR(&g_bulkbuf[i * mps]);
        g_btd[i][2] = pid
                   | ((uint32_t)addr << 8)
                   | (((uint32_t)(ep & 0x7Fu)) << 15)
                   | tog
                   | (((uint32_t)(want - 1)) << 21);      /* maxlen = len-1 */
        g_btd[i][1] = UHCI_TD_ACTIVE | UHCI_TD_ERR3 | ls
                   | (dir_in ? UHCI_TD_SPD : 0u);
        g_btd[i][0] = (i + 1 < n) ? U32PTR(&g_btd[i + 1]) : 0x1u;
        tog ^= (1u << 19);                                 /* 后续包翻转 */
    }

    /* 挂上 bulk QH（element -> TD0，Q=0 纵向），逐包串行等待 */
    g_bqh[1] = U32PTR(&g_btd[0]);

    int total = 0;
    int fail = 0;
    for (int i = 0; i < n && !fail; i++) {
        uint32_t t0 = g_pit_ticks;
        int naks = 0;
        int done = 0;
        while ((uint32_t)(g_pit_ticks - t0) < UHCI_BULK_TO_MS) {
            uint32_t st = g_btd[i][1];
            if (st & UHCI_TD_ACTIVE) continue;             /* HC 还在跑 */
            if (st & UHCI_TD_NAK) {
                /* 设备忙：规范里 NAK 的 TD 置完成+NAK 位，HC 不再重试——
                 * 软件重激活同一个 TD（toggle/token 不变，设备期待的
                 * toggle 序列也没变），上界防设备永久忙。 */
                if (++naks > UHCI_BULK_NAK_MAX) { fail = 1; break; }
                g_btd[i][1] = (st & ~(uint32_t)UHCI_TD_NAK & ~0x7FFu)
                            | UHCI_TD_ACTIVE | UHCI_TD_ERR3 | ls
                            | (dir_in ? UHCI_TD_SPD : 0u);
                continue;
            }
            if (st & (UHCI_TD_FATAL | UHCI_TD_STALL)) { fail = 1; break; }
            done = 1;
            break;
        }
        if (!done) { fail = 1; break; }

        uint32_t st = g_btd[i][1];
        uint32_t al = st & 0x7FFu;
        int got = (al == 0x7FFu) ? 0 : (int)al + 1;
        total += got;
        *togp ^= 1u;                                       /* 事务完成，翻 toggle */

        int want = (blen - total + got > mps) ? mps : (blen - total + got);
        if (dir_in && got < want) break;                   /* IN 短包：阶段结束 */
    }

    /* 撤下：防 HC 反复重跑已完成的 TD 链 */
    g_bqh[1] = 0x1u;

    if (fail) {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-BULK: xfer fail addr=");
        u_app_dec(line, &li, lim, (uint32_t)addr);
        u_app_str(line, &li, lim, " ep=0x");
        u_app_hex(line, &li, lim, (uint32_t)ep, 2);
        u_app_str(line, &li, lim, " want=");
        u_app_dec(line, &li, lim, (uint32_t)blen);
        u_app_str(line, &li, lim, " got=");
        u_app_dec(line, &li, lim, (uint32_t)total);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    if (dir_in && total > 0) {
        int cpy = total;
        if (cpy > blen) cpy = blen;
        for (int i = 0; i < cpy; i++) buf[i] = (uint8_t)g_bulkbuf[i];
    }
    if (actlen) *actlen = total;
    return 0;
}

/* BOT reset / ClearFeature(HALT) 之后调用：bulk 两个方向的 toggle 清回
 * DATA0（USB 规范要求端点复位时 toggle 重置）。 */
void uhci_bulk_tog_reset(uint8_t addr) {
    if (addr > 127u) return;
    g_btog_in[addr]  = 0;
    g_btog_out[addr] = 0;
}

/* 确保 HC 处于运行态（见 uhci.h 注释）：GRESET/端口复位之后、发起传输之前
 * 必须调一次，否则 CF/RS 被清掉时 HC 只发 SOF 不执行 TD。 */int uhci_hc_start(uint16_t io) {
    u_outl(io, 0x08, UHCI_FRAME_PHYS);   /* 重锁存帧列表物理地址 */
    uint16_t cmd = u_inw(io, 0x00);
    u_outw(io, 0x00, (uint16_t)(cmd | 0x0001u | 0x0100u));  /* RS=1, CF=1 */
    u_delay_ms(2);
    uint16_t sts = u_inw(io, 0x02);
    return (sts & 0x0020u) ? -1 : 0;     /* bit5 HCH=1 仍 halted */
}

/* 端口级复位：置 PR(bit9)=1 保持 >=10ms 再清 0，使设备进入默认态（地址 0）。
 * QEMU UHCI 端口位（与官方 uhci-regs.h 一致）：
 *   bit0 CCS 连接(只读)  bit1 CSC 连接变化(写1清)  bit2 EN 端口使能(可写)
 *   bit3 ENC 使能变化(写1清)  bit8 LSDA 低速(只读)  bit9 RESET 复位
 *   bit12 SUSPEND 挂起(可写，写0即清)
 * 关键陷阱：QEMU 的 UHCI_PORT_READ_ONLY=0x1BB（不含 bit2/bit9/bit12），故任何
 * 端口写都会把"不在 val 里的可写位"清掉；且 QEMU 在 PR 1->0 后【不会自动使能】
 * 端口——EN(bit2) 会被清 0，必须软件显式写 1 重新使能。否则端口保持禁用态，
 * HC 永不向该口发事务，表现为 TD 永不执行、Active 不清除、无错误位（"死寂"）。
 * 故清 PR 那一步必须同时清 SUSPEND(bit12) 并置 EN(bit2)=1。端口无设备时(QEMU
 * 在写时按 CCS 判定) EN 会被忽略，属正常——fail closed 跳过。
 * 返回复位后是否仍连接（CCS bit0）；不连设备时返回 0。
 * H2-2b 起对外导出（uhci.h）：枚举前必须先复位，让设备回到默认态/地址 0。 */
int uhci_port_reset(uint16_t io, int p) {
    uint16_t poff = (uint16_t)(0x10 + p * 2);
    uint16_t pv = u_inw(io, poff);
    /* 置 PR(bit9)=1 触发设备复位（QEMU 在 PR 上升沿调用 usb_device_reset）。
     * 保留其余位（含 SUSPEND），不在此清除。 */
    u_outw(io, poff, (uint16_t)(pv | 0x0200u));    /* PR=1 */
    u_delay_ms(50);                                /* 规范 >=10ms，留余量 */
    pv = u_inw(io, poff);
    /* 清 PR(bit9)=0；同时清 SUSPEND(bit12)=0（写未含该位即清）、置 EN(bit2)=1
     * 显式使能端口。若端口已连接(CCS)，QEMU 允许置 EN，端口进入使能态。 */
    u_outw(io, poff, (uint16_t)((pv & ~0x0200u & ~0x1000u) | 0x0004u));
    u_delay_ms(100);                               /* 等端口重新使能 + 设备稳定 */
    pv = u_inw(io, poff);
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: port");
        u_app_dec(line, &li, lim, (uint32_t)p);
        u_app_str(line, &li, lim, " postreset conn=");
        u_app_dec(line, &li, lim, (pv & 0x0001u) ? 1u : 0u);
        u_app_str(line, &li, lim, " en=");
        u_app_dec(line, &li, lim, (pv & 0x0004u) ? 1u : 0u);
        u_app_str(line, &li, lim, " susp=");
        u_app_dec(line, &li, lim, (pv & 0x1000u) ? 1u : 0u);
        u_app_str(line, &li, lim, " raw=0x");
        u_app_hex(line, &li, lim, (uint32_t)pv, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
    return (pv & 0x0001u) ? 1 : 0;
}

/* 最小枚举探针：对首个已连接端口做
 *   GET_DESCRIPTOR(Device) @addr0 -> SET_ADDRESS(1) -> GET_DESCRIPTOR @addr1
 * 仅读设备描述符前几个字段作证据（不解析 HID 报告、不做完整枚举全链路）。
 * 不插设备时干净跳过（fail closed，绝不误报成功）。 */
static void uhci_control_probe(void) {
    if (g_ctrl_io == 0 || g_ctrl_port < 0) return;
    uint16_t io = g_ctrl_io;
    int port = g_ctrl_port;

    /* 仅对已连接端口做复位（空口复位无意义，跳过） */
    int conn = uhci_port_reset(io, port);
    if (!conn) {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: port");
        u_app_dec(line, &li, lim, (uint32_t)port);
        u_app_str(line, &li, lim, " no device after reset, skip probe");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return;
    }

    /* 确保 HC 锁存 BSS 帧列表并处于运行状态（GRESET 已清 CF/RS，必须重设；
     * 每帧链表挂接由 uhci_control_xfer 在每次传输时设置）。 */
    {
        u_outl(io, 0x08, UHCI_FRAME_PHYS);          /* 重锁存帧列表物理地址 */
        uint16_t cmd = u_inw(io, 0x00);
        u_outw(io, 0x00, (uint16_t)(cmd | 0x0001u | 0x0100u));   /* RS=1, CF=1 */
        u_delay_ms(2);
        uint16_t sts  = u_inw(io, 0x02);
        uint16_t fr   = u_inw(io, 0x06);
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: hc flba=0x");
        u_app_hex(line, &li, lim, UHCI_FRAME_PHYS, 8);
        u_app_str(line, &li, lim, " STS=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts, 4);
        u_app_str(line, &li, lim, " FRNUM=0x");
        u_app_hex(line, &li, lim, (uint32_t)fr, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    int ls = g_ctrl_ls;

    /* 1) GET_DESCRIPTOR(Device, idx0, len18) @addr0 */
    uint8_t req[8]  = {0x80,0x06,0x00,0x01,0x00,0x00,0x12,0x00};
    uint8_t dbuf[64]; int act = 0;
    int r = uhci_control_xfer(io, 0, 0, req, 1, dbuf, 18, ls, &act);
    if (r == 0 && act >= 8) {
        uint16_t bcd  = (uint16_t)(dbuf[2] | (dbuf[3] << 8));
        uint16_t vid  = (uint16_t)(dbuf[8] | (dbuf[9] << 8));
        uint16_t pid  = (uint16_t)(dbuf[10] | (dbuf[11] << 8));
        uint8_t  dcls = dbuf[4];
        uint8_t  ncfg = dbuf[17];
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: GET_DESCRIPTOR dev @0 ok len=");
        u_app_dec(line, &li, lim, (uint32_t)act);
        u_app_str(line, &li, lim, " bcdUSB=0x");
        u_app_hex(line, &li, lim, (uint32_t)bcd, 4);
        u_app_str(line, &li, lim, " VID=0x");
        u_app_hex(line, &li, lim, (uint32_t)vid, 4);
        u_app_str(line, &li, lim, " PID=0x");
        u_app_hex(line, &li, lim, (uint32_t)pid, 4);
        u_app_str(line, &li, lim, " cls=");
        u_app_hex(line, &li, lim, (uint32_t)dcls, 2);
        u_app_str(line, &li, lim, " ncfg=");
        u_app_dec(line, &li, lim, (uint32_t)ncfg);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    } else {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: GET_DESCRIPTOR @0 failed act=");
        u_app_dec(line, &li, lim, (uint32_t)act);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* 2) SET_ADDRESS(1) @addr0（数据阶段 OUT 无数据；状态阶段 IN） */
    uint8_t sa[8] = {0x00,0x05,0x01,0x00,0x00,0x00,0x00,0x00};
    int a = 0;
    /* blen=0（无数据阶段），缓冲传 NULL 即可（xfer 只在 blen>0 时要求非 NULL） */
    int rs = uhci_control_xfer(io, 0, 0, sa, 0, NULL, 0, ls, &a);
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: SET_ADDRESS(1) ");
        u_app_str(line, &li, lim, (rs == 0) ? "ok" : "fail");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* 3) GET_DESCRIPTOR(Device) @addr1（确认新地址生效） */
    uint8_t req2[8] = {0x80,0x06,0x00,0x01,0x00,0x00,0x12,0x00};
    uint8_t dbuf2[64]; int act2 = 0;
    int r2 = uhci_control_xfer(io, 1, 0, req2, 1, dbuf2, 18, ls, &act2);
    if (r2 == 0 && act2 >= 8) {
        uint16_t vid = (uint16_t)(dbuf2[8] | (dbuf2[9] << 8));
        uint16_t pid = (uint16_t)(dbuf2[10] | (dbuf2[11] << 8));
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: GET_DESCRIPTOR @1 ok VID=0x");
        u_app_hex(line, &li, lim, (uint32_t)vid, 4);
        u_app_str(line, &li, lim, " PID=0x");
        u_app_hex(line, &li, lim, (uint32_t)pid, 4);
        u_app_str(line, &li, lim, " len=");
        u_app_dec(line, &li, lim, (uint32_t)act2);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    } else {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI-CTRL: GET_DESCRIPTOR @1 failed act=");
        u_app_dec(line, &li, lim, (uint32_t)act2);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
}

/* ---------- 单个 UHCI 控制器初始化（返回 0=成功, -1=放弃） ---------- */
static int uhci_setup_one(const pci_device_t *d) {
    /* 不可信字段上界：bus/dev/func 来自配置空间，先夹紧避免越界访问 */
    uint8_t bus  = d->bus;
    uint8_t dev  = d->dev;
    uint8_t func = d->func;
    if (dev > 31 || func > 7) return -1;     /* fail closed：合法范围外直接放弃 */

    /* 找 UHCI 的 I/O 类型 BAR。UHCI 规范强制 I/O 空间，但不同实现把 I/O BAR
     * 放在不同索引：标准 UHCI 用 BAR0，而 QEMU 的 piix3-usb-uhci 放在 BAR4
     *（实测 query-pci 显示其 I/O region 在 bar=4）。所以扫描全部 6 个 BAR 取
     * 第一个 I/O 类型（bit0=1），不硬编码 BAR0，避免读错寄存器基址。 */
    uint32_t bar0 = 0;
    int bar_idx = -1;
    for (int b = 0; b < 6; b++) {
        if ((d->bar[b] & 0x1u) == 0x1u) { bar0 = d->bar[b]; bar_idx = b; break; }
    }
    uint16_t io = 0;
    int bar_ok = 0;

    if (bar_idx < 0) {
        /* 没有任何 I/O 类型 BAR：UHCI 必须 I/O，fail closed 跳过本控制器 */
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: no I/O BAR found, skip");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    uint32_t bar_off = (uint32_t)PCI_REG_BAR0 + (uint32_t)bar_idx * 4u;

    if (bar0 == 0) {
        /* 该 I/O BAR 未被编程（SeaBIOS/固件未分配 I/O 基址）。自己探测大小并分配。 */
        {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: BAR");
            u_app_dec(line, &li, lim, (uint32_t)bar_idx);
            u_app_str(line, &li, lim, "==0 (unprogrammed), assigning I/O base");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }

        /* 探测 I/O 窗口大小：写全 1 读回掩码（标准 BAR sizing）。
         * 先备份原始值，探测完立即还原，再写最终基址，避免中间态被 HC 误解。 */
        uint32_t orig = pci_read_dword(bus, dev, func, (uint8_t)bar_off);
        pci_write_dword(bus, dev, func, (uint8_t)bar_off, 0xFFFFFFFFu);
        uint32_t rdbk = pci_read_dword(bus, dev, func, (uint8_t)bar_off);
        pci_write_dword(bus, dev, func, (uint8_t)bar_off, orig);

        uint32_t size_mask;
        uint32_t size;
        if ((rdbk & 0x1u) == 0x1u) {
            /* 标准 I/O BAR：bit0=1 表示 I/O 空间，高 30 位是大小掩码 */
            size_mask = rdbk & 0xFFFFFFFCu;
            size = (~size_mask + 1u);          /* 2 的幂，DMA 窗口大小 */
        } else {
            /* QEMU 的 piix3-usb-uhci 不对 I/O BAR 实现标准 sizing 读回
             *（写全 1 读回 0）。UHCI 规范强制 I/O、寄存器窗口固定 32 字节，
             * 用 UHCI 标准默认 0x20，不因此 fail closed 放弃本控制器。 */
            size_mask = 0u;
            size = 0x20u;
            {
                char line[128];
                int li = 0; int lim = (int)sizeof(line) - 1;
                u_app_str(line, &li, lim, "UHCI: BAR size probe unreliable (rdbk=0x");
                u_app_hex(line, &li, lim, rdbk, 8);
                u_app_str(line, &li, lim, "), use UHCI default 0x20");
                if (li < lim) line[li] = 0; else line[lim] = 0;
                dmesg_write(line);
            }
        }

        /* 不可信上界 + 合法性：size 必须是 4..64KB 的 2 的幂；否则回退默认 */
        if (size < 4u || size > 0x10000u || (size & (size - 1u)) != 0u) {
            size = 0x20u;
            size_mask = 0u;
        }

        /* 建议基址 0xC000，向上对齐到 size 边界（保证 I/O 解码不跨边界） */
        uint32_t base = 0xC000u;
        uint32_t align = size - 1u;
        base = (base + align) & ~align;
        /* 上界：基址 + 窗口不能越过 64KB I/O 空间，且不能回绕 */
        if (base + size > 0x10000u || base == 0) {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: base 0x");
            u_app_hex(line, &li, lim, base, 4);
            u_app_str(line, &li, lim, " out of I/O range, skip");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
            return -1;
        }

        /* 写回基址（保留 I/O 类型位 bit0=1），并开启 IO 解码 + 总线主控 */
        pci_write_dword(bus, dev, func, (uint8_t)bar_off, base | 0x1u);
        uint32_t cmd = pci_read_dword(bus, dev, func, PCI_REG_COMMAND);
        cmd |= (uint32_t)(PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
        pci_write_dword(bus, dev, func, PCI_REG_COMMAND, cmd);

        io = (uint16_t)base;
        bar_ok = 1;

        {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: assigned io=0x");
            u_app_hex(line, &li, lim, (uint32_t)base, 4);
            u_app_str(line, &li, lim, " (BAR");
            u_app_dec(line, &li, lim, (uint32_t)bar_idx);
            u_app_str(line, &li, lim, ") size=0x");
            u_app_hex(line, &li, lim, size, 4);
            u_app_str(line, &li, lim, " mask=0x");
            u_app_hex(line, &li, lim, size_mask, 8);
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
    } else {
        /* I/O BAR 已编程：直接用，并补开 IO/BM 位（防御性） */
        io = (uint16_t)(bar0 & 0xFFFCu);
        uint32_t cmd = pci_read_dword(bus, dev, func, PCI_REG_COMMAND);
        if ((cmd & (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER)) !=
            (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER)) {
            cmd |= (uint32_t)(PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
            pci_write_dword(bus, dev, func, PCI_REG_COMMAND, cmd);
        }
        bar_ok = 1;
        {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: using preprogrammed io=0x");
            u_app_hex(line, &li, lim, (uint32_t)io, 4);
            u_app_str(line, &li, lim, " (BAR");
            u_app_dec(line, &li, lim, (uint32_t)bar_idx);
            u_app_str(line, &li, lim, ")");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
    }

    if (!bar_ok) return -1;

    /* ---------- 全局复位（GRESET, bit2） ---------- */
    uint16_t cmd = u_inw(io, 0x00);
    u_outw(io, 0x00, (uint16_t)(cmd | 0x0004u));   /* GRESET=1 */
    u_delay_ms(20);                                 /* 规范：保持 >=10ms */
    cmd = u_inw(io, 0x00);
    u_outw(io, 0x00, (uint16_t)(cmd & ~0x0004u));   /* GRESET=0 */
    u_delay_ms(2);

    /* 复位后应处于 Halted（HCH bit5=1）。读回确认，打日志（不强制 abort） */
    uint16_t sts = u_inw(io, 0x02);
    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: reset done, USBSTS=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts, 4);
        u_app_str(line, &li, lim, " HCHALTED=");
        u_app_dec(line, &li, lim, (sts & 0x0020u) ? 1u : 0u);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* ---------- 建立帧列表（BSS，4KB 对齐，与 AHCI DMA 缓冲同区，已验证对
     * 设备 DMA 一致；不依赖 pmm 池，免去资源失败回滚的额外复杂度） ---------- */
    /* 建立帧列表 + 常驻调度骨架（见 g_iqh 处的骨架注释）：
     *   帧列表全部 1024 项 -> 中断 QH(Q=1) -> 水平链 -> 控制 QH(Q=1)
     * 两个 QH 的 element 初始都是 T（空队列），后续各传各的、只改自己那一个
     * dword。帧列表建好之后**永不改写**，也不必每次传输重刷 4KB。 */
    g_iqh[0] = U32PTR(&g_qh[0]) | 0x2u;          /* 水平链 -> 控制 QH (Q=1) */
    g_iqh[1] = 0x1u;                             /* element = T（暂无中断 TD） */
    g_qh[0]  = U32PTR(&g_bqh[0]) | 0x2u;         /* 控制 QH 水平链 -> bulk QH */
    g_qh[1]  = 0x1u;                             /* element = T（暂无控制 TD） */
    g_bqh[0] = 0x1u;                             /* bulk QH 水平链终止 */
    g_bqh[1] = 0x1u;                             /* element = T（暂无 bulk TD） */
    for (int k = 0; k < 1024; k++)
        g_frame[k] = U32PTR(&g_iqh[0]) | 0x2u;   /* frame -> 中断 QH (Q=1) */
    u_outl(io, 0x08, UHCI_FRAME_PHYS);          /* FLBASEADD = 帧列表物理地址 */
    u_outw(io, 0x06, 0x0000);                   /* FRNUM = 0 */

    /* ---------- 启动调度（RS=1, CF=1, MAXP=0→64 字节包） ----------
     * CF（Configure Flag, bit8）必须置 1：UHCI 规范要求 CF=1 时 HC 才向设备
     * 发送 USB 令牌/事务；CF=0 时 HC 只发 SOF、不处理 TD 队列（表现就是
     * 调度在跑、FRNUM 前进，但 TD 永远不执行、无错误位）。 */
    uint16_t cmd2 = u_inw(io, 0x00);
    cmd2 = (uint16_t)((cmd2 | 0x0001u | 0x0100u) & ~0x0080u);  /* RS=1, CF=1, MAXP=0 */
    u_outw(io, 0x00, cmd2);
    u_delay_ms(2);

    uint16_t sts2 = u_inw(io, 0x02);
    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: schedule started, USBSTS=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts2, 4);
        u_app_str(line, &li, lim, " HCHALTED=");
        u_app_dec(line, &li, lim, (sts2 & 0x0020u) ? 1u : 0u); /* 1=halted */
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* ---------- 轮询端口连接状态（PIIX3 UHCI 固定 2 端口） ---------- */
    int first_conn = -1;     /* 记录首个已连接端口，交 H2-2b 探针使用 */
    int first_ls   = 0;
    for (int p = 0; p < 2; p++) {
        uint16_t poff = (uint16_t)(0x10 + p * 2);

        /* GRESET 把各端口也带入复位态，UHCI 规范标准流程需各自 Port Reset
         *（PR bit9 置 1 保持 >=10ms 再清零）让端口重新建立连接态并回读 CCS。
         * 这只是端口级复位，不做 SET_ADDRESS/GET_DESCRIPTOR，不算设备枚举。 */
        uint16_t pv = u_inw(io, poff);
        u_outw(io, poff, (uint16_t)(pv | 0x0200u));    /* PR=1 */
        u_delay_ms(20);
        pv = u_inw(io, poff);
        /* 清 PR 同时显式使能端口(EN bit2)并清 SUSPEND(bit12)，否则 QEMU 下端口
         * 保持禁用，HC 不会向该口发事务（见 uhci_port_reset 注释）。 */
        u_outw(io, poff, (uint16_t)((pv & ~0x0200u & ~0x1000u) | 0x0004u));
        u_delay_ms(20);
        pv = u_inw(io, poff);

        uint32_t conn = (pv & 0x0001u) ? 1u : 0u;   /* bit0 CCS */
        uint32_t ls   = (pv & 0x0100u) ? 1u : 0u;   /* bit8 LSDA */
        uint32_t rst  = (pv & 0x0200u) ? 1u : 0u;   /* bit9 PR */
        if (conn && first_conn < 0) { first_conn = p; first_ls = (int)ls; }
        /* H2-2c：登记进端口表供枚举模块消费（表满则丢弃多余条目，
         * 不扩表、不动态分配——UHCI 根口本来只有 2 个）。 */
        if (conn && g_plist_n < UHCI_MAX_PORTS) {
            g_plist[g_plist_n].io   = io;
            g_plist[g_plist_n].port = p;
            g_plist[g_plist_n].ls   = (int)ls;
            g_plist_n++;
        }
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: port");
        u_app_dec(line, &li, lim, (uint32_t)p);
        u_app_str(line, &li, lim, " conn=");
        u_app_dec(line, &li, lim, conn);
        u_app_str(line, &li, lim, " ls=");
        u_app_dec(line, &li, lim, ls);
        u_app_str(line, &li, lim, " rst=");
        u_app_dec(line, &li, lim, rst);
        u_app_str(line, &li, lim, " raw=0x");
        u_app_hex(line, &li, lim, (uint32_t)pv, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* 把首个已连接端口的句柄交给 H2-2b 控制传输探针 */
    g_ctrl_io    = io;
    g_ctrl_port  = first_conn;
    g_ctrl_ls    = first_ls;

    /* ---------- 汇总行 ---------- */
    uint16_t sts_f = u_inw(io, 0x02);
    uint8_t irq = d->intr_line;
    int irq_valid = (irq != 0xFFu) && (irq < 16u);
    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: io=0x");
        u_app_hex(line, &li, lim, (uint32_t)io, 4);
        u_app_str(line, &li, lim, " irq=");
        if (irq_valid) u_app_dec(line, &li, lim, (uint32_t)irq);
        else           u_app_str(line, &li, lim, "N/A");
        u_app_str(line, &li, lim, " frame=0x");
        u_app_hex(line, &li, lim, UHCI_FRAME_PHYS, 8);
        u_app_str(line, &li, lim, " sts=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts_f, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    return 0;
}

/* ---------- UHCI 初始化入口 ---------- */
void uhci_init(void) {
    int n = pci_device_count();
    int found = 0;
    int handled = 0;

    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (!d) continue;

        /* 只收 UHCI：class 0x0C / subclass 0x03 / progif 0x00（绝不碰其它类型） */
        if (d->class_code != 0x0Cu || d->subclass != 0x03u || d->prog_if != 0x00u)
            continue;

        found++;
        if (handled) {
            /* 本步只初始化第一个 UHCI，其余记一笔跳过，避免重复帧列表/端口日志 */
            dmesg_write("UHCI: additional UHCI controller skipped this step");
            continue;
        }
        handled = 1;
        uhci_setup_one(d);
    }

    if (found == 0) {
        dmesg_write("UHCI: no UHCI controller found");
        return;
    }

    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: ");
        u_app_dec(line, &li, lim, (uint32_t)found);
        u_app_str(line, &li, lim, " UHCI controller(s) found, initialized first");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* H2-2b：对已连接端口做最小控制传输探针（GET_DESCRIPTOR/SET_ADDRESS） */
    uhci_control_probe();
}
