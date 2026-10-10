#!/usr/bin/env python3
"""Generate the *constructed* HTTP response fixtures used by tests/lib/test_http.jc2.

The recorded fixtures (chunked_example_org.http, content_length_apple.http, ...) are
real captures; the files written by this script are hand-built byte streams that pin
down the framing rules a real capture cannot isolate:

    two_responses_cl.http         第一响应 CL 收尾，后面紧跟第二响应的起始字节
    two_responses_chunked.http    第一响应 chunked 收尾（trailer 之后）紧跟第二响应
    no_body_304_then_200.http     304 无正文，后面紧跟一个完整 200
    head_with_content_length.http HEAD 语义：有 CL 头但一个正文字节都没有
    chunked_gzip.http             gzip 正文 + chunked 传输（块边界故意切在 gzip 头/流中间）
    chunked_empty_body.http       0 块 + trailer 结束
    deflate_zlib.http             Content-Encoding: deflate，zlib 容器（RFC 1950）
    deflate_raw.http              Content-Encoding: deflate，裸 DEFLATE（RFC 1951）
    bad_gzip.http                 gzip 容器被破坏 → 必须报 HttpError
    truncated_chunked.http        块数据中途截断 → 必须报 HttpError

压缩数据全部由 CPython 的 zlib/gzip 生成（参考实现），JC2 侧只做解压，
所以这是一份独立实现，而不是自己编码自己解码。脚本可反复运行：写出的字节完全一致。

Run:
    python data/http_vectors/generate_http_vectors.py           # 重新生成夹具 + 同步 manifest_extra.json
    python data/http_vectors/generate_http_vectors.py --check    # 只校验 manifest 里的 sha256 / 字节数
"""

import gzip
import hashlib
import json
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFESTS = ("manifest.json", "manifest_extra.json")
EXTRA = os.path.join(HERE, "manifest_extra.json")

# 固定载荷：ASCII + 多字节（解码后必须逐字节等于这个字符串）
PAYLOAD = "JC2 http gzip+chunked \u2713 \u8f7d\u8377 \u4e2d\u6587\u5b57\u7b26 " + "0123456789" * 12
PAYLOAD_BYTES = PAYLOAD.encode("utf-8")


def header(status, fields, version="HTTP/1.1"):
    """Build a response head; `status` is the full status line tail (e.g. "200 OK")."""
    lines = [f"{version} {status}"]
    lines += [f"{name}: {value}" for name, value in fields]
    return ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")


def chunk(data):
    return f"{len(data):x}\r\n".encode("ascii") + data + b"\r\n"


def chunked_body(data, cut_points):
    """Split `data` into chunks at the given byte offsets (plus the tail)."""
    pieces = []
    prev = 0
    for off in cut_points:
        if prev < off < len(data):
            pieces.append(data[prev:off])
            prev = off
    pieces.append(data[prev:])
    return b"".join(chunk(p) for p in pieces) + b"0\r\n\r\n"


def gzip_bytes(data):
    # mtime=0 keeps the stream byte-identical across runs (no timestamp in the header)
    return gzip.compress(data, compresslevel=9, mtime=0)


