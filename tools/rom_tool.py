import sys
import argparse
import struct

ROM_SIZE = 32 * 1024  # 32KB (M27C256)


# ---- File loaders ----


def load_bin(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) > ROM_SIZE:
        raise ValueError(
            f"File is {len(data)} bytes, exceeds ROM size ({ROM_SIZE} bytes)"
        )
    if len(data) < ROM_SIZE:
        print(f"  Padding {ROM_SIZE - len(data)} bytes with 0xFF")
        data = data + b"\xff" * (ROM_SIZE - len(data))
    return data


def load_hex(path: str) -> bytes:
    rom = bytearray(b"\xff" * ROM_SIZE)
    base = 0
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line.startswith(":"):
                continue
            raw = bytes.fromhex(line[1:])
            if sum(raw) & 0xFF != 0:
                raise ValueError(f"Checksum error at line {lineno}")
            byte_count = raw[0]
            address = struct.unpack_from(">H", raw, 1)[0]
            rec_type = raw[3]
            payload = raw[4 : 4 + byte_count]

            if rec_type == 0x00:  # Data
                addr = base + address
                if addr + byte_count > ROM_SIZE:
                    raise ValueError(
                        f"Data at 0x{addr:04X}+{byte_count} exceeds ROM size at line {lineno}"
                    )
                rom[addr : addr + byte_count] = payload
            elif rec_type == 0x01:  # End Of File
                break
            elif rec_type == 0x02:  # Extended Segment Address
                base = struct.unpack_from(">H", payload)[0] << 4
            elif rec_type == 0x04:  # Extended Linear Address
                base = struct.unpack_from(">H", payload)[0] << 16
            elif rec_type in (0x03, 0x05):  # Start address records - ignore
                pass
            else:
                raise ValueError(
                    f"Unknown record type 0x{rec_type:02X} at line {lineno}"
                )
    return bytes(rom)


def detect_and_load(path: str) -> bytes:
    ext = path.lower().rsplit(".", 1)[-1] if "." in path else ""
    if ext == "hex":
        print("  Format: Intel HEX")
        return load_hex(path)
    elif ext in ("bin", "rom", "epr"):
        print("  Format: Binary")
        return load_bin(path)
    else:
        with open(path, "rb") as f:
            sig = f.read(1)
        if sig == b":":
            print("  Format: Intel HEX (detected by content)")
            return load_hex(path)
        else:
            print("  Format: Binary (detected by content)")
            return load_bin(path)


# ---- File savers ----


