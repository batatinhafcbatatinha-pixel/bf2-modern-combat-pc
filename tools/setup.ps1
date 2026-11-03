# One-click setup: installs what the build needs, asks for your game, builds the port privately.
# Started by Setup.cmd. Safe to run again: finished steps are skipped. Log: setup-log.txt.
# The steps of this game are in setup.json; the same script is shared by every port.
# -Yes answers every question with yes; -Rebuild redoes every step; -ToolsOnly stops after the tools;
# -NoShortcut skips the desktop shortcut; -Jobs sets the number of parallel compile jobs.
param([switch]$Yes, [switch]$Rebuild, [switch]$ToolsOnly, [switch]$NoShortcut, [int]$Jobs = 0)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $Root
$Setup = Get-Content -LiteralPath (Join-Path $Root 'setup.json') -Raw | ConvertFrom-Json
$Install = Get-Content -LiteralPath (Join-Path $Root 'installer.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Game = $Setup.game
$Pt = (Get-UICulture).Name -like 'pt*'
function Say([string]$en, [string]$pt, [string]$color = 'White') { Write-Host ($(if ($Pt) { $pt } else { $en })) -ForegroundColor $color }
function Step([string]$en, [string]$pt) { Write-Host ''; Say "== $en" "== $pt" 'Cyan' }
function Fail([string]$en, [string]$pt) {
  Write-Host ''; Say $en $pt 'Red'
  Say 'Details are in setup-log.txt. You can run Setup again; finished steps are skipped.' `
      'Os detalhes estao em setup-log.txt. Pode rodar o Setup de novo; as etapas prontas sao puladas.' 'Yellow'
  Stop-Transcript | Out-Null; exit 1
}
function Ask([string]$en, [string]$pt) {
  if ($Yes) { return $true }
  $answer = Read-Host ($(if ($Pt) { "$pt [S/n]" } else { "$en [Y/n]" }))
  return ($answer -eq '' -or $answer -match '^[sSyY]')
}
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
function Winget-Install([string]$id, [string[]]$extra) {
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, then run Setup again." `
         "O winget nao esta instalado. Instale o 'Instalador de Aplicativo' na Microsoft Store e rode o Setup de novo."
  }
  & winget install -e --id $id --accept-package-agreements --accept-source-agreements @extra
  # 0 = installed, 3010 = installed (restart needed), -1978335189 = already installed
  if ($LASTEXITCODE -notin @(0, 3010, -1978335189)) { Fail "Installing $id failed (code $LASTEXITCODE)." "A instalacao de $id falhou (codigo $LASTEXITCODE)." }
  Refresh-Path
}
function Game-Ok {
  # The exact supported revision: SHA-256 of every executable listed in installer.json.
  foreach ($file in $Install.required_files.PSObject.Properties) {
    $path = Join-Path $Root (Join-Path 'game_files' $file.Name)
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.Value) { return $false }
  }
  return $true
}