def raw_deflate_bytes(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


def build():
    """Return {filename: {"data": bytes, ...metadata}}."""
    out = {}

    def add(name, data, **meta):
        meta.setdefault("source", "(synthetic)")
        meta.setdefault("constructed_for_test", True)
        meta["data"] = data
        out[name] = meta

    # 1. Content-Length 边界：第一条响应之后紧跟第二条响应的开头
    add(
        "two_responses_cl.http",
        header("200 OK", [
            ("Date", "Mon, 01 Jan 2024 00:00:00 GMT"),
            ("Server", "jc2-vector"),
            ("Content-Type", "text/plain"),
            ("Content-Length", "10"),
        ]) + b"0123456789"
        + header("200 OK", [("Content-Type", "text/plain"), ("Content-Length", "5")]) + b"HELLO",
        status=200, transfer_encoding_chunked=False, content_length=10,
        expect_body="0123456789", expect_error=None,
        note="手工构造：CL=10 的第一响应之后紧跟第二响应（CL=5），用来钉死“正文按 CL 截止、绝不多吃一个字节”",
    )

    # 2. chunked 边界：trailer 结束之后紧跟第二条响应
    add(
        "two_responses_chunked.http",
        header("200 OK", [("Content-Type", "text/plain"), ("Transfer-Encoding", "chunked")])
        + b"5\r\nhello\r\n" + b"6\r\n world\r\n" + b"0\r\n" + b"\r\n"
        + header("204 No Content", []),
        status=200, transfer_encoding_chunked=True, content_length=None,
        expect_body="hello world", expect_error=None,
        note="手工构造：chunked 第一响应（0 块 + trailer 空行）之后紧跟 204，用来钉死“分块正文到 trailer 结束”",
    )

    # 3. 304 无正文：后面的 200 不能被当成 304 的正文
    add(
        "no_body_304_then_200.http",
        header("304 Not Modified", [("Date", "Mon, 01 Jan 2024 00:00:00 GMT"), ("ETag", '"abc"')])
        + header("200 OK", [("Content-Length", "3")]) + b"xyz",
        status=304, transfer_encoding_chunked=False, content_length=None,
        expect_body="", expect_error=None,
        note="手工构造：304 无正文，之后紧跟一个完整 200；读到对端关闭的实现会把 200 当成 304 的正文",
    )

    # 4. HEAD：有 Content-Length 但没有正文
    add(
        "head_with_content_length.http",
        header("200 OK", [
            ("Date", "Mon, 01 Jan 2024 00:00:00 GMT"),
            ("Content-Type", "text/html; charset=UTF-8"),
            ("Content-Length", "1256"),
        ]),
        status=200, transfer_encoding_chunked=False, content_length=1256,
        expect_body="", expect_error=None,
        note="手工构造：HEAD 语义 —— 头部有 Content-Length: 1256 但正文为零字节（只有方法为 HEAD 时才能解析）",
    )

    # 5. chunked + gzip：块边界故意切在 gzip 头（1、7）与 deflate 流中间（33）
    gz = gzip_bytes(PAYLOAD_BYTES)
    add(
        "chunked_gzip.http",
        header("200 OK", [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Encoding", "gzip"),
            ("Transfer-Encoding", "chunked"),
        ]) + chunked_body(gz, [1, 7, 33, len(gz) - 5]),
        status=200, transfer_encoding_chunked=True, content_length=None,
        content_encoding="gzip",
        expect_body=PAYLOAD, expect_error=None,
        note="手工构造：gzip 压缩的固定载荷按 chunked 传输，块边界切在 gzip 头（1、7）与 deflate 流中间（33）",
    )

    # 6. 0 块 + trailer
    add(
        "chunked_empty_body.http",
        header("200 OK", [("Transfer-Encoding", "chunked")]) + b"0\r\nX-Trailer: done\r\n\r\n",
        status=200, transfer_encoding_chunked=True, content_length=None,
        expect_body="", expect_error=None,
        note="手工构造：只有一个 0 块加一个 trailer 字段，正文为空字符串",
    )

    # 7. deflate 的两种现实容器
    zlib_stream = zlib.compress(PAYLOAD_BYTES, 9)
    add(
        "deflate_zlib.http",
        header("200 OK", [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Encoding", "deflate"),
            ("Content-Length", str(len(zlib_stream))),
        ]) + zlib_stream,
        status=200, transfer_encoding_chunked=False, content_length=len(zlib_stream),
        content_encoding="deflate",
        expect_body=PAYLOAD, expect_error=None,
        note="手工构造：Content-Encoding: deflate 的 zlib 容器（RFC 1950，2 字节头 + Adler-32）",
    )
    raw_stream = raw_deflate_bytes(PAYLOAD_BYTES)
    add(
        "deflate_raw.http",
        header("200 OK", [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Encoding", "deflate"),
            ("Content-Length", str(len(raw_stream))),
        ]) + raw_stream,
        status=200, transfer_encoding_chunked=False, content_length=len(raw_stream),
        content_encoding="deflate",
        expect_body=PAYLOAD, expect_error=None,
        note="手工构造：Content-Encoding: deflate 的裸 DEFLATE（RFC 1951，无容器头无校验和）",
    )

    # 8. 坏 gzip：CRC32 置零 + deflate 中途翻一位
    bad = bytearray(gz)
    bad[20] ^= 0xFF
    bad[-8:-4] = b"\x00\x00\x00\x00"
    bad = bytes(bad)
    add(
        "bad_gzip.http",
        header("200 OK", [("Content-Encoding", "gzip"), ("Content-Length", str(len(bad)))]) + bad,
        status=200, transfer_encoding_chunked=False, content_length=len(bad),
        content_encoding="gzip",
        expect_body=None, expect_error="HttpError",
        note="手工构造：gzip 流被破坏（deflate 中途翻位 + CRC32 置零）→ 必须报 HttpError",
    )

    # 9. 分块中途截断：声明 16 字节，只给 5 字节然后断流
    add(
        "truncated_chunked.http",
        header("200 OK", [("Transfer-Encoding", "chunked")]) + b"10\r\nabcde",
        status=200, transfer_encoding_chunked=True, content_length=None,
        expect_body=None, expect_error="HttpError",
        note="手工构造：块长声明 16 字节、实际只到 5 字节就断流 → 必须报 HttpError",
    )

    return out


def write_fixtures(fixtures):
    for name in sorted(fixtures):
        with open(os.path.join(HERE, name), "wb") as fh:
            fh.write(fixtures[name]["data"])


def extra_entries(fixtures):
    """Manifest entries, in the same field style as the existing manifest_extra.json."""
    entries = []
    for name in sorted(fixtures):
        meta = fixtures[name]
        data = meta["data"]
        entry = {
            "file": name,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "source": meta["source"],
            "status": meta.get("status"),
            "constructed_for_test": True,
            "note": meta.get("note", ""),
            "transfer_encoding_chunked": meta.get("transfer_encoding_chunked"),
            "content_length": meta.get("content_length"),
        }
        if "content_encoding" in meta:
            entry["content_encoding"] = meta["content_encoding"]
        entry["expect_body"] = meta.get("expect_body")
        entry["expect_error"] = meta.get("expect_error")
        entries.append(entry)
    return entries


def sync_manifest(entries):
    """Merge the generated entries into manifest_extra.json, keeping the hand-written ones."""
    existing = []
    if os.path.exists(EXTRA):
        with open(EXTRA, "r", encoding="utf-8") as fh:
            loaded = json.load(fh)
        existing = loaded["entries"] if isinstance(loaded, dict) else loaded
    generated = {e["file"] for e in entries}
    merged = [e for e in existing if e.get("file") not in generated] + entries
    merged.sort(key=lambda e: e["file"])
    text = json.dumps(merged, indent=2, ensure_ascii=False) + "\n"
    with open(EXTRA, "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write(text)
    return len(merged)


def check():
    """Recompute sha256 for every file listed in the manifests."""
    bad = 0
    for manifest in MANIFESTS:
        path = os.path.join(HERE, manifest)
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8") as fh:
            payload = json.load(fh)
        entries = payload["entries"] if isinstance(payload, dict) else payload
        for entry in entries:
            target = os.path.join(HERE, entry["file"])
            if not os.path.exists(target):
                print(f"MISSING  {entry['file']}")
                bad += 1
                continue
            with open(target, "rb") as fh:
                data = fh.read()
            digest = hashlib.sha256(data).hexdigest()
            if digest != entry["sha256"] or len(data) != entry["bytes"]:
                print(f"MISMATCH {entry['file']}: {len(data)} bytes / {digest}")
                bad += 1
    print(f"checked manifests: {bad} problem(s)")
    return 1 if bad else 0


def main():
    if "--check" in sys.argv:
        return check()

    fixtures = build()
    write_fixtures(fixtures)
    total = sync_manifest(extra_entries(fixtures))
    print(f"wrote {len(fixtures)} fixture(s) into {HERE}")
    for name in sorted(fixtures):
        data = fixtures[name]["data"]
        print(f"  {name:<30} {len(data):>5} bytes  sha256={hashlib.sha256(data).hexdigest()[:16]}...")
    print(f"manifest_extra.json now lists {total} fixture(s)")
    print("payload (utf-8, {0} bytes): {1}".format(
        len(PAYLOAD_BYTES), PAYLOAD.encode("unicode_escape").decode("ascii")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
