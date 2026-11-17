#pragma once
#include <windows.h>

// Bateria do mouse Logitech sem fio, lida numa thread propria a cada 15 s.
typedef struct {
    BOOL present;      // ha um mouse pareado no receptor
    BOOL stale;        // mouse dormindo: ultimo valor conhecido
    BOOL charging;
    int percent;
    WCHAR name[64];
} BatteryInfo;

void Battery_Start(HWND notify, UINT msg);   // posta msg para notify quando o valor muda
void Battery_Refresh(void);                 // forca uma leitura agora
void Battery_Stop(void);
BOOL Battery_Get(BatteryInfo *out);
