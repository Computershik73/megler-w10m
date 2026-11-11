#!/usr/bin/env python3
"""Inspect the opt-in droidguard test app's captures without loading Google code.

Run with Python's isolated mode: python -I tools/droidguard-inspect.py CAPTURE_DIR
The field numbers are those of GmsCore 264330027. Output describes the messages;
it does not print the collected signals, device identifiers or encrypted token.
"""
import argparse
import json
from pathlib import Path
import re
import sys


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset == len(data):
            raise ValueError("truncated varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            if value > 0xffffffffffffffff:
                raise ValueError("varint exceeds 64 bits")
            return value, offset
    raise ValueError("unterminated varint")


def fields(data):
    result = []
    offset = 0
    while offset < len(data):
        if len(result) == 4096:
            raise ValueError("too many fields")
        tag, offset = varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if not 0 < number < (1 << 29):
            raise ValueError("invalid field number")
        if wire == 0:
            value, offset = varint(data, offset)
        else:
            if wire == 2:
                length, offset = varint(data, offset)
            elif wire in (1, 5):
                length = 8 if wire == 1 else 4
            else:
                raise ValueError("unsupported wire type %d" % wire)
            if length > len(data) - offset:
                raise ValueError("field extends past the message")
            value = data[offset:offset + length]
            offset += length
        result.append((number, wire, value))
    return result


def read(path):
    if path.stat().st_size > 2 << 20:
        raise ValueError("capture exceeds 2 MiB: " + path.name)
    return path.read_bytes()


def inspect(directory):
    captures = []
    for path in directory.glob("*.bin"):
        match = re.fullmatch(r"(\d+)-t(\d+)-(.+)\.bin", path.name)
        if match:
            captures.append((int(match[1]), int(match[2]), match[3], path))
    captures.sort()
    reports = []
    for sequence, thread, kind, request in captures:
        if kind != "request-fast":
            continue
        previous = [entry for entry in captures if entry[1] == thread and entry[0] < sequence and entry[2] == "vm-result"]
        next_requests = [entry[0] for entry in captures if entry[1] == thread and entry[0] > sequence and entry[2].startswith("request-")]
        stop = min(next_requests) if next_requests else float("inf")
        following = [entry for entry in captures if entry[1] == thread and sequence < entry[0] < stop and entry[2] == "signed-fast-body"]
        if not previous or not following:
            raise ValueError("incomplete capture around " + request.name)
        vm = previous[-1][3]
        response = following[0][3]
        results = [value for number, wire, value in fields(read(request)) if number == 10 and wire == 2]
        if len(results) != 1 or results[0] != read(vm):
            raise ValueError("fast field 10 differs from the preceding VM result: " + request.name)
        body = read(response)
        parsed = fields(body)
        reports.append({
            "request": request.name,
            "vm_result": vm.name,
            "result_bytes": len(results[0]),
            "result_matches": True,
            "response": response.name,
            "response_bytes": len(body),
            "bytecode_present": any(number == 1 for number, _, _ in parsed),
            "fields": [{"number": number, "wire": wire,
                        **({"bytes": len(value)} if isinstance(value, bytes) else {"value": value})}
                       for number, wire, value in parsed],
        })
    if not reports:
        raise ValueError("no complete fast exchange found")
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.capture_dir), indent=2))
    except (OSError, ValueError) as error:
        print("DroidGuard capture: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
