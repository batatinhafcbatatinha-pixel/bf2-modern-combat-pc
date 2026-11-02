@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0Instalar-Arquivos.exe" (
  start "" "%~dp0Instalar-Arquivos.exe" --config "%~dp0installer.json"
  exit /b 0
)
if not exist "%~dp0tools\port-installer\installer.py" (
  git submodule update --init -- tools/port-installer
  if errorlevel 1 goto missing
)
python "%~dp0tools\port-installer\installer.py" --config "%~dp0installer.json"
if errorlevel 1 goto missing
exit /b 0
:missing
echo Nao foi possivel abrir o instalador.
echo Use o pacote Instalar-Arquivos-Windows em Actions, ou instale Python 3.11 com Tk.
pause
exit /b 1
