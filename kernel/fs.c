/*
 * fs.c - 文件系统统一分发层（exFAT / FAT12 / FAT16 / FAT32）
 *
 * 挂载策略：
 *   1) 首选数据盘（默认从盘 1）优先：先试 exFAT，再扫 MBR 各分区挂 FAT，
 *      最后尝试整盘 superfloppy FAT；
 *   2) 首选盘无文件系统时依次尝试其余驱动器（0-3，含副 ATA 总线）；
 *   3) 全部失败：首选盘有介质返回 -2（可触发格式化），无介质返回 -1。
 *
 * fs_create_file 统一为 create-or-replace 语义：先删旧文件再建新文件，
 * 修复记事本/vi/write 对已存在文件二次保存失败的存量问题。
 */
#include "fs.h"
#include "exfat.h"
#include "fat.h"
#include "ata.h"

/* 布局一致性编译期检查：三种目录项结构必须逐字节一致 */
typedef char fs_layout_exfat[sizeof(fs_dir_entry_t) == sizeof(exfat_dir_entry_t) ? 1 : -1];
typedef char fs_layout_fat[sizeof(fs_dir_entry_t) == sizeof(fat_dir_entry_t) ? 1 : -1];

static int      fs_type_cur = FS_NONE;
static uint8_t  fs_drive_cur = 0;
static uint8_t  fs_preferred_drive = 1;    /* 数据盘默认从盘 */
static fs_info_t fs_volume;

/* 分区表项的合法起始扇区上限（128GB/512B 扇区），过滤损坏表项 */
#define FS_PART_LBA_LIMIT 0x08000000u

static void fs_fill_info(void) {
    fs_volume.type = (uint8_t)fs_type_cur;
    fs_volume.drive = fs_drive_cur;
    if (fs_type_cur == FS_EXFAT) {
        const exfat_info_t *e = exfat_get_info();
        fs_volume.part_start = (uint32_t)e->partition_offset;
        fs_volume.bytes_per_sector = e->bytes_per_sector;
        fs_volume.sectors_per_cluster = e->sectors_per_cluster;
        fs_volume.cluster_count = e->cluster_count;
        fs_volume.volume_sectors = (uint32_t)e->volume_length;
    } else {
        const fat_info_t *f = fat_get_info();
        fs_volume.part_start = f->part_start;
        fs_volume.bytes_per_sector = f->bytes_per_sector;
        fs_volume.sectors_per_cluster = f->sectors_per_cluster;
        fs_volume.cluster_count = f->cluster_count;
        fs_volume.volume_sectors = f->total_sectors;
    }
}

/* 在 drive 上尝试挂载 FAT：扫 MBR 主分区 + superfloppy；成功返回 12/16/32 */
static int fat_try_mount(uint8_t drive) {
    uint8_t sec[512];
    if (ata_read_sector(drive, 0, sec) != 0) return 0;

    if (sec[510] == 0x55 && sec[511] == 0xAA) {
        int has_part = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t *entry = sec + 446 + i * 16;
            if (entry[4] == 0) continue;
            has_part = 1;
            uint32_t start = entry[8] | ((uint32_t)entry[9] << 8) |
                             ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
            uint32_t len = entry[12] | ((uint32_t)entry[13] << 8) |
                           ((uint32_t)entry[14] << 16) | ((uint32_t)entry[15] << 24);
            if (start == 0 || start >= FS_PART_LBA_LIMIT || len == 0) continue;
            int t = fat_mount(drive, start);
            if (t == 12 || t == 16 || t == 32) return t;
        }
        if (has_part) return 0;   /* 有分区表但都不是 FAT：不再按 superfloppy 解析 */
    }

    /* 无 MBR 签名（或空分区表）：整盘 FAT 卷 */
    int t = fat_mount(drive, 0);
    if (t == 12 || t == 16 || t == 32) return t;
    return 0;
}

/* 在 drive 上尝试任意文件系统；成功置 fs_type_cur 并返回 1 */
static int fs_try_drive(uint8_t drive) {
    /* 快速判存：空插槽对 IDENTIFY 无应答，避免后续读盘等满超时 */
    if (!ata_drive_present(drive)) return 0;

    /* exFAT 后端：exfat_init 自带 MBR 扫描与 superfloppy 回退 */
    exfat_set_drive(drive);
    if (exfat_init() == 0) {
        fs_type_cur = FS_EXFAT;
        fs_drive_cur = drive;
        fs_fill_info();
        return 1;
    }

    int t = fat_try_mount(drive);
    if (t == 12 || t == 16 || t == 32) {
        fs_type_cur = t;
        fs_drive_cur = drive;
        fs_fill_info();
        return 1;
    }
    return 0;
}

static int fs_is_fat(void) {
    return fs_type_cur == FS_FAT12 || fs_type_cur == FS_FAT16 || fs_type_cur == FS_FAT32;
}

