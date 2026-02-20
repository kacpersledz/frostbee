#!/usr/bin/env bash
#
# Create a Zigbee OTA Upgrade file from a signed MCUboot binary.
# Follows Zigbee ZCL specification (OTA Upgrade cluster, §11.4).
#
# The resulting .zigbee file can be served by Zigbee2MQTT or ZHA.
#
# Usage:
#   ./create_ota.sh \
#     --input  build/app/zephyr/zephyr.signed.bin \
#     --output frostbee-v1.2.3.zigbee \
#     --manufacturer-id 0x1234 \
#     --image-type      0x0001 \
#     --file-version    0x01020300
#
# Version convention (matching MCUboot image version "1.2.3+0"):
#   v1.2.3 → file version 0x01020300
#   (major << 24) | (minor << 16) | (patch << 8) | 0

set -euo pipefail

# ── Defaults ──
HEADER_STRING="Frostbee OTA"

usage() {
    echo "Usage: $0 --input FILE --output FILE --manufacturer-id HEX --image-type HEX --file-version HEX"
    echo ""
    echo "  --input            Signed MCUboot binary (.bin)"
    echo "  --output           Output Zigbee OTA file (.zigbee)"
    echo "  --manufacturer-id  Manufacturer code, hex (e.g. 0x1234)"
    echo "  --image-type       Image type, hex (e.g. 0x0001)"
    echo "  --file-version     File version, hex (e.g. 0x01020300)"
    echo "  --header-string    OTA header string, max 32 chars (default: 'Frostbee OTA')"
    exit 1
}

# ── Parse arguments ──
while [[ $# -gt 0 ]]; do
    case "$1" in
        --input)            INPUT="$2";           shift 2 ;;
        --output)           OUTPUT="$2";          shift 2 ;;
        --manufacturer-id)  MANUFACTURER_ID="$2"; shift 2 ;;
        --image-type)       IMAGE_TYPE="$2";      shift 2 ;;
        --file-version)     FILE_VERSION="$2";    shift 2 ;;
        --header-string)    HEADER_STRING="$2";   shift 2 ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

for var in INPUT OUTPUT MANUFACTURER_ID IMAGE_TYPE FILE_VERSION; do
    if [[ -z "${!var:-}" ]]; then
        echo "Error: --$(echo "$var" | tr '_' '-' | tr '[:upper:]' '[:lower:]') is required"
        usage
    fi
done

if [[ ! -f "$INPUT" ]]; then
    echo "Error: input file not found: $INPUT"
    exit 1
fi

# ── Convert hex strings to integers ──
MFR=$((MANUFACTURER_ID))
IMG=$((IMAGE_TYPE))
FVER=$((FILE_VERSION))

# ── Constants ──
FILE_ID=0x0BEEF11E    # Zigbee OTA magic
HDR_VERSION=0x0100
FIELD_CTRL=0x0000     # No optional fields
ZBEE_STACK=0x0002     # Zigbee Pro
TAG_IMAGE=0x0000      # Subelement tag: Upgrade Image
HDR_LEN=56            # Fixed header size (no optional fields)

# ── Helper: write little-endian bytes ──
# Usage: le16 VALUE, le32 VALUE
le16() { printf '\\x%02x\\x%02x' $(($1 & 0xFF)) $((($1 >> 8) & 0xFF)); }
le32() { printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($1 & 0xFF)) $((($1 >> 8) & 0xFF)) $((($1 >> 16) & 0xFF)) $((($1 >> 24) & 0xFF)); }

# ── Build the file ──
PAYLOAD_SIZE=$(stat -c%s "$INPUT")
SUBELEMENT_OVERHEAD=6  # tag (2) + length (4)
TOTAL_SIZE=$((HDR_LEN + SUBELEMENT_OVERHEAD + PAYLOAD_SIZE))

# Pad/truncate header string to exactly 32 bytes
HDR_STR_HEX=$(printf '%-32s' "$HEADER_STRING" | head -c 32 | xxd -p)

{
    # Header (56 bytes, all little-endian)
    le32 $FILE_ID
    le16 $HDR_VERSION
    le16 $HDR_LEN
    le16 $FIELD_CTRL
    le16 $MFR
    le16 $IMG
    le32 $FVER
    le16 $ZBEE_STACK
    # Header string (32 bytes, ASCII padded with spaces)
    printf '%s' "$HDR_STR_HEX" | sed 's/../\\x&/g' | xargs -0 printf
    le32 $TOTAL_SIZE

    # Subelement header
    le16 $TAG_IMAGE
    le32 $PAYLOAD_SIZE

    # Payload (the signed MCUboot binary)
    cat "$INPUT"
} > "$OUTPUT"

# ── Summary ──
ACTUAL_SIZE=$(stat -c%s "$OUTPUT")
VMAJ=$(( (FVER >> 24) & 0xFF ))
VMIN=$(( (FVER >> 16) & 0xFF ))
VPAT=$(( (FVER >> 8) & 0xFF ))
VBLD=$(( FVER & 0xFF ))

echo "Written: $OUTPUT  ($ACTUAL_SIZE bytes)"
echo "  Manufacturer : 0x$(printf '%04X' $MFR)"
echo "  Image type   : 0x$(printf '%04X' $IMG)"
echo "  File version : 0x$(printf '%08X' $FVER)  ($VMAJ.$VMIN.$VPAT.$VBLD)"
echo "  Payload size : $PAYLOAD_SIZE bytes"

if [[ "$ACTUAL_SIZE" -ne "$TOTAL_SIZE" ]]; then
    echo "WARNING: expected $TOTAL_SIZE bytes but wrote $ACTUAL_SIZE bytes"
    exit 1
fi
