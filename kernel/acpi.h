/*
 * acpi.h - ACPI 表解析 + 真机电源管理（关机）
 *
 * 目标：让 shutdown 在真机上工作，而不只是 QEMU 的固定端口 0x604。
 * 路径：RSDP -> RSDT/XSDT -> FADT 取 PM1a_CNT_BLK；DSDT 里找 _S5 包
 * 取 SLP_TYP；写 PM1a_CNT = SLP_TYPa<<10 | SLP_EN。
 * 任何一步解析失败 -> fail closed，退回 QEMU 0x604 写法（无害）。
 */
#ifndef ACPI_H
#define ACPI_H

/* 开机时调用一次（读低内存即可，不依赖 PCI/分页时序）。
 * 结果只存内部状态；找到 FADT 与 _S5 才算 ready。 */
void acpi_init(void);

/* 关机：ACPI ready 时写真实 PM1a_CNT（必要时含 PM1b 与 SMI 使能），
 * 否则回退 QEMU i440fx 的 0x604。不返回（cli+hlt 兜底）。 */
void acpi_shutdown(void);

/* 诊断：返回描述解析结果的行（供 klog），如
 * "ACPI: RSDPv2 FADT rev4 PM1a_CNT=0x604 S5_TYP=0x1" 或
 * "ACPI: not found - using legacy QEMU port" */
const char *acpi_status_line(void);

#endif
