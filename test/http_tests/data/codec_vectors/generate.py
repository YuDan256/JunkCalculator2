#!/usr/bin/env python3
"""Generate data/codec_vectors/vectors.json for the JC2 `inflate` module tests.

Everything compressed here is produced by CPython's zlib (the reference RFC 1950 /
RFC 1951 / RFC 1952 implementation).  The JC2 side only *decompresses*, so these
vectors are an independent implementation to check the hand-written container
layer (and, transitively, the hand-written raw DEFLATE decoder in
src/utils/deflate/Deflate.cpp) against.

Run:
    python data/codec_vectors/generate.py

Containers:  wbits = -15 (raw DEFLATE) / +15 (zlib) / 31 (gzip)
Levels:      0, 1, 6, 9

The JSON records, per vector:
    name, kind (positive/negative), container, level,
    data_len / data_sha256   - the original (uncompressed) bytes
    compressed_len / payload - the stream handed to the module, as hex
    data_hex                 - the original bytes as hex, for small inputs only
    expect                   - positive: "ok";  negative: the reason it must fail
"""

import hashlib
import json
import os
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vectors.json")

# The original bytes are echoed verbatim (as hex) only when they are small
# enough to keep vectors.json readable; bigger ones are pinned by length +
# SHA-256 instead.  Either way every positive vector is checked byte-for-byte.
DATA_HEX_LIMIT = 4096

CONTAINERS = {"deflate": -15, "zlib": 15, "gzip": 31}
LEVELS = [0, 1, 6, 9]


# ─── deterministic input corpus ───────────────────────────────────────────────

def lcg_bytes(n, seed):
    """Deterministic pseudo-random bytes (Park-Miller LCG)."""
    out = bytearray(n)
    st = seed
    for i in range(n):
        st = (st * 1103515245 + 12345) % 2147483648
        out[i] = (st >> 16) & 0xFF
    return bytes(out)


