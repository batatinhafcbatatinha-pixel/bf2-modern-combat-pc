# Instalar os arquivos do jogo

Abra **Instalar-Arquivos.cmd** e escolha sua ISO ou a pasta completa já extraída.
A interface mostra o arquivo em andamento, a cópia/extração/verificação, o progresso e uma estimativa de término.
Os arquivos ficam em **game_files/**. A ISO não é mantida como dependência do jogo.
Depois da verificação, você pode escolher apagar a ISO original; o padrão é preservar.
Pastas de origem nunca são apagadas. Arquivos existentes diferentes não são sobrescritos.

Para usar sem Python, baixe o pacote **Instalar-Arquivos-Windows** na aba **Actions**, extraia na raiz deste port e abra **Instalar-Arquivos.exe**.
Ele deve ficar ao lado do arquivo **installer.json**.

Em checkouts de desenvolvimento, o código compartilhado fica fixado no submódulo **tools/port-installer**.
Para baixar: `git submodule update --init tools/port-installer`.
No Linux: `python3 tools/port-installer/installer.py --config installer.json` (Python 3.11+ com Tk).

O instalador prepara os dados da revisão compatível. A compilação e o estado de compatibilidade do port continuam seguindo seu README.
Ports PS2 precisam recompilar o runtime com a adaptação de leitura de arquivos; executáveis anteriores não recebem essa capacidade só pela instalação.
