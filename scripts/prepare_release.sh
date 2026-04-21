#!/usr/bin/env bash
# Bundle release artifacts with the exact filenames the install page /
# webflasher manifests expect. Run before tagging a new version.
#
# Output lands in dist/<version>/:
#   poseidon-factory.bin            (Cardputer-Adv standalone, flash at 0x0)
#   poseidon-launcher.bin           (app-only image for bmorcelli's Launcher)
#   trident-factory.bin             (TRIDENT / ESP32-C5 satellite, flash at 0x0)
#
# The Web Flasher on the site pulls from GitHub Releases'
#   /releases/latest/download/<name>
# so the filenames here MUST match docs/manifest-trident.json.
set -e

# esptool is often user-installed under ~/.local/bin (pip install --user)
# and that directory isn't on PATH by default — make sure it is here so
# `python -m esptool` and the merge-bin step both resolve.
export PATH="$HOME/.local/bin:$PATH"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-dev}"
OUT="$REPO/dist/$VERSION"
mkdir -p "$OUT"

echo "[release] bundling $VERSION -> $OUT"

# POSEIDON standalone (full factory image at 0x0).
SRC1="$REPO/.pio/build/cardputer/firmware.factory.bin"
if [ -f "$SRC1" ]; then
    cp "$SRC1" "$OUT/poseidon-factory.bin"
    echo "  ok  poseidon-factory.bin        $(stat -c %s "$OUT/poseidon-factory.bin") bytes"
else
    echo "  MISS poseidon-factory.bin       (build cardputer env first)"
fi

# POSEIDON for bmorcelli's Launcher (app-only, installs into ota_0).
SRC2="$REPO/.pio/build/cardputer-launcher/firmware.bin"
if [ -f "$SRC2" ]; then
    cp "$SRC2" "$OUT/poseidon-launcher.bin"
    echo "  ok  poseidon-launcher.bin       $(stat -c %s "$OUT/poseidon-launcher.bin") bytes"
else
    echo "  MISS poseidon-launcher.bin      (build cardputer-launcher env first)"
fi

# TRIDENT (ESP32-C5 satellite) factory image.
SRC3="$REPO/c5_node/build/poseidon_c5_node.bin"
BL3="$REPO/c5_node/build/bootloader/bootloader.bin"
PT3="$REPO/c5_node/build/partition_table/partition-table.bin"
if [ -f "$SRC3" ] && [ -f "$BL3" ] && [ -f "$PT3" ]; then
    # Merge bootloader + partition table + app into a single factory bin at 0x0
    # so the webflasher / esptool "write_flash 0x0 ..." both work.
    python -m esptool --chip esp32c5 merge-bin \
        --output "$OUT/trident-factory.bin" \
        0x0    "$BL3" \
        0x8000 "$PT3" \
        0x10000 "$SRC3" >/dev/null
    echo "  ok  trident-factory.bin         $(stat -c %s "$OUT/trident-factory.bin") bytes"
else
    echo "  MISS trident-factory.bin        (cd c5_node && idf.py build first)"
fi

echo
echo "Upload the files in $OUT as release assets on the v$VERSION tag."
echo "Names must match exactly — the webflasher manifests are hard-wired."
