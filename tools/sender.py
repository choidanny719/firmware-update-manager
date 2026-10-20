import argparse
from pathlib import Path
import struct
import sys
import zlib

IMAGE_CAPACITY = 256 * 1024


def encode_packet(kind, sequence, payload):
    header = struct.pack("<2sBIH", b"\x55\xaa", kind, sequence, len(payload))
    checked = header + payload
    return checked + struct.pack("<I", zlib.crc32(checked))


def package_image(image, version, chunk_size=256):
    if not isinstance(image, bytes) or not 1 <= len(image) <= IMAGE_CAPACITY:
        raise ValueError("image size must be between 1 and 262144 bytes")
    if type(version) is not int or not 1 <= version <= 0xffffffff:
        raise ValueError("version must be between 1 and 4294967295")
    if type(chunk_size) is not int or not 1 <= chunk_size <= 256:
        raise ValueError("chunk size must be between 1 and 256 bytes")
    manifest = struct.pack("<III", version, len(image), zlib.crc32(image))
    packets = [encode_packet(1, 0, manifest)]
    sequence = 1
    for offset in range(0, len(image), chunk_size):
        packets.append(encode_packet(2, sequence, image[offset:offset + chunk_size]))
        sequence += 1
    packets.append(encode_packet(3, sequence, b""))
    return b"".join(packets)


def main():
    parser = argparse.ArgumentParser(description="Package a firmware image for the simulated serial receiver.")
    parser.add_argument("image", type=Path)
    parser.add_argument("--version", required=True, type=int)
    parser.add_argument("--chunk-size", default=256, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if not args.image.is_file():
            raise ValueError("image must be a regular file")
        with args.image.open("rb") as source:
            image = source.read(IMAGE_CAPACITY + 1)
        encoded = package_image(image, args.version, args.chunk_size)
        with args.output.open("xb") as output:
            output.write(encoded)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
