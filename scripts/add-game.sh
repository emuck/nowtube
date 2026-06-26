#!/usr/bin/env bash
# add-game.sh — copy Z3 games from the zvibe catalog into main/spiffs/
#
# Usage:
#   ./scripts/add-game.sh                 # copy restaurant.z3 (default)
#   ./scripts/add-game.sh --flash         # copy restaurant.z3 then flash SPIFFS
#   ./scripts/add-game.sh --list          # list available catalog games
#   ./scripts/add-game.sh --interactive   # choose catalog games manually
#   ./scripts/add-game.sh --game NAME.z3  # copy one named catalog game
#
# Games in main/spiffs/ are baked into the SPIFFS partition image and
# survive every "idf.py flash" without needing to re-upload.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CATALOG="$REPO_ROOT/components/zvibe/games/catalog"
DEST="$REPO_ROOT/main/spiffs"
DEFAULT_GAME="restaurant.z3"
SPIFFS_GAME_LIMIT=50

FLASH=0
MODE="default"
GAME="$DEFAULT_GAME"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --flash) FLASH=1; shift ;;
        --list) MODE="list"; shift ;;
        --interactive) MODE="interactive"; shift ;;
        --game)
            MODE="game"
            GAME="${2:-}"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if [[ ! -d "$CATALOG" ]]; then
    echo "Error: catalog not found at $CATALOG"
    echo "Is the zvibe submodule initialised? Run: git submodule update --init components/zvibe"
    exit 1
fi

mapfile -t ALL_GAMES < <(find "$CATALOG" -maxdepth 1 -name "*.z3" | sort)
if [[ ${#ALL_GAMES[@]} -eq 0 ]]; then
    echo "No .z3 files found in $CATALOG"
    exit 1
fi

if [[ "$MODE" == "list" ]]; then
    echo "Available games in catalog:"
    for g in "${ALL_GAMES[@]}"; do
        name="$(basename "$g")"
        already=""
        [[ -f "$DEST/$name" ]] && already=" (already in spiffs)"
        printf "  %s%s\n" "$name" "$already"
    done
    exit 0
fi

current_count() {
    find "$DEST" -maxdepth 1 -name "*.z3" | wc -l | tr -d ' '
}

copy_games() {
    local selected=("$@")
    local count
    count="$(current_count)"
    local new_count=$(( count + ${#selected[@]} ))
    if [[ $new_count -gt $SPIFFS_GAME_LIMIT ]]; then
        echo "Error: adding ${#selected[@]} game(s) would bring total to $new_count (limit: $SPIFFS_GAME_LIMIT)."
        exit 1
    fi

    echo "Copying to $DEST:"
    for g in "${selected[@]}"; do
        local name
        name="$(basename "$g")"
        cp "$g" "$DEST/$name"
        printf "  copied %s\n" "$name"
    done
}

if [[ "$MODE" == "interactive" ]]; then
    count="$(current_count)"
    echo ""
    echo "Available games (catalog: $CATALOG)"
    echo "Installed: $count / $SPIFFS_GAME_LIMIT"
    echo ""
    for i in "${!ALL_GAMES[@]}"; do
        name="$(basename "${ALL_GAMES[$i]}")"
        already=""
        [[ -f "$DEST/$name" ]] && already=" *"
        printf "  [%d] %s%s\n" "$((i+1))" "$name" "$already"
    done
    echo ""
    echo "  (* = already in spiffs)"
    echo ""
    echo "Enter numbers separated by spaces, or 'q' to quit:"
    read -r -p "> " selection

    if [[ "$selection" == "q" || -z "$selection" ]]; then
        echo "Cancelled."
        exit 0
    fi

    selected=()
    for token in $selection; do
        if ! [[ "$token" =~ ^[0-9]+$ ]]; then
            echo "Invalid selection: $token"
            exit 1
        fi
        idx=$(( token - 1 ))
        if [[ $idx -lt 0 || $idx -ge ${#ALL_GAMES[@]} ]]; then
            echo "Out of range: $token"
            exit 1
        fi
        selected+=("${ALL_GAMES[$idx]}")
    done
    copy_games "${selected[@]}"
else
    if [[ -z "$GAME" || "$GAME" == */* || "$GAME" == *\\* || "$GAME" != *.z3 ]]; then
        echo "Error: --game must be a catalog .z3 filename with no path separators"
        exit 1
    fi
    src="$CATALOG/$GAME"
    if [[ ! -f "$src" ]]; then
        echo "Error: catalog game not found: $GAME"
        echo "Run './scripts/add-game.sh --list' to see available games."
        exit 1
    fi
    copy_games "$src"
fi

echo ""
if [[ $FLASH -eq 1 ]]; then
    echo "Flashing SPIFFS partition..."
    cd "$REPO_ROOT"
    idf.py spiffs-flash
else
    echo "To bake into firmware: run 'idf.py flash' from the repo root."
    echo "To flash only the SPIFFS partition: run './scripts/add-game.sh --flash'"
fi
