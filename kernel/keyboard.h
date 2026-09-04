#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KEY_UP    -1
#define KEY_DOWN  -2
#define KEY_LEFT  -3
#define KEY_RIGHT -4
#define KEY_PGUP  -5
#define KEY_PGDN  -6
#define KEY_F1    -7
#define KEY_F2    -8
#define KEY_F3    -9
#define KEY_F4    -10
#define KEY_F5    -11
#define KEY_F6    -12
#define KEY_F7    -13
#define KEY_F8    -14
#define KEY_F9    -15
#define KEY_F10   -16
#define KEY_F11   -17
#define KEY_F12   -18
#define KEY_ALT_TAB -19   /* Alt+Tab 组合键：窗口切换（gw_handle_key 消费） */

void keyboard_init(void);
void keyboard_handler(void);
int keyboard_getchar(void);   // ���� int��֧���������
void irq1_handler(void);

#endif
