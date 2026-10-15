// main/ui.h —— 设备屏幕:Wi-Fi / USB 两张状态卡与实时速率。
#pragma once

// 创建并加载状态页,启动 500 ms 刷新定时器。调用方须持有 bsp_lvgl_lock()。
void ui_create(void);
