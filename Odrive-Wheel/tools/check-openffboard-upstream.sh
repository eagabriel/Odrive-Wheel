#!/usr/bin/env bash
# =============================================================================
# Compara nossas cópias forkadas de OpenFFBoard contra upstream (submodule)
# =============================================================================
# Uso: ./tools/check-openffboard-upstream.sh
#
# Mostra:
#   1. Commit do submodule atualmente checkado
#   2. Lista arquivos forkados em Odrive-Wheel/{src,inc} que existem
#      tanto local quanto no upstream
#   3. Pra cada um, indica se diff vs upstream
#   4. (Opcional) imprime diff detalhado com flag --verbose
#
# Workflow recomendado pra acompanhar atualizações upstream:
#   git submodule update --remote OpenFFBoard-master/OpenFFBoard-master
#   ./tools/check-openffboard-upstream.sh
#   # → revê os diffs, integra mudanças relevantes nas cópias locais
#   git add OpenFFBoard-master/OpenFFBoard-master Odrive-Wheel/...
#   git commit -m "Bump OpenFFBoard upstream + integrate changes"
# =============================================================================

set -e
cd "$(dirname "$0")/../.."

UPSTREAM="OpenFFBoard-master/OpenFFBoard-master/Firmware/FFBoard"
LOCAL_SRC="Odrive-Wheel/src"
LOCAL_INC="Odrive-Wheel/inc"
VERBOSE=${1:-}

echo "═══════════════════════════════════════════════════════════════════"
echo "  OpenFFBoard upstream comparison"
echo "═══════════════════════════════════════════════════════════════════"

# 1. Status do submodule
echo
echo "▶ Submodule status:"
git submodule status OpenFFBoard-master/OpenFFBoard-master | sed 's/^/  /'

if [ ! -d "$UPSTREAM" ]; then
    echo "  ❌ Submodule não inicializado. Rode:"
    echo "     git submodule update --init --recursive"
    exit 1
fi

# 2. Lista arquivos forkados (mesmo nome em local e upstream)
echo
echo "▶ Arquivos forkados em $LOCAL_SRC + $LOCAL_INC:"
echo

declare -i differing=0
declare -i identical=0
declare -i missing=0

check_file() {
    local localfile="$1"
    local upstream_path="$2"
    local basename=$(basename "$localfile")

    if [ ! -f "$upstream_path" ]; then
        printf "  %-35s %s\n" "$basename" "(não existe em upstream — arquivo local original)"
        return
    fi

    if diff -q --strip-trailing-cr "$localfile" "$upstream_path" > /dev/null 2>&1; then
        printf "  %-35s ✓ idêntico ao upstream\n" "$basename"
        identical+=1
    else
        local nlines=$(diff --strip-trailing-cr "$localfile" "$upstream_path" 2>/dev/null | grep -c "^[<>]" || echo 0)
        printf "  %-35s ⚠ DIVERGE (%d linhas alteradas)\n" "$basename" "$nlines"
        differing+=1
        if [ "$VERBOSE" = "--verbose" ] || [ "$VERBOSE" = "-v" ]; then
            diff -u --strip-trailing-cr "$upstream_path" "$localfile" | head -30 | sed 's/^/      /'
            echo "      ..."
        fi
    fi
}

# Processa cada arquivo local que pode existir no upstream
for f in "$LOCAL_SRC"/*.cpp "$LOCAL_SRC"/*.c "$LOCAL_INC"/*.h; do
    [ ! -f "$f" ] && continue
    base=$(basename "$f")
    # Busca caminho upstream
    upstream_path=$(find "$UPSTREAM" -name "$base" -type f 2>/dev/null | head -1)
    [ -z "$upstream_path" ] && continue
    check_file "$f" "$upstream_path"
done

echo
echo "▶ Resumo:"
echo "  Idênticos: $identical (cópias puras — atualizar se upstream mudou)"
echo "  Diverge:   $differing (modificações locais — revisar antes de mergear)"
echo
if [ "$VERBOSE" != "--verbose" ] && [ "$VERBOSE" != "-v" ]; then
    echo "  Pra ver diff detalhado: $0 --verbose"
fi
echo "═══════════════════════════════════════════════════════════════════"
