#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_handler(void);
void irq12_handler(void);
int mouse_get_wheel(void);   // >0 向上滚，<0 向下滚，0 无事件
int mouse_present(void);     // 1 有指点设备，0 无
int mouse_get_x(void);       // 当前 X 坐标（0 到 GFX_W-1，图形模式，分辨率自适应）
int mouse_get_y(void);       // 当前 Y 坐标（0 到 GFX_H-1，图形模式，分辨率自适应）
int mouse_get_buttons(void); // bit0=左键, bit1=右键, bit2=中键（按下为 1）
unsigned long mouse_packet_count(void); // [DEBUG] 完整包计数
void mouse_raw_trace(unsigned char *out, unsigned long *cnt); // [DEBUG] 最近2原始字节+总数
void mouse_set_sensitivity(int mul256); // 鼠标灵敏度：256=1.0x，可调 32..2048
void mouse_warp(int x, int y);          // 直接设置指针位置（GUI 启动时按实际分辨率居中）

#endif
