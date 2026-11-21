#pragma once
void fbcon_init(void);       // HDMI text console; no-op if the firmware has no framebuffer
void fbcon_putc(char c);     // called for every byte sent to the UART
