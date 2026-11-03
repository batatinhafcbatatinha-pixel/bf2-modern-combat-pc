@echo off
rem Starts the port built by Setup.cmd with your game files. Extra options go to the game.
cd /d "%~dp0"
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
%PY% "%~dp0tools\play.py" --game-files "%~dp0game_files" %*
if errorlevel 1 (
  echo.
  echo Could not start the game. Run Setup.cmd first. / Nao foi possivel abrir o jogo. Rode o Setup.cmd primeiro.
  pause
)
