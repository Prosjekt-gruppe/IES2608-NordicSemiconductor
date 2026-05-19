#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SRC_DIR="$SCRIPT_DIR/figures/source"
OUT_DIR="$SCRIPT_DIR/figures/generated"

mkdir -p "$OUT_DIR"

for texfile in "$SRC_DIR"/*.tex; do
    [ -e "$texfile" ] || continue

    if [ "$(basename "$texfile")" = "figure_defaults.tex" ]; then
        continue
    fi

    name="$(basename "$texfile" .tex)"

    echo "Building $name..."

    pdflatex \
        -interaction=nonstopmode \
        -halt-on-error \
        -output-directory="$OUT_DIR" \
        "$texfile"
done

echo "Done."