#!/usr/bin/env python3
"""Golden fixture generator for dslab analysis tests (tests/fixtures).

Builds synthetic capture runs (capture.raw in dslogic-cross-v1 + capture.json)
with *independently implemented* packing, so the C unpacker/decoders are
cross-checked against a second implementation of the wire format.

Fixtures:
  square1k      D0=1kHz 50% duty square @1MHz, D1=same delayed 100 samples
  square_jitter D0=period alternating 480/520 samples (jitter range)
  uart_8n1      100000 baud 8N1 bytes "Hi!" + one frame with a bad stop bit
  i2c_txns      write 0x50 [0xBE 0xEF] ACK; restart read 0x50 1 byte NACK;
                write 0x37 -> address NACK
  spi_mode0     two CS-framed 2-byte exchanges @ mode0, MSB first
"""
import json
import os
import sys


def pack_cross(channels, n):
    """channels: list of per-slot sample bit lists; returns raw bytes."""
    out = bytearray()
    for g in range(0, n, 64):
        for c in channels:
            block = bytearray(8)
            for k in range(64):
                s = g + k
                if s < n and c[s]:
                    block[k // 8] |= 1 << (k % 8)
            out += block
    return bytes(out)


def square(n, period, duty=0.5, phase=0, high_init=None):
    sig = [0] * n
    for i in range(n):
        ph = (i - phase) % period
        sig[i] = 1 if ph < period * duty else 0
    if high_init is not None:
        sig[0] = high_init
    return sig


def jitter_square(n, periods, duty=0.5):
    sig = [0] * n
    i = 0
    k = 0
    while i < n:
        p = periods[k % len(periods)]
        hi = int(p * duty)
        for j in range(min(p, n - i)):
            sig[i + j] = 1 if j < hi else 0
        i += p
        k += 1
    return sig


def uart_frames(bytes_, spb, stop_bits=1, bad_stop_index=-1):
    """Returns (signal, frame_start_samples)."""
    sig = []
    starts = []
    for idx, b in enumerate(bytes_):
        starts.append(len(sig))
        frame = [0] * int(spb)                      # start bit
        for k in range(8):
            frame += [(b >> k) & 1] * int(spb)      # LSB first
        stop = [0] if idx == bad_stop_index else [1]
        frame += stop * int(spb) * stop_bits
        sig += frame
        if idx == bad_stop_index:
            # let the line recover to idle after the broken frame
            sig += [1] * int(spb)
    return sig, starts


class Sig:
    """Multi-channel time-series builder with per-sample level setting."""

    def __init__(self, names):
        self.names = names
        self.ch = {nm: [] for nm in names}

    def hold(self, levels, count):
        for _ in range(count):
            for nm in self.names:
                prev = self.ch[nm][-1] if self.ch[nm] else 0
                self.ch[nm].append(levels.get(nm, prev))

    def total(self):
        return len(self.ch[self.names[0]])

    def get(self):
        return [self.ch[nm] for nm in self.names]


def build_i2c(rate=1_000_000, half=5):
    s = Sig(["D0", "D1"])  # D0=SCL, D1=SDA
    s.hold({"D0": 1, "D1": 1}, 20)                 # idle
    txns = []

    def bit(b):
        s.hold({"D0": 0, "D1": b}, half)           # data valid while low
        s.hold({"D0": 1, "D1": b}, half)           # sampled on rising

    def start():
        s.hold({"D0": 1, "D1": 0}, half * 2)       # SDA falls while SCL high

    def stop():
        s.hold({"D0": 0, "D1": 0}, half)
        s.hold({"D0": 1, "D1": 0}, half)           # setup
        s.hold({"D0": 1, "D1": 1}, half * 2)       # SDA rises while SCL high

    def byte(v, ack):
        for k in range(7, -1, -1):
            bit((v >> k) & 1)
        bit(0 if ack == "ACK" else 1)

    # txn 1: write 0x50, data BE EF, all ACK
    start()
    txns.append({"addr": 0x50, "rw": "write", "bytes": [0xBE, 0xEF], "acks": ["ACK"] * 3})
    byte(0x50 << 1 | 0, "ACK")
    byte(0xBE, "ACK")
    byte(0xEF, "ACK")
    stop()
    s.hold({"D0": 1, "D1": 1}, 20)

    # txn 2: restart, read 0x50, one byte, NACK from master
    start()
    txns.append({"addr": 0x50, "rw": "read", "bytes": [0x77], "acks": ["ACK", "NACK"]})
    byte(0x50 << 1 | 1, "ACK")
    byte(0x77, "NACK")
    stop()
    s.hold({"D0": 1, "D1": 1}, 20)

    # txn 3: write 0x37, address NACK, stop
    start()
    txns.append({"addr": 0x37, "rw": "write", "bytes": [], "acks": ["NACK"]})
    byte(0x37 << 1 | 0, "NACK")
    stop()
    s.hold({"D0": 1, "D1": 1}, 20)

    return s, txns


def build_spi(rate=1_000_000, half=5, mode=0):
    s = Sig(["D0", "D1", "D2", "D3"])  # clk, mosi, miso, cs
    txns = []

    def idle():
        s.hold({"D0": 0, "D3": 1}, half * 2)  # CPOL=0 idle low

    def exchange(mosi_bytes, miso_bytes):
        s.hold({"D0": 0, "D3": 0}, half * 2)   # CS falls while clock low
        t0 = s.total() - 1
        for mb, ib in zip(mosi_bytes, miso_bytes):
            for k in range(7, -1, -1):          # MSB first
                s.hold({"D0": 0, "D1": (mb >> k) & 1, "D2": (ib >> k) & 1, "D3": 0}, half)
                s.hold({"D0": 1, "D1": (mb >> k) & 1, "D2": (ib >> k) & 1, "D3": 0}, half)
        s.hold({"D0": 0, "D3": 0}, half)
        s.hold({"D0": 0, "D3": 1}, half * 2)
        txns.append({"mosi": mosi_bytes, "miso": miso_bytes, "start": t0})

    idle()
    exchange([0xA5, 0x3A], [0x00, 0xFF])
    idle()
    exchange([0x81], [0x7E])
    idle()
    return s, txns


def write_run(outdir, channels, names, rate, complete=True, overflow=False):
    os.makedirs(outdir, exist_ok=True)
    n = len(channels[0])
    raw = pack_cross(channels, n)
    with open(os.path.join(outdir, "capture.raw"), "wb") as f:
        f.write(raw)
    meta = {
        "schema_version": "1.0",
        "tool": "dslab-fixtures",
        "command": "fixture",
        "device": {"model": "fixture"},
        "sample_format": {
            "type": "dslogic-cross-v1",
            "description": "fixture: channel-interleaved 64-sample blocks, LSB-first",
            "channel_slots": len(names),
        },
        "capture": {
            "sample_rate_hz": rate,
            "requested_samples": n,
            "expected_samples_aligned": n,
            "received_samples": n,
            "expected_bytes": len(raw),
            "received_bytes": len(raw),
            "mode": "stream",
            "channel_mode": "fixture",
            "duration_ms": n // (rate // 1000),
            "raw_path": "capture.raw",
            "events_path": "events.jsonl",
        },
        "enabled_channels": names,
        "trigger": {"triggered": False, "remain_cnt": 0},
        "integrity": {"complete": complete, "overflow": overflow,
                      "packet_errors": False, "static_waveform": False,
                      "final_state": "complete"},
    }
    with open(os.path.join(outdir, "capture.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)
    with open(os.path.join(outdir, "events.jsonl"), "w", encoding="utf-8") as f:
        f.write('{"event":"fixture"}\n')
    return n


def generate(base):
    made = {}

    n = 50048  # multiple of 64: no phantom edges in the final padded group
    # 2 kHz square (500 samples/period @1 MHz), D0 starts LOW so both
    # channels have clean first rising edges; D1 lags D0 by 100 samples
    d0 = square(n, 500, 0.5, 250)
    d1 = square(n, 500, 0.5, 350)
    made["square1k"] = write_run(os.path.join(base, "square1k"), [d0, d1],
                                 ["D0", "D1"], 1_000_000)

    n = 48000
    d0 = jitter_square(n, [480, 520])
    made["square_jitter"] = write_run(os.path.join(base, "square_jitter"), [d0],
                                      ["D0"], 1_000_000)

    rate = 1_000_000
    baud = 100000
    spb = rate / baud
    payload = [ord(c) for c in "Hi!"] + [0x00, 0xFF]
    sig, starts = uart_frames(payload, spb, bad_stop_index=3)
    made["uart_8n1"] = write_run(os.path.join(base, "uart_8n1"), [sig], ["D0"], rate)
    made["uart_8n1_meta"] = {"bytes": payload, "starts": starts, "bad_stop": 3,
                             "baud": baud, "spb": spb}

    s, txns = build_i2c()
    made["i2c_txns"] = write_run(os.path.join(base, "i2c_txns"), s.get(),
                                 ["D0", "D1"], rate)
    made["i2c_txns_meta"] = {"txns": txns}

    s2, stxns = build_spi()
    made["spi_mode0"] = write_run(os.path.join(base, "spi_mode0"), s2.get(),
                                  ["D0", "D1", "D2", "D3"], rate)
    made["spi_mode0_meta"] = {"txns": stxns}
    return made


if __name__ == "__main__":
    out = sys.argv[1]
    info = generate(out)
    print(json.dumps({k: (v if not isinstance(v, dict) else "...") for k, v in info.items()},
                     indent=1))