def mixed_bytes(n):
    """Half text / half incompressible noise, with a repetitive tail."""
    out = bytearray()
    words = [b"the quick brown fox ", b"junk calculator 2 ", b"deflate container ",
             b"0123456789abcdef ", b"aaaaaaaabbbbbbbb "]
    i = 0
    while len(out) < n // 2:
        out += words[i % len(words)]
        i += 1
    out += lcg_bytes(n // 2, 20260101)
    while len(out) < n:
        out += b"\x00\xff\x7f\x80"
    return bytes(out[:n])


INPUTS = [
    ("empty",     b""),
    ("one_byte",  b"A"),
    ("rand_1k",   lcg_bytes(1024, 987654321)),
    ("mixed_100k", mixed_bytes(102400)),
    ("same_100k", b"\xa5" * 102400),
]

# Fixed DEFLATE stream that the *hand-written* encoder in
# src/utils/deflate/Deflate.cpp produces for the ASCII text "jc2 deflate core"
# (stored block: BFINAL=1, BTYPE=00, 5 pad bits, LEN=16, NLEN=~LEN, then the
# bytes).  Kept here so at least one vector exercises the btype=0 path with a
# stream that no zlib version can silently "fix" for us; byte-for-byte this is
# what zlib itself emits for a level-0 raw stream of the same input.
HARDCODED = [
    ("stored_literal_16",
     "jc2 deflate core",
     "deflate",
     0,
     "011000efff6a6332206465666c61746520636f7265"),
]


def compress(data, container, level):
    co = zlib.compressobj(level, zlib.DEFLATED, CONTAINERS[container])
    return co.compress(data) + co.flush()


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


# ─── vector assembly ─────────────────────────────────────────────────────────

def positive(name, data, container, level, payload):
    assert zlib.decompress(payload, CONTAINERS[container]) == data, name
    v = {
        "name": name,
        "kind": "positive",
        "container": container,
        "level": level,
        "data_len": len(data),
        "data_sha256": sha256_hex(data),
        "compressed_len": len(payload),
        "payload": payload.hex(),
        "expect": "ok",
    }
    if len(data) <= DATA_HEX_LIMIT:
        v["data_hex"] = data.hex()
    return v


def negative(name, container, payload, reason):
    return {
        "name": name,
        "kind": "negative",
        "container": container,
        "level": -1,
        "compressed_len": len(payload),
        "payload": payload.hex(),
        "expect": reason,
    }


def build():
    vectors = []

    # 1. container x level x corpus (empty input included: level 0 still emits a
    #    stored block, levels 1/6/9 emit a fixed-Huffman empty block, so the
    #    empty case is not degenerate in the compressed form).
    for cname in CONTAINERS:
        for level in LEVELS:
            for iname, data in INPUTS:
                payload = compress(data, cname, level)
                vectors.append(positive(
                    "%s_%s_l%d" % (iname, cname, level), data, cname, level, payload))

    # 2. the hand-written encoder's own stored-block output
    for name, text, cname, level, payload_hex in HARDCODED:
        vectors.append(positive(name, text.encode("ascii"), cname, level,
                                bytes.fromhex(payload_hex)))

    # 3. gzip header flag coverage: FTEXT, FHCRC, FEXTRA, FNAME, FCOMMENT.
    #    Python's gzip module writes FNAME when given a filename; FEXTRA/FCOMMENT/
    #    FHCRC are hand-assembled around a real deflate body.
    body = zlib.compressobj(6, zlib.DEFLATED, -15)
    raw = body.compress(b"gzip header flags") + body.flush()
    trailer = (zlib.crc32(b"gzip header flags") & 0xFFFFFFFF).to_bytes(4, "little")
    trailer += (17).to_bytes(4, "little")

    def gz(flags, extra=b"", name=b"", comment=b""):
        head = bytes([0x1F, 0x8B, 8, flags, 0, 0, 0, 0, 0, 0xFF])
        if flags & 4:
            head += len(extra).to_bytes(2, "little") + extra
        if flags & 8:
            head += name + b"\x00"
        if flags & 16:
            head += comment + b"\x00"
        if flags & 2:
            head += (zlib.crc32(head) & 0xFFFF).to_bytes(2, "little")
        return head + raw + trailer

    vectors.append(positive("gz_flag_ftext", b"gzip header flags", "gzip", 6, gz(1)))
    vectors.append(positive("gz_flag_fhcrc", b"gzip header flags", "gzip", 6, gz(2)))
    vectors.append(positive("gz_flag_fextra", b"gzip header flags", "gzip", 6,
                            gz(4, extra=b"\x01\x02\x00\x03ABC")))
    vectors.append(positive("gz_flag_fname", b"gzip header flags", "gzip", 6,
                            gz(8, name=b"orig.txt")))
    vectors.append(positive("gz_flag_fcomment", b"gzip header flags", "gzip", 6,
                            gz(16, comment=b"a comment")))
    vectors.append(positive("gz_flag_all", b"gzip header flags", "gzip", 6,
                            gz(1 | 2 | 4 | 8 | 16, extra=b"\x00\x00",
                               name=b"n.bin", comment=b"c")))
    # FLG bits 5/6/7 are reserved and must be rejected (not silently ignored)
    vectors.append(negative("gz_bad_reserved_flag", "gzip", gz(0x20), "header"))

    # Concatenated gzip members (RFC 1952 §2.2).  zlib's decompress() stops after
    # the first member, and so does this module, which makes the vector ambiguous
    # unless the expectation is pinned by a test that names the behaviour.
    two = compress(b"abc", "gzip", 6) * 2
    vectors.append(positive("gz_two_members_first_only", b"abc", "gzip", 6, two))

    # 4. negative: truncation, checksum damage, trailing garbage, bad magic/header.
    base_gz = compress(b"the payload that will be damaged", "gzip", 6)
    base_zl = compress(b"the payload that will be damaged", "zlib", 6)
    n = len(b"the payload that will be damaged")

    vectors.append(negative("gz_truncated_1", "gzip", base_gz[:-1], "truncated"))
    vectors.append(negative("gz_truncated_5", "gzip", base_gz[:-5], "truncated"))
    crc_bad = bytearray(base_gz)
    crc_bad[len(crc_bad) - 8] ^= 0x01                 # CRC32 low byte
    vectors.append(negative("gz_crc_flipped", "gzip", bytes(crc_bad), "checksum"))
    isize_bad = bytearray(base_gz)
    isize_bad[len(isize_bad) - 1] ^= 0x01             # ISIZE high byte
    vectors.append(negative("gz_isize_flipped", "gzip", bytes(isize_bad), "checksum"))
    vectors.append(negative("gz_trailing_garbage", "gzip", base_gz + b"junk!",
                            "checksum"))
    magic_bad = bytearray(base_gz)
    magic_bad[0] = 0x1E
    vectors.append(negative("gz_bad_magic", "gzip", bytes(magic_bad), "header"))
    cm_bad = bytearray(base_gz)
    cm_bad[2] = 7                                     # CM must be 8
    vectors.append(negative("gz_bad_cm", "gzip", bytes(cm_bad), "header"))
    vectors.append(negative("gz_empty", "gzip", b"", "truncated"))
    vectors.append(negative("gz_header_only", "gzip", base_gz[:10], "truncated"))

    zl_hdr = bytearray(base_zl)
    zl_hdr[1] ^= 0x01                                 # breaks (CMF<<8|FLG) % 31 == 0
    vectors.append(negative("zl_bad_header_check", "zlib", bytes(zl_hdr), "header"))
    zl_cm = bytearray(base_zl)
    zl_cm[0] = 0x77                                   # CM must be 8
    vectors.append(negative("zl_bad_cm", "zlib", bytes(zl_cm), "header"))
    vectors.append(negative("zl_truncated_1", "zlib", base_zl[:-1], "truncated"))
    vectors.append(negative("zl_truncated_5", "zlib", base_zl[:-5], "truncated"))
    zl_adler = bytearray(base_zl)
    zl_adler[len(zl_adler) - 1] ^= 0x01               # Adler-32 high byte
    vectors.append(negative("zl_adler_flipped", "zlib", bytes(zl_adler), "checksum"))
    vectors.append(negative("zl_trailing_garbage", "zlib", base_zl + b"junk!",
                            "checksum"))
    vectors.append(negative("zl_empty", "zlib", b"", "truncated"))
    vectors.append(negative("zl_header_only", "zlib", base_zl[:2], "truncated"))

    raw = compress(b"the payload that will be damaged", "deflate", 6)
    vectors.append(negative("raw_truncated_1", "deflate", raw[:-1], "deflate"))
    vectors.append(negative("deflate_empty", "deflate", b"", "truncated"))
    # 0x03 = BFINAL=1, BTYPE=01 (fixed Huffman), 0x00 = end-of-block symbol:
    # a *valid* empty deflate stream that decodes to zero bytes, while every
    # zlib-produced stream for an empty input is 3 or more bytes.
    vectors.append(positive("deflate_fixed_empty", b"", "deflate", 1, b"\x03\x00"))
    return vectors


def main():
    vectors = build()

    def counts(key):
        out = {}
        for v in vectors:
            k = v[key]
            out[k] = out.get(k, 0) + 1
        return out

    meta = {
        "generator": "data/codec_vectors/generate.py",
        "produced_by": "CPython zlib %s (zlib %s)" % (os.sys.version.split()[0],
                                                      zlib.ZLIB_VERSION),
        "containers": CONTAINERS,
        "levels": LEVELS,
        "total": len(vectors),
        "positive": sum(1 for v in vectors if v["kind"] == "positive"),
        "negative": sum(1 for v in vectors if v["kind"] == "negative"),
        "by_container": counts("container"),
        "by_level": counts("level"),
    }

    doc = {"meta": meta, "vectors": vectors}
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    print("wrote %s: %d vectors (%d positive / %d negative)"
          % (OUT, meta["total"], meta["positive"], meta["negative"]))
    print("by container:", meta["by_container"])
    print("by level:    ", meta["by_level"])


if __name__ == "__main__":
    main()
