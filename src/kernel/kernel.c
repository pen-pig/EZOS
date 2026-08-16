#include "tty.h"
#include "keyboard.h"
#include "idt.h"
#include "isr.h"
#include "types.h"
#include "ata.h"
#include "shell.h"
#include "exfat.h"
#include "mouse.h"

// 简单长度函数，供自动格式化示例使用
static size_t my_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

void kernel_main(void) {
    terminal_initialize();
	terminal_writestring("\n");
    terminal_writestring("  _____   ______  _____   _____\n");
    terminal_writestring(" |  ___| |___  / |  _  | /  ___|\n");
    terminal_writestring(" | |__      / /  | | | | \\ `--. \n");
    terminal_writestring(" |  __|    / /   | | | |  `--. \\\n");
    terminal_writestring(" | |___  ./ /___ | |_| | /\\__/ /\n");
    terminal_writestring(" \\____/  \\_____/ \\_____/ \\____/ \n");
    terminal_writestring("\n");
    terminal_writestring("by luogu __penpig & XPH_csc\n");
    idt_init();
    isr_install();
    irq_install();
    asm volatile("sti");

    terminal_writestring("Hello, EZOS!\n");

    ata_init();

    // 自动检测 exFAT，若未格式化则自动格式化从盘
    int ret = exfat_init();
    if (ret == -2) {
        terminal_writestring("exFAT not found, auto-formatting slave disk...\n");
        if (exfat_format() == 0) {
            terminal_writestring("exFAT format complete.\n");
            const char *example = "Hello from EZOS exFAT!\n";
            exfat_create_file("README.TXT", (const uint8_t*)example, my_strlen(example));
        } else {
            terminal_writestring("exFAT auto-format failed.\n");
        }
    } else if (ret == 0) {
        terminal_writestring("exFAT filesystem ready.\n");
    }

    terminal_writestring("Keyboard is active.\n");

    mouse_init();
    if (mouse_present()) {
        terminal_writestring("Mouse is active.\n");
    } else {
        terminal_writestring("Mouse not detected, using keyboard only.\n");
    }

    shell_run();

    while(1);
}
