# PainelLeve

Painel de controle leve na bandeja do Windows 11, em C puro + Win32 (sem dependências).
Substitui o TranslucentTB e o EarTrumpet num único app de ~80 KB e ~12 MB de RAM.

## Recursos

- **Barra de tarefas transparente** — só transparência. Usa a política de acento do DWM e uma DLL
  (`PainelLeveTap`) carregada no Explorer via `InitializeXamlDiagnosticsEx`, que oculta o fundo XAML
  que o Windows 11 pinta por cima (mesma técnica do TranslucentTB).
- **Medidor de velocidade da internet** — barrinha arrastável com download/upload da interface que
  leva à internet.
- **Mixer de volume** (clique esquerdo no ícone) — por dispositivo e por aplicativo, com medidor de
  nível em tempo real, mudo, som de amostra ao soltar o slider, ícone por tipo de dispositivo e
  seta para recolher/expandir os outros dispositivos. Estilo EarTrumpet.
- Iniciar com o Windows, instância única, reaplica tudo se o Explorer reiniciar.

## Compilar

Precisa só do [Zig](https://ziglang.org) (`winget install zig.zig`):

```
build.cmd
```

Gera `PainelLeve.exe` e `PainelLeveTap.dll` na mesma pasta. Para testar uma DLL nova sem reiniciar o
Explorer (que trava a DLL carregada), compile com outro nome e aponte no `config.ini`:

```
build.cmd PainelLeveTap.dev2.dll
```

```ini
[geral]
tap_dll=PainelLeveTap.dev2.dll
```

## Arquivos

| Arquivo | O que é |
|---|---|
| `main.c` | bandeja, menu, transparência, medidor de velocidade, injeção da DLL |
| `mixer.c` / `mixer.h` | mixer de volume (Core Audio) |
| `tap.cpp` / `tap.def` | DLL carregada no Explorer (XAML diagnostics) |
| `build.cmd` | compilação com `zig cc` / `zig c++` |

## Notas técnicas

- O endpoint do `InitializeXamlDiagnosticsEx` tem de ser `VisualDiagConnection1`.
- `IVisualTreeService2::GetPropertyIndex` devolve `E_INVALIDARG`; o índice vem de
  `GetPropertyValuesChain`.
- `IAudioMeterInformation` não vem nos cabeçalhos do mingw; está declarada em `mixer.c`.
- O envio de volume (`SetMasterVolume`) custa até ~20 ms por sessão; o mixer faz isso numa thread
  auxiliar para a interface não travar.
