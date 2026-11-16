// Mixer de volume por aplicativo (estilo EarTrumpet), parte do PainelLeve.
#pragma once
#include <windows.h>

void Mixer_Init(HINSTANCE hinst, HFONT font, int dpi);   // chamar uma vez, com COM ja inicializado (STA)
void Mixer_Toggle(void);                                   // abre (perto da bandeja) ou fecha o painel
void Mixer_Close(void);
BOOL Mixer_IsOpen(void);
