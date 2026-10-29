#!/usr/bin/env python3
"""
tracegen.py -- memory trace generator for the Ramulator 2.0 HBM assignment.

Emits one request per line. Default format:

    <decimal_address> <R|W>

IMPORTANT: confirm this matches the trace frontend implementation you selected
in src/frontend/impl/memory_trace/ before generating multi-GB traces. Use
--format hex-space or --format addr-only if your frontend differs, or add your
own emitter in write_trace().

Patterns
--------
seq     sequential, stride = --granularity
stride  fixed stride of --stride bytes
rand    uniform random, --granularity-aligned, over --region bytes
kv      --streams concurrent streams, each reading a contiguous --block byte
        region from a random aligned base; round-robin interleaved at
        --granularity. Models decode-phase KV cache reads.
"""

import argparse
import random
import sys


def gen_seq(n, gran, base, region):
    addr = base
    for _ in range(n):
        yield addr
        addr += gran
        if addr >= base + region:
            addr = base


def gen_stride(n, stride, base, region):
    addr = base
    for _ in range(n):
        yield addr
        addr += stride
        if addr >= base + region:
            # offset by one granule so we don't retrace the same lines
            base += stride // 2 if stride > 1 else 1
            addr = base
    return


def gen_rand(n, gran, base, region, rng):
    slots = region // gran
    for _ in range(n):
        yield base + rng.randrange(slots) * gran


def gen_kv(n, streams, block, gran, base, region, rng):
    """Round-robin across `streams` contiguous blocks at random aligned bases."""
    if block % gran:
        sys.exit(f"--block ({block}) must be a multiple of --granularity ({gran})")
    if region < block * streams:
        sys.exit("--region too small for the requested streams x block footprint")

    lines_per_block = block // gran
    slots = region // block

    def fresh_bases():
        return [base + rng.randrange(slots) * block for _ in range(streams)]

    bases = fresh_bases()
    offsets = [0] * streams
    emitted = 0
    while emitted < n:
        for s in range(streams):
            if emitted >= n:
                return
            yield bases[s] + offsets[s] * gran
            offsets[s] += 1
            if offsets[s] == lines_per_block:
                # stream finished its block; start a new one elsewhere
                bases[s] = base + rng.randrange(slots) * block
                offsets[s] = 0
            emitted += 1


def write_trace(addrs, path, write_frac, fmt, rng):
    n = 0
    with open(path, "w", buffering=1 << 22) as f:
        for a in addrs:
            # EDIT: change the trace format
            op = "ST" if write_frac > 0 and rng.random() < write_frac else "LD"
            if fmt == "dec-space":
                f.write(f"{op} {a}\n")
            elif fmt == "hex-space":
                f.write(f"{op} 0x{a:x}\n")
            # EDIT END
            elif fmt == "addr-only":
                f.write(f"{a}\n")
            else:
                sys.exit(f"unknown --format {fmt}")
            n += 1
    return n


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pattern", required=True,
                   choices=["seq", "stride", "rand", "kv"])
    p.add_argument("--out", required=True, help="output trace path")
    p.add_argument("--n", type=int, default=2_000_000, help="number of requests")
    p.add_argument("--granularity", type=int, default=64,
                   help="bytes per request (default 64)")
    p.add_argument("--stride", type=int, default=4096,
                   help="stride in bytes for --pattern stride")
    p.add_argument("--region", type=int, default=1 << 30,
                   help="address footprint in bytes (default 1 GiB)")
    p.add_argument("--base", type=int, default=0, help="base address")
    p.add_argument("--streams", type=int, default=16,
                   help="concurrent streams for --pattern kv")
    p.add_argument("--block", type=int, default=8192,
                   help="bytes per contiguous block for --pattern kv")
    p.add_argument("--write-frac", type=float, default=0.0,
                   help="fraction of requests that are writes [0,1]")
    p.add_argument("--seed", type=int, default=1234)
    p.add_argument("--format", default="dec-space",
                   choices=["dec-space", "hex-space", "addr-only"])
    a = p.parse_args()

    if not (0.0 <= a.write_frac <= 1.0):
        sys.exit("--write-frac must be in [0, 1]")
    if a.granularity <= 0 or (a.granularity & (a.granularity - 1)):
        sys.exit("--granularity must be a positive power of two")

    rng = random.Random(a.seed)

    if a.pattern == "seq":
        addrs = gen_seq(a.n, a.granularity, a.base, a.region)
    elif a.pattern == "stride":
        addrs = gen_stride(a.n, a.stride, a.base, a.region)
    elif a.pattern == "rand":
        addrs = gen_rand(a.n, a.granularity, a.base, a.region, rng)
    else:
        addrs = gen_kv(a.n, a.streams, a.block, a.granularity,
                       a.base, a.region, rng)

    n = write_trace(addrs, a.out, a.write_frac, a.format, rng)
    print(f"wrote {n} requests to {a.out} "
          f"(pattern={a.pattern}, gran={a.granularity}B, "
          f"write_frac={a.write_frac}, seed={a.seed})", file=sys.stderr)


if __name__ == "__main__":
    main()
