# PainelLeve

Painel de controle leve na bandeja do Windows 11, em C puro + Win32, sem dependências.
Substitui o TranslucentTB e o EarTrumpet num único app de ~80 KB e ~12 MB de RAM.

## Instalar (um comando)

Abra o **PowerShell** (Win + X → Terminal) e cole:

```powershell
irm https://raw.githubusercontent.com/vacilo2/PainelLeve/main/instalar.ps1 | iex
```

Isso baixa a versão mais recente, instala em `%LOCALAPPDATA%\PainelLeve`, configura para iniciar
com o Windows e abre o app. Para **atualizar**, rode o mesmo comando de novo.

Requisito: Windows 11. (No Windows 10 o app abre, mas a barra não fica transparente.)

### Instalar à mão

1. Baixe o `PainelLeve-vX.Y.Z.zip` na página de [Releases](https://github.com/vacilo2/PainelLeve/releases/latest).
2. Extraia numa pasta qualquer (ex.: `C:\PainelLeve`).
3. Abra `PainelLeve.exe`. No menu (clique direito no ícone da bandeja) marque **Iniciar com o Windows**.

### Desinstalar

Clique direito no ícone → **Sair**, desmarque **Iniciar com o Windows** antes se estiver marcado, e apague a
pasta `%LOCALAPPDATA%\PainelLeve`. Não há nada no registro além da entrada de inicialização.

## Como usar

| Ação | Resultado |
|---|---|
| Clique **esquerdo** no ícone | abre/fecha o mixer de volume |
| Clique **direito** no ícone | menu: barra transparente, medidor de velocidade, iniciar com o Windows, sair |
| No mixer: arrastar / roda do mouse | muda o volume (toca o som de amostra ao soltar) |
| No mixer: clicar no ícone do app | silencia / desfaz |
| No mixer: seta no cabeçalho | mostra ou esconde os outros dispositivos de saída |
| Barrinha de velocidade | arraste para onde quiser; clique direito nela abre o menu |

## Recursos

- **Barra de tarefas transparente** — só transparência. Política de acento do DWM + a DLL `PainelLeveTap`,
  carregada no Explorer via `InitializeXamlDiagnosticsEx`, que oculta o fundo XAML que o Windows 11 pinta
  por cima (mesma técnica do TranslucentTB).
- **Medidor de velocidade da internet** — download/upload da interface que leva à internet.
- **Mixer de volume** estilo EarTrumpet — por dispositivo e por aplicativo, medidor de nível em tempo real,
  ícone por tipo de dispositivo, nomes amigáveis, animação de abertura.
- Instância única; reaplica tudo se o Explorer reiniciar.

## Compilar a partir do código

Só precisa do [Zig](https://ziglang.org):

```powershell
winget install zig.zig
git clone https://github.com/vacilo2/PainelLeve.git
cd PainelLeve
.\build.cmd
```

Gera `PainelLeve.exe` e `PainelLeveTap.dll`. Para testar uma DLL nova sem reiniciar o Explorer (que trava
a DLL carregada), compile com outro nome e aponte no `config.ini`:

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
| `instalar.ps1` | instalador/atualizador de um comando |

## Notas técnicas

- O endpoint do `InitializeXamlDiagnosticsEx` tem de ser `VisualDiagConnection1`.
- `IVisualTreeService2::GetPropertyIndex` devolve `E_INVALIDARG`; o índice vem de `GetPropertyValuesChain`.
- `IAudioMeterInformation` não vem nos cabeçalhos do mingw; está declarada em `mixer.c`.
- `SetMasterVolume` custa até ~20 ms por sessão; o mixer envia numa thread auxiliar para a interface não travar.
