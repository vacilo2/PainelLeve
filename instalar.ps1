# Instala ou atualiza o PainelLeve: baixa a versao mais recente do GitHub, extrai em
# %LOCALAPPDATA%\PainelLeve, configura para iniciar com o Windows e abre o app.
# Uso (PowerShell):  irm https://raw.githubusercontent.com/vacilo2/PainelLeve/main/instalar.ps1 | iex
$ErrorActionPreference = 'Stop'
$repo = 'vacilo2/PainelLeve'
$dir  = Join-Path $env:LOCALAPPDATA 'PainelLeve'
$exe  = Join-Path $dir 'PainelLeve.exe'

Write-Host "PainelLeve: procurando a versao mais recente..." -ForegroundColor Cyan
$rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'PainelLeve-instalador' }
$asset = $rel.assets | Where-Object { $_.name -like 'PainelLeve*.zip' } | Select-Object -First 1
if (-not $asset) { throw "Nenhum pacote .zip encontrado na release $($rel.tag_name)." }
Write-Host "Versao $($rel.tag_name) - baixando $($asset.name) ($([math]::Round($asset.size/1KB)) KB)"

$zip = Join-Path $env:TEMP 'PainelLeve.zip'
Invoke-WebRequest $asset.browser_download_url -OutFile $zip -UseBasicParsing

# fecha a versao anterior, se estiver aberta (ela restaura a barra ao sair)
Get-Process PainelLeve -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null; $_.WaitForExit(3000) | Out-Null }
Get-Process PainelLeve -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force $dir | Out-Null
Expand-Archive $zip -DestinationPath $dir -Force
Remove-Item $zip -ErrorAction SilentlyContinue
# a DLL fica travada pelo Explorer enquanto a versao antiga estiver carregada; o app sabe lidar com isso
# (PainelLeveTap.dll nova so entra em uso depois de um reinicio do Explorer ou do PC)

Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name PainelLeve -Value "`"$exe`""
Start-Process $exe
Write-Host "Pronto! PainelLeve $($rel.tag_name) instalado em $dir e configurado para iniciar com o Windows." -ForegroundColor Green
Write-Host "O icone (dois sliders) fica na bandeja, ao lado do relogio. Clique esquerdo = mixer, clique direito = menu."
