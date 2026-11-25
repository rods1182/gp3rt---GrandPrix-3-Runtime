#!/bin/sh
# Grand Prix 3 para Linux (GP3RT): verifica o sistema e cria o atalho no menu.
# Uso: ./instalar-atalho.sh [pasta-do-jogo]
DIR=$(cd "$(dirname "$0")" && pwd)
ok=1
echo "== Grand Prix 3 para Linux =="
if ! "$DIR/gp3" --version >/dev/null 2>&1; then
    echo "[!] Falta a biblioteca SDL2. Instale com o gerenciador de pacotes:"
    echo "      Mint/Ubuntu/Debian:  sudo apt install libsdl2-2.0-0"
    echo "      Fedora:              sudo dnf install SDL2"
    echo "      Arch/Manjaro:        sudo pacman -S sdl2"
    echo "      openSUSE:            sudo zypper install libSDL2-2_0-0"
    ok=0
fi
if ! "$DIR/gp3-game" --selftest >/dev/null 2>&1; then
    echo "[!] Este sistema nao consegue executar programas de 32 bits (kernel sem IA32)."
    ok=0
fi
GAME=$("$DIR/gp3" --find-game ${1:+"$1"} 2>/dev/null)
if [ -z "$GAME" ]; then
    echo "[!] Executavel do jogo (Gp3.exe ou gp3 2000.exe) nao encontrado. Copie estes arquivos para a pasta do jogo"
    echo "    ou informe a pasta: ./instalar-atalho.sh /caminho/do/GP3"
    exit 1
fi
[ $ok = 1 ] || { echo "Corrija os itens acima e rode de novo."; exit 1; }
mkdir -p "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/32x32/apps"
"$DIR/gp3" --install-icons 2>/dev/null
# our icon on gp3 / GPxPatch in the file manager
[ -e "$DIR/gp3" ] && gio set -n "$DIR/gp3" metadata::custom-icon "file://$HOME/.local/share/icons/hicolor/256x256/apps/gp3rt.png" 2>/dev/null
[ -e "$DIR/GPxPatch" ] && gio set -n "$DIR/GPxPatch" metadata::custom-icon "file://$HOME/.local/share/icons/hicolor/256x256/apps/gpxpatch-linux.png" 2>/dev/null
cat > "$HOME/.local/share/applications/grand-prix-3.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Grand Prix 3
Comment=Grand Prix 3 nativo no Linux (GP3RT)
Exec="$DIR/gp3" "$GAME"
Path=$GAME
Icon=$HOME/.local/share/icons/hicolor/256x256/apps/gp3rt.png
Terminal=false
Categories=Game;Simulation;
StartupWMClass=gp3
DESK
# GPxPatch: configuration screen + start with/without GPxPatch (its own window)
if [ -x "$DIR/GPxPatch" ] && [ -n "$(ls "$GAME" | grep -i '^gpxpdata$')" ]; then
    ICON=$HOME/.local/share/icons/hicolor/256x256/apps/gpxpatch-linux.png
    cat > "$HOME/.local/share/applications/gp3-gpxpatch.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=GPxPatch (Grand Prix 3)
Comment=Configura o GPxPatch e inicia o Grand Prix 3 com ou sem ele
Exec="$DIR/GPxPatch" "$GAME"
Path=$GAME
Icon=$ICON
Terminal=false
Categories=Game;Simulation;
DESK
    echo "Pronto! \"GPxPatch (Grand Prix 3)\" tambem foi adicionado ao menu."
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" 2>/dev/null
echo "Pronto! \"Grand Prix 3\" foi adicionado ao menu de aplicativos (Jogos)."
echo "Pasta do jogo: $GAME"
