#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KEY_UP    -1
#define KEY_DOWN  -2
#define KEY_LEFT  -3
#define KEY_RIGHT -4
#define KEY_PGUP  -5
#define KEY_PGDN  -6

void keyboard_init(void);
void keyboard_handler(void);
int keyboard_getchar(void);   // ·µ»Ø int£¬Ö§³ÖĞéÄâ¼üÂë
void irq1_handler(void);

#endif