def save_hex(path: str, data: bytes) -> None:
    RECORD_LEN = 16
    lines = []
    for addr in range(0, len(data), RECORD_LEN):
        chunk = data[addr : addr + RECORD_LEN]
        if all(b == 0xFF for b in chunk):  # skip erased (0xFF) regions
            continue
        n = len(chunk)
        record = bytes([n, (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + chunk
        checksum = (-sum(record)) & 0xFF
        lines.append(f":{record.hex().upper()}{checksum:02X}")
    lines.append(":00000001FF")  # EOF record
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def save_file(path: str, data: bytes) -> None:
    ext = path.lower().rsplit(".", 1)[-1] if "." in path else ""
    if ext == "hex":
        print("  Format: Intel HEX")
        save_hex(path, data)
    else:
        print("  Format: Binary")
        with open(path, "wb") as f:
            f.write(data)


# ---- Serial helpers ----


def open_port(port: str):
    try:
        import serial
    except ImportError:
        print("Error: pyserial not installed. Run: pip install pyserial")
        sys.exit(1)
    return serial.Serial(port, baudrate=115200, timeout=10)


# ---- Commands ----


def cmd_write(args):
    print(f"[Write]  Port: {args.port}  File: {args.file}")
    data = detect_and_load(args.file)
    assert len(data) == ROM_SIZE

    with open_port(args.port) as ser:
        ser.reset_input_buffer()
        print("  Sending 'W' command ...")
        ser.write(b"W")

        print(f"  Writing {ROM_SIZE} bytes", end="", flush=True)
        chunk = 256
        for i in range(0, ROM_SIZE, chunk):
            ser.write(data[i : i + chunk])
            if (i // chunk) % 32 == 31:
                print(".", end="", flush=True)
        print()

        print("  Waiting for ACK ...")
        ack = ser.read(1)
        if ack == b"K":
            print("  Done: Write successful.")
        elif ack == b"X":
            print("  Error: Device reported timeout/abort.")
            sys.exit(1)
        else:
            print(f"  Error: Unexpected response: {(ack + ser.read(64))!r}")
            sys.exit(1)


def cmd_read(args):
    print(f"[Read]  Port: {args.port}  Output: {args.file}")
    print(f"  Reading {ROM_SIZE} bytes", end="", flush=True)

    with open_port(args.port) as ser:
        try:
            data = do_read(ser)
        except IOError as e:
            print(f"  Error: {e}")
            sys.exit(1)

    save_file(args.file, data)
    print(f"  Done: {ROM_SIZE} bytes saved to {args.file}")


def do_read(ser) -> bytes:
    ser.reset_input_buffer()
    ser.write(b"R")
    data = bytearray()
    while len(data) < ROM_SIZE:
        chunk = ser.read(ROM_SIZE - len(data))
        if not chunk:
            raise IOError(f"Timeout after {len(data)} bytes.")
        data.extend(chunk)
        dots = len(data) // (ROM_SIZE // 32)
        print(
            "\r  Reading %d bytes  %s" % (len(data), "." * dots),
            end="",
            flush=True,
        )
    print()
    return bytes(data)


def print_diff(expected: bytes, actual: bytes) -> int:
    diffs = [
        (i, expected[i], actual[i])
        for i in range(ROM_SIZE)
        if expected[i] != actual[i]
    ]
    if not diffs:
        return 0
    print(f"  {len(diffs)} byte(s) differ:")
    for addr, exp, got in diffs[:32]:
        print(f"    0x{addr:04X}: expected 0x{exp:02X}, got 0x{got:02X}")
    if len(diffs) > 32:
        print(f"    ... and {len(diffs) - 32} more")
    # Check if it looks like a systematic offset
    offsets = [
        got - exp for _, exp, got in diffs if exp != 0xFF and got != 0xFF
    ]
    if offsets and len(set(offsets)) == 1:
        print(
            f"  Hint: all differences are a constant offset of {offsets[0]:+d} -- possible address shift?"
        )
    return len(diffs)


def cmd_verify(args):
    print(f"[Verify]  Port: {args.port}  File: {args.file}")
    expected = detect_and_load(args.file)

    with open_port(args.port) as ser:
        actual = do_read(ser)

    n = print_diff(expected, actual)
    if n == 0:
        print("  OK: Device content matches file exactly.")
    else:
        print(f"  FAIL: {n} byte(s) differ.")
        sys.exit(1)


def cmd_erase(args):
    print(f"[Erase]  Port: {args.port}")
    with open_port(args.port) as ser:
        ser.reset_input_buffer()
        ser.write(b"E")
        ack = ser.read(1)
        if ack == b"K":
            print("  Done: Erase successful.")
        else:
            print(f"  Error: Unexpected response: {(ack + ser.read(64))!r}")
            sys.exit(1)


def cmd_info(args):
    print(f"[Info]  Port: {args.port}")
    with open_port(args.port) as ser:
        ser.reset_input_buffer()
        ser.write(b"I")
        import time

        time.sleep(0.2)
        resp = ser.read(256)
        print(resp.decode("utf-8", errors="replace"))


# ---- Entry point ----


def main():
    parser = argparse.ArgumentParser(
        description="RP-ROM EPROM Emulator tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  python rom_tool.py write COM3 firmware.bin
  python rom_tool.py write COM3 firmware.hex
  python rom_tool.py read  COM3 dump.bin
  python rom_tool.py read  COM3 dump.hex
  python rom_tool.py verify COM3 firmware.bin
  python rom_tool.py erase COM3
  python rom_tool.py info  COM3
""",
    )
    sub = parser.add_subparsers(dest="command", metavar="command")
    sub.required = True

    p_write = sub.add_parser("write", help="Write ROM image to device")
    p_write.add_argument("port", help="Serial port (e.g. COM3)")
    p_write.add_argument("file", help="ROM image (.bin or .hex)")
    p_write.set_defaults(func=cmd_write)

    p_read = sub.add_parser("read", help="Read ROM image from device")
    p_read.add_argument("port", help="Serial port (e.g. COM3)")
    p_read.add_argument("file", help="Output file (.bin)")
    p_read.set_defaults(func=cmd_read)

    p_verify = sub.add_parser(
        "verify", help="Verify device content against a file"
    )
    p_verify.add_argument("port", help="Serial port (e.g. COM3)")
    p_verify.add_argument("file", help="ROM image to compare (.bin or .hex)")
    p_verify.set_defaults(func=cmd_verify)

    p_erase = sub.add_parser("erase", help="Erase ROM (fill with 0xFF)")
    p_erase.add_argument("port", help="Serial port (e.g. COM3)")
    p_erase.set_defaults(func=cmd_erase)

    p_info = sub.add_parser("info", help="Print device info")
    p_info.add_argument("port", help="Serial port (e.g. COM3)")
    p_info.set_defaults(func=cmd_info)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
