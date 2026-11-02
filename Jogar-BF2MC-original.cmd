@echo off
rem Versao ORIGINAL (sem as otimizacoes de 27/10), so para comparar defeitos visuais e FPS.
rem Battlefield 2: Modern Combat (port ReXGlue, renderer/audio nativos).
rem
rem Controle (layout moderno, como os Battlefield atuais; so durante a jogatina - menus usam o layout original):
rem   LT mirar (segurar)   RT atirar        A pular (agachado/deitado: levanta)
rem   B toque agachar/levantar, segurar deitar      X toque recarregar, segurar usar/entrar no veiculo
rem   Y usar/entrar        LB hotswap       RB (segurar) + analogico direito = trocar arma
rem   L3 correr            R3 trocar assento/chamar tropas       D-pad radio    Start pausa
rem   Para o layout original do jogo: Jogar-BF2MC.cmd --bf2_modern_controls=false
rem
rem Teclado e mouse:
rem   Mouse move a camera (--bf2_mouse_sensitivity=1.0, --bf2_mouse_invert=true)
rem   Botao esq. atirar    Botao dir. mirar (segurar)    WASD andar    Shift correr
rem   Espaco pular   C agachar/levantar   Ctrl (segurar) agachado   Z deitar/levantar
rem   R recarregar   E usar/entrar   Q hotswap   F (segurar) + mouse trocar arma   V trocar assento
rem   Setas = D-pad (menus/radio)  Enter/Esc = Start   Tab = Back   Backspace = voltar (menus)   F1 opcoes
setlocal
cd /d "%~dp0project\out\build\win-amd64-release"
start "" bf2mc-base.exe "--game_data_root=%~dp0game_files" ^
  --mnk_mode=true --mnk_mouse=false ^
  --keybind_lstick_up=W,Shift+W,Ctrl+W,Shift+Ctrl+W --keybind_lstick_down=S,Shift+S,Ctrl+S,Shift+Ctrl+S ^
  --keybind_lstick_left=A,Shift+A,Ctrl+A,Shift+Ctrl+A --keybind_lstick_right=D,Shift+D,Ctrl+D,Shift+Ctrl+D ^
  --keybind_right_trigger=LMB,Shift+LMB,Ctrl+LMB,Shift+Ctrl+LMB --keybind_left_trigger=RMB,Shift+RMB,Ctrl+RMB,Shift+Ctrl+RMB ^
  --keybind_a=Space,Shift+Space,Ctrl+Space,Shift+Ctrl+Space --keybind_b=C,Ctrl+C,Shift+C,Backspace --keybind_x=R,Shift+R,Ctrl+R,Shift+Ctrl+R --keybind_y=E,Shift+E,Ctrl+E,Shift+Ctrl+E ^
  --keybind_left_shoulder=Q,Shift+Q,Ctrl+Q,Shift+Ctrl+Q --keybind_right_shoulder=F,Shift+F,Ctrl+F,Shift+Ctrl+F ^
  --keybind_lstick_press=F24 --keybind_rstick_press=V,Shift+V,Ctrl+V,Shift+Ctrl+V ^
  --keybind_rstick_up=F24 --keybind_rstick_down=F24 --keybind_rstick_left=F24 --keybind_rstick_right=F24 ^
  --keybind_dpad_up=Up --keybind_dpad_down=Down --keybind_dpad_left=Left --keybind_dpad_right=Right ^
  --keybind_start=Return,Escape --keybind_back=Tab %*