Start-Transcript -Path (Join-Path $Root 'setup-log.txt') -Force | Out-Null
Say "Setup for $Game (PC port). Your game is never uploaded anywhere." "Setup de $Game (port para PC). Seu jogo nunca e enviado para lugar nenhum." 'Green'
if ($Setup.available -eq $false) {
  Fail "This port cannot be built from this download yet: $($Setup.reason_en)" "Este port ainda nao pode ser montado a partir deste download: $($Setup.reason_pt)"
}
if (-not [Environment]::Is64BitOperatingSystem) { Fail 'A 64-bit Windows is required.' 'E preciso Windows de 64 bits.' }
$drive = Get-PSDrive -Name $Root.Substring(0, 1)
$freeGB = [math]::Floor($drive.Free / 1GB)
if ($freeGB -lt $Setup.disk_gb) {
  Say "Only $freeGB GB free on $($drive.Name):. About $($Setup.disk_gb) GB are needed here, plus about 7 GB on C: for the Build Tools." `
      "So ha $freeGB GB livres em $($drive.Name):. Sao necessarios cerca de $($Setup.disk_gb) GB aqui, mais cerca de 7 GB no C: para o Build Tools." 'Yellow'
  if (-not (Ask 'Continue anyway?' 'Continuar mesmo assim?')) { Stop-Transcript | Out-Null; exit 1 }
}

# 1. Python (with Tk for the game-file installer)
Step '1/5 Python' '1/5 Python'
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python', '-B'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $exe = & $c[0] $c[1] -c "import sys, tkinter; assert sys.version_info >= (3, 11); print(sys.executable)" 2>$null
    if ($LASTEXITCODE -eq 0 -and $exe) { return "$exe".Trim() }
  }
  $local = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
  if (Test-Path $local) { return $local }
  return $null
}
$Python = Find-Python
if (-not $Python) {
  Say 'Python 3.12 will be installed (for your user only).' 'O Python 3.12 sera instalado (so para o seu usuario).'
  if (-not (Ask 'Install Python now?' 'Instalar o Python agora?')) { Fail 'Python is required.' 'O Python e necessario.' }
  Winget-Install 'Python.Python.3.12' @('--scope', 'user')
  $Python = Find-Python
  if (-not $Python) { Fail 'Python was installed but not found. Restart the PC and run Setup again.' 'O Python foi instalado mas nao foi encontrado. Reinicie o PC e rode o Setup de novo.' }
}
Say "OK: $Python" "OK: $Python" 'Green'

# 2. Visual Studio Build Tools (C++ compiler libraries and the Windows SDK)
Step '2/5 Visual Studio Build Tools (C++)' '2/5 Visual Studio Build Tools (C++)'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Find-VC {
  if (-not (Test-Path $vswhere)) { return $null }
  $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $sdk = Test-Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include')
  if ($path -and $sdk) { return "$path".Trim() }
  return $null
}
if (-not (Find-VC)) {
  Say 'Microsoft C++ Build Tools will be installed (about 7 GB, needs administrator permission, 10-30 minutes).' `
      'O Microsoft C++ Build Tools sera instalado (cerca de 7 GB, pede permissao de administrador, 10 a 30 minutos).'
  if (-not (Ask 'Install it now?' 'Instalar agora?')) { Fail 'The C++ Build Tools are required.' 'O C++ Build Tools e necessario.' }
  $components = '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait --norestart'
  $other = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { $null }
  if ($other) {
    # Visual Studio exists without the C++ tools: add them to that install.
    $vsSetup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    $p = Start-Process -FilePath $vsSetup -ArgumentList "modify --installPath `"$other`" $components" -Verb RunAs -Wait -PassThru
    if ($p.ExitCode -notin @(0, 3010)) { Fail "Visual Studio Installer failed (code $($p.ExitCode))." "O instalador do Visual Studio falhou (codigo $($p.ExitCode))." }
  } else {
    Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' @('--override', $components)
  }
  if (-not (Find-VC)) { Fail 'The C++ Build Tools were not found after installing. Restart the PC and run Setup again.' 'O C++ Build Tools nao foi encontrado depois da instalacao. Reinicie o PC e rode o Setup de novo.' }
}
$VsPath = Find-VC
Say 'OK' 'OK' 'Green'

# 3. Portable tools inside this folder (.local\tools) and the parts a ZIP download leaves out
Step '3/5 Build tools and project parts' '3/5 Ferramentas de build e partes do projeto'
Say "Kept inside this folder; the first download is about $($Setup.download_gb) GB." "Ficam dentro desta pasta; o primeiro download tem cerca de $($Setup.download_gb) GB."
$toolLines = & $Python (Join-Path $PSScriptRoot 'setup_tools.py')
if ($LASTEXITCODE -ne 0) { Fail 'Downloading the build tools failed. Check your internet connection.' 'O download das ferramentas falhou. Confira sua conexao com a internet.' }
$Tool = @{}
foreach ($line in $toolLines) { if ($line -match '^(\w+)=(.*)$') { $Tool[$Matches[1]] = $Matches[2] } }
# The C++ environment of the Build Tools, then the portable tools in front of it.
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
$front = @($Tool['PATH'] -split ';' | Where-Object { $_ })
if ($Tool['JAVA_HOME']) { $env:JAVA_HOME = $Tool['JAVA_HOME']; $front += (Join-Path $Tool['JAVA_HOME'] 'bin') }
if ($Tool['GHIDRA']) { $env:GHIDRA = $Tool['GHIDRA'] }
$env:Path = (($front + @(Split-Path -Parent $Python)) -join ';') + ';' + $env:Path
Say 'OK' 'OK' 'Green'
if ($ToolsOnly) { Stop-Transcript | Out-Null; exit 0 }

# 4. Your game files
Step "4/5 Your game ($($Setup.disc))" "4/5 Seu jogo ($($Setup.disc))"
if (-not (Game-Ok)) {
  $installer = Join-Path $Root 'tools\port-installer\installer.py'
  Say 'A window opens: choose your disc image (.iso) or extracted game folder, then wait for it to finish.' `
      'Uma janela vai abrir: escolha a imagem do disco (.iso) ou a pasta extraida do jogo e espere terminar.'
  & $Python $installer --config (Join-Path $Root 'installer.json')
  if (-not (Game-Ok)) { Fail "game_files does not hold the supported version: $($Setup.disc)." "game_files nao contem a versao suportada: $($Setup.disc)." }
}
Say 'OK' 'OK' 'Green'

# 5. Build
Step '5/5 Building your PC version' '5/5 Montando sua versao para PC'
Say "This takes a while (about $($Setup.minutes) minutes on a recent PC). Keep this window open." `
    "Isso demora (cerca de $($Setup.minutes) minutos num PC recente). Deixe esta janela aberta."
$Py = $Python
if ($Setup.requirements) {
  # The build's own Python packages, in a private environment inside .local.
  $venv = Join-Path $Root '.local\venv'
  if (-not (Test-Path (Join-Path $venv 'Scripts\python.exe'))) { & $Python -m venv $venv; if ($LASTEXITCODE -ne 0) { Fail 'Creating the Python environment failed.' 'A criacao do ambiente Python falhou.' } }
  $Py = Join-Path $venv 'Scripts\python.exe'
  & $Py -m pip install --disable-pip-version-check -q -r (Join-Path $Root $Setup.requirements)
  if ($LASTEXITCODE -ne 0) { Fail 'Installing the Python packages failed.' 'A instalacao dos pacotes Python falhou.' }
  $env:Path = (Split-Path -Parent $Py) + ';' + $env:Path
}
if ($Jobs -le 0) {
  $freeRAM = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1KB
  $Jobs = [math]::Max(1, [math]::Min([Environment]::ProcessorCount - 2, [math]::Floor($freeRAM / ($Setup.gb_per_job * 1GB))))
  if ($Setup.max_jobs) { $Jobs = [math]::Min($Jobs, $Setup.max_jobs) }
}
function Expand([string]$text) {
  $text.Replace('{python}', $Py).Replace('{root}', $Root).Replace('{game_files}', (Join-Path $Root 'game_files')).
    Replace('{jobs}', "$Jobs").Replace('{llvm}', "$($Tool['LLVM'])")
}
foreach ($s in $Setup.steps) {
  if (-not $Rebuild -and $s.done_if -and (Test-Path (Join-Path $Root $s.done_if))) { Say "- $($s.name): already done" "- $($s.name): ja feito" 'DarkGray'; continue }
  if ($s.env) { foreach ($e in $s.env.PSObject.Properties) { Set-Item -Path "env:$($e.Name)" -Value (Expand $e.Value) } }
  while ($true) {
    Say "- $($s.name) ($Jobs jobs)" "- $($s.name) ($Jobs compilacoes ao mesmo tempo)"
    $cmd = @($s.run | ForEach-Object { Expand $_ })
    if ($s.reuse_if -and -not $Rebuild -and (Test-Path (Join-Path $Root $s.reuse_if))) { $cmd += $s.reuse_arg }
    $exe, $rest = $cmd
    & $exe @rest
    if ($LASTEXITCODE -eq 0) { break }
    # Fewer jobs only help once compiling has started (usually memory); other failures repeat the same way.
    if ($Jobs -le 1 -or -not $s.retry_if -or -not (Test-Path (Join-Path $Root $s.retry_if))) { Fail "The build failed at: $($s.name)." "O build falhou em: $($s.name)." }
    $Jobs = [math]::Max(1, [math]::Floor($Jobs / 2))
    Say 'The build stopped; trying again with fewer parallel jobs.' 'O build parou; tentando de novo com menos compilacoes ao mesmo tempo.' 'Yellow'
  }
}

$play = Join-Path $Root $Setup.play
if (-not $NoShortcut -and (Test-Path $play) -and (Ask 'Create a desktop shortcut to play?' 'Criar um atalho na area de trabalho para jogar?')) {
  $shell = New-Object -ComObject WScript.Shell
  $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) "$Game.lnk"))
  $link.TargetPath = $play
  $link.WorkingDirectory = $Root
  $link.Save()
}
Write-Host ''
Say "Done! Play with $($Setup.play) (or the desktop shortcut). F1 in the game opens the PC settings." `
    "Pronto! Jogue pelo $($Setup.play) (ou pelo atalho na area de trabalho). F1 no jogo abre as configuracoes de PC." 'Green'
Stop-Transcript | Out-Null
