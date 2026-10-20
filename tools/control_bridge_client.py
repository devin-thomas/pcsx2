#!/usr/bin/env python3
"""Minimal Unix-socket diagnostic client for docs/CONTROL_BRIDGE.md."""

import argparse
import json
import socket
import struct
import time
import unittest

REQUEST = struct.Struct("<HBBQQIIHH")
REPLY = struct.Struct("<4H8Q6I2HIQ")
FIELDS = (
    "version status action_status flags epoch request action current applied completed "
    "applied_ns completed_ns executed_mask released_mask requested_count completed_count "
    "max_boundaries max_read_bytes max_ranges capabilities data_length action_epoch"
).split()
OK, PENDING = 0, 1
HELLO, ACQUIRE, STEP, QUERY, SNAPSHOT, CANCEL, RELEASE = range(7)


def encode(command, request_id, epoch, mask=0, count=0, ranges=(), version=1):
    if len(ranges) > 32:
        raise ValueError("at most 32 ranges")
    payload = bytes([0x10]) + REQUEST.pack(
        version, command, 0, epoch, request_id, mask, count, len(ranges), 0
    )
    payload += b"".join(struct.pack("<II", address, length) for address, length in ranges)
    return struct.pack("<I", len(payload) + 4) + payload


def decode(frame):
    if len(frame) < 5 or struct.unpack_from("<I", frame)[0] != len(frame):
        raise ValueError("invalid PINE frame length")
    if len(frame) == 5 and frame[4] == 255:
        return {"pine_status": 255}
    if len(frame) < 5 + REPLY.size:
        raise ValueError("truncated bridge reply")
    result = dict(zip(FIELDS, REPLY.unpack_from(frame, 5)))
    if result["version"] != 1 or len(frame) != 5 + REPLY.size + result["data_length"]:
        raise ValueError("unsupported version or invalid snapshot size")
    expected_tag = 0 if result["status"] in (OK, PENDING) else 255
    if frame[4] != expected_tag:
        raise ValueError("PINE and bridge status disagree")
    result["data"] = frame[5 + REPLY.size :]
    return result


class Client:
    def __init__(self, endpoint):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(3)
        self.socket.connect(endpoint)
        self.id = 0
        self.epoch = 0

    def close(self):
        self.socket.close()  # Native disconnect cleanup is also required on client failure.

    def _read(self, size):
        data = bytearray()
        while len(data) < size:
            block = self.socket.recv(size - len(data))
            if not block:
                raise EOFError("bridge disconnected")
            data.extend(block)
        return bytes(data)

    def send(self, command, **kwargs):
        self.id += 1
        self.socket.sendall(encode(command, self.id, self.epoch, **kwargs))
        header = self._read(4)
        length = struct.unpack("<I", header)[0]
        if length < 5 or length > 5 + REPLY.size + 4096:
            raise ValueError("unbounded response")
        result = decode(header + self._read(length - 4))
        if "request" in result and result["request"] != self.id:
            raise ValueError("request acknowledgment mismatch")
        if result.get("epoch"):
            self.epoch = result["epoch"]
        return result


def require(result, status=OK):
    if result.get("status") != status:
        raise RuntimeError(f"unexpected bridge result: {printable(result)}")
    return result


def printable(result):
    return {key: (value.hex() if isinstance(value, bytes) else value) for key, value in result.items()}


def smoke(endpoint, count, mask):
    client = Client(endpoint)
    try:
        hello = require(client.send(HELLO))
        assert hello["capabilities"] == 15 and hello["max_boundaries"] == 300
        print(json.dumps({"hello": printable(hello)}))
        # Capability/error-only mode works without booting a VM.
        if not count:
            require(client.send(QUERY, version=999), 4)
            require(client.send(255), 19)
            print(json.dumps({"checks": ["capabilities", "unsupported_version", "unknown_command"]}))
            return
        require(client.send(ACQUIRE))
        started = require(client.send(STEP, count=count, mask=mask), PENDING)
        assert started["flags"] & 2 and not started["flags"] & 4
        deadline = time.monotonic() + 11
        while True:
            result = require(client.send(QUERY))
            if result["action_status"] != PENDING:
                break
            if time.monotonic() >= deadline:
                raise TimeoutError("completion unknown; socket close requests native cleanup")
            time.sleep(0.01)
        assert result["action_status"] == OK and result["flags"] & 4
        assert result["action"] == started["request"]
        assert result["action_epoch"] == started["epoch"] == result["epoch"]
        assert result["completed_count"] == count
        assert result["completed"] - result["applied"] == count
        assert result["completed_ns"] > result["applied_ns"]
        assert result["executed_mask"] == mask and result["released_mask"] == 0
        snapshot = require(client.send(SNAPSHOT, ranges=((0, 16),)))
        assert snapshot["current"] == result["completed"] and len(snapshot["data"]) == 16
        # RAM contents deliberately omitted from diagnostic stdout.
        print(json.dumps({"completion": printable(result), "snapshot_bytes": len(snapshot["data"])}))
        require(client.send(RELEASE))
    finally:
        client.close()


class CodecTests(unittest.TestCase):
    def test_request_schema(self):
        frame = encode(STEP, 0x123456789ABCDEF, 42, mask=0x4081, count=17)
        self.assertEqual(len(frame), 37)
        self.assertEqual(frame[:5], b"%\x00\x00\x00\x10")
        self.assertEqual(REQUEST.unpack_from(frame, 5), (1, STEP, 0, 42, 0x123456789ABCDEF, 0x4081, 17, 0, 0))

    def test_reply_unknown_completion(self):
        values = [1, 16, 16, 0, 0, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 300, 4096, 32, 15, 0, 0]
        payload = REPLY.pack(*values)
        result = decode(struct.pack("<IB", len(payload) + 5, 255) + payload)
        self.assertEqual(result["status"], 16)
        self.assertEqual(result["flags"], 0)
        self.assertEqual(result["epoch"], 0)

    def test_rejects_truncated_or_conflicting_reply(self):
        with self.assertRaises(ValueError):
            decode(b"\x06\x00\x00\x00\x00\x01")
        payload = REPLY.pack(1, 16, 16, 0, *([0] * 18))
        with self.assertRaises(ValueError):
            decode(struct.pack("<IB", len(payload) + 5, 0) + payload)

    def test_legacy_failure_is_failure(self):
        self.assertEqual(decode(b"\x05\x00\x00\x00\xff"), {"pine_status": 255})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", help="explicit socket of an OWN isolated diagnostic instance")
    parser.add_argument("--steps", type=int, default=0, help="0: capability/error checks only; otherwise 1..300")
    parser.add_argument("--mask", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--self-test", action="store_true", help="codec fixtures only; does not contact an emulator")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(CodecTests)
        raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
    if not args.endpoint or not 0 <= args.steps <= 300 or not 0 <= args.mask <= 0xFFFF:
        parser.error("supply --endpoint, --steps 0..300, and a 16-bit mask")
    smoke(args.endpoint, args.steps, args.mask)


if __name__ == "__main__":
    main()
