#!/usr/bin/env python3
"""
Create a Zigbee OTA Upgrade file (Zigbee spec r22, section 11.4)
from a signed MCUboot application binary.

The resulting .zigbee file can be placed in Zigbee2MQTT's OTA directory.
Z2M will serve it to devices whose manufacturer code and image type match.

Version convention:
  Git tag v1.2.3  →  file version 0x01020300
  This matches the MCUboot image version "1.2.3+0" packed as:
    (major << 24) | (minor << 16) | (patch << 8) | 0

Usage:
  python3 create_ota.py \\
    --input  build/app/zephyr/zephyr.signed.bin \\
    --output frostbee-v1.2.3.zigbee \\
    --manufacturer-id 0x1234 \\
    --image-type      0x0001 \\
    --file-version    0x01020300
"""

import argparse
import struct
import sys

# ── Zigbee OTA Upgrade file format constants ─────────────────────────────────
_FILE_ID      = 0x0BEEF11E  # magic: "upgrade file identifier"
_HDR_VERSION  = 0x0100
_FIELD_CTRL   = 0x0000      # no optional fields (min/max HW version, etc.)
_ZBEE_STACK   = 0x0002      # Zigbee Pro
_TAG_IMAGE    = 0x0000      # subelement tag: Upgrade Image
_HDR_LEN      = 56          # fixed header size when no optional fields are set


def create_ota(in_path, out_path, mfr, img_type, file_ver, hdr_string):
    with open(in_path, "rb") as f:
        payload = f.read()

    # Subelement: tag (2) + length (4) + binary payload
    subelement = struct.pack("<HI", _TAG_IMAGE, len(payload)) + payload
    total_size = _HDR_LEN + len(subelement)

    hdr_str_bytes = hdr_string.encode("ascii")[:32].ljust(32, b"\x00")

    # Header layout (56 bytes, all little-endian):
    #   4  Upgrade File Identifier
    #   2  Header Version
    #   2  Header Length
    #   2  Field Control
    #   2  Manufacturer Code
    #   2  Image Type
    #   4  File Version
    #   2  Zigbee Stack Version
    #  32  OTA Header String
    #   4  Total Image Size
    header = struct.pack(
        "<IHHHHHIH32sI",
        _FILE_ID,
        _HDR_VERSION,
        _HDR_LEN,
        _FIELD_CTRL,
        mfr,
        img_type,
        file_ver,
        _ZBEE_STACK,
        hdr_str_bytes,
        total_size,
    )
    assert len(header) == _HDR_LEN, f"Header size mismatch: {len(header)}"

    with open(out_path, "wb") as f:
        f.write(header + subelement)

    print(f"Written: {out_path}  ({total_size:,} bytes)")
    print(f"  Manufacturer : 0x{mfr:04X}")
    print(f"  Image type   : 0x{img_type:04X}")
    print(f"  File version : 0x{file_ver:08X}  "
          f"({(file_ver >> 24) & 0xFF}."
          f"{(file_ver >> 16) & 0xFF}."
          f"{(file_ver >> 8) & 0xFF}."
          f"{file_ver & 0xFF})")
    print(f"  Payload size : {len(payload):,} bytes")


def main():
    p = argparse.ArgumentParser(
        description="Wrap a signed MCUboot binary into a Zigbee OTA Upgrade file."
    )
    p.add_argument("--input",           required=True,
                   help="Signed MCUboot binary (.bin)")
    p.add_argument("--output",          required=True,
                   help="Output Zigbee OTA file (.zigbee)")
    p.add_argument("--manufacturer-id", required=True,
                   help="Manufacturer code, hex (e.g. 0x1234). "
                        "Must match CONFIG_ZIGBEE_FOTA_MANUFACTURER_ID.")
    p.add_argument("--image-type",      required=True,
                   help="Image type, hex (e.g. 0x0001). "
                        "Must match CONFIG_ZIGBEE_FOTA_IMAGE_TYPE.")
    p.add_argument("--file-version",    required=True,
                   help="File version, hex (e.g. 0x01020300 for v1.2.3). "
                        "Must be greater than the version on the device.")
    p.add_argument("--header-string",   default="Frostbee OTA",
                   help="OTA header string, max 32 ASCII chars (default: 'Frostbee OTA')")
    args = p.parse_args()

    create_ota(
        args.input,
        args.output,
        int(args.manufacturer_id, 0),
        int(args.image_type, 0),
        int(args.file_version, 0),
        args.header_string,
    )


if __name__ == "__main__":
    main()
