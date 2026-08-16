#ifndef MOUSE_H
#define MOUSE_H

// 鼠标按钮位定义
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

void mouse_init(void);
void mouse_handler(void);
void irq12_handler(void);
int mouse_get_wheel(void);   // >0 向上滚，<0 向下滚，0 无事件
int mouse_present(void);     // 1 有指点设备，0 无
int mouse_get_irq_count(void); // 调试：IRQ12 触发次数
int mouse_get_x(void);       // 当前 X 坐标（屏幕像素）
int mouse_get_y(void);       // 当前 Y 坐标（屏幕像素）
int mouse_get_buttons(void); // 当前按钮状态（MOUSE_BTN_* 位组合）
void mouse_set_bounds(int w, int h); // 设置坐标边界（按当前显示模式）

#endif