int fs_init(void) {
    if (fs_type_cur != FS_NONE) return 0;    /* 已挂载：幂等返回，保留 cwd */

    /* 1) 首选数据盘 */
    if (fs_try_drive(fs_preferred_drive)) return 0;

    /* 2) 其余数据驱动器 1-3（含副 ATA 总线）。
     *    0 号盘是引导盘，不参与自动回退挂载，防止误识别后写入破坏启动镜像
     *    （用户仍可显式 setdrive 0 挂载）。 */
    for (uint8_t d = 1; d < 4; d++) {
        if (d == fs_preferred_drive) continue;
        if (fs_try_drive(d)) return 0;
    }

    /* 3) 全部失败：区分"有盘无文件系统"与"无介质" */
    if (!ata_drive_present(fs_preferred_drive)) return -1;
    uint8_t sec[512];
    if (ata_read_sector(fs_preferred_drive, 0, sec) == 0) return -2;
    return -1;
}

int fs_ready(void) { return fs_type_cur != FS_NONE; }

const char *fs_type_name(void) {
    switch (fs_type_cur) {
    case FS_EXFAT: return "exFAT";
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    default:       return "none";
    }
}

const fs_info_t *fs_get_info(void) { return &fs_volume; }

void fs_set_drive(uint8_t drive) {
    if (drive > 3) return;
    if (drive == fs_preferred_drive && fs_type_cur == FS_NONE) return;
    fs_preferred_drive = drive;
    if (fs_type_cur != FS_NONE) {
        fs_type_cur = FS_NONE;      /* 强制下次 fs_init 重新扫描 */
        exfat_reset_cwd();          /* 旧盘 cwd 不能带到新盘 */
    }
}

int fs_format(void) {
    /* 0 号盘是引导盘：拒绝格式化，防止把启动镜像抹掉 */
    if (fs_preferred_drive == 0) return -1;
    exfat_set_drive(fs_preferred_drive);
    exfat_reset_cwd();
    if (exfat_format() != 0) return -1;
    fs_type_cur = FS_EXFAT;
    fs_drive_cur = fs_preferred_drive;
    fs_fill_info();
    return 0;
}

/* ============ 分发 API ============ */

int fs_read_file(const char *name, uint8_t *buffer, uint32_t max_size) {
    if (fs_type_cur == FS_EXFAT) return exfat_read_file(name, buffer, max_size);
    if (fs_is_fat()) return fat_read_file(name, buffer, max_size);
    return -1;
}

uint32_t fs_get_file_size(const char *name) {
    if (fs_type_cur == FS_EXFAT) return exfat_get_file_size(name);
    if (fs_is_fat()) return fat_get_file_size(name);
    return 0;
}

int fs_create_file(const char *name, const uint8_t *data, uint32_t size) {
    if (fs_type_cur == FS_NONE) return -1;
    /* create-or-replace：先删旧文件（不存在则忽略），修复覆盖写 */
    if (fs_type_cur == FS_EXFAT) {
        exfat_delete_file(name);
        return exfat_create_file(name, data, size);
    }
    if (fs_is_fat()) {
        fat_delete_file(name);
        return fat_create_file(name, data, size);
    }
    return -1;
}

int fs_delete_file(const char *name) {
    if (fs_type_cur == FS_EXFAT) return exfat_delete_file(name);
    if (fs_is_fat()) return fat_delete_file(name);
    return -1;
}

int fs_list_root(void) {
    if (fs_type_cur == FS_EXFAT) return exfat_list_root();
    if (fs_is_fat()) return fat_list_root();
    return -1;
}

int fs_read_dir(fs_dir_entry_t *entries, int max_entries) {
    if (fs_type_cur == FS_EXFAT)
        return exfat_read_dir((exfat_dir_entry_t *)entries, max_entries);
    if (fs_is_fat())
        return fat_read_dir((fat_dir_entry_t *)entries, max_entries);
    return -1;
}

int fs_change_dir(const char *name) {
    if (fs_type_cur == FS_EXFAT) return exfat_change_dir(name);
    if (fs_is_fat()) return fat_change_dir(name);
    return -1;
}

int fs_mkdir(const char *name) {
    if (fs_type_cur == FS_EXFAT) return exfat_mkdir(name);
    if (fs_is_fat()) return fat_mkdir(name);
    return -1;
}

const char *fs_cwd_path(void) {
    if (fs_type_cur == FS_EXFAT) return exfat_cwd_path();
    if (fs_is_fat()) return fat_cwd_path();
    return "/";
}

uint32_t fs_count_used_clusters(void) {
    if (fs_type_cur == FS_EXFAT) return exfat_count_used_clusters();
    if (fs_is_fat()) return fat_count_used_clusters();
    return 0;
}

uint32_t fs_get_file_clusters(const char *name) {
    if (fs_type_cur == FS_EXFAT) return exfat_get_file_clusters(name);
    if (fs_is_fat()) return fat_get_file_clusters(name);
    return 0;
}
