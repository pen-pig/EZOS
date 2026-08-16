#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_handler(void);
void irq12_handler(void);
int mouse_get_wheel(void);   // >0 向上滚，<0 向下滚，0 无事件
int mouse_present(void);     // 1 有指点设备，0 无

#endif
