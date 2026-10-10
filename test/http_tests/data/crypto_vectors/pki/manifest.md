# PKI / big-integer official-vector fixtures — manifest

Companion to `data/crypto_vectors/x509/manifest.md`, generated the same way (`openssl ca -startdate/-enddate`, fixed validity windows, private keys in a sibling `keys/` directory).

- OpenSSL: `OpenSSL 3.2.1 30 Jan 2024 (Library: OpenSSL 3.2.1 30 Jan 2024)`
- Python used for extraction / cross-checks: `3.12.14 (main, Sep  1 2026, 14:17:39) [MSC v.1944 64 bit (AMD64)]`
- **Only new files were created.** Nothing under `CMakeLists.txt`, `src/`, `modules/` or `tests/` was touched; everything lives under `data/crypto_vectors/pki/`.
- All twelve certificates are issued with explicit `-startdate 200101000000Z` / `-enddate 350101000000Z`, i.e. **2020-01-01T00:00:00Z → 2035-01-01T00:00:00Z**. No validity date is relative to the wall clock, so no result below depends on when it is re-run.
- Every certificate carries a critical `basicConstraints`, a critical `keyUsage` and a `subjectAltName` with DNS names — never CN-only.
- Reproduce with `work/make_chains.py`, re-verify with `work/verify_chains.py`, rebuild this file with `work/make_manifest.py`.

## 1. Files and hashes

| 文件 | 字节数 | sha256 |
|---|---:|---|
| `p384_root.der` | 554 | `425d5309cfb76c442a77a75fdec190b146d753e082d6600707f92c41aa252cd2` |
| `p384_inter.der` | 564 | `a9781e1425ff89e763b3fc267c836db2d56d54e5ef310878ec737784799f7f1b` |
| `p384_leaf.der` | 581 | `a179b7184f90b76231bd066a410dbfb70ba83676af438ad50a71312975c92c39` |
| `p521_root.der` | 628 | `651746901334c412b8f81f66cd1903ebba3e5b05f582a98a6536007ba63e1ca9` |
| `p521_inter.der` | 636 | `06d414496c9a68681c3363d4bd00b03857e65ef66e6c114f0555984b4b3a767e` |
| `p521_leaf.der` | 616 | `8352d9755eab62d5417781896b4b0a78eb592558780a6bfa5eefbd93b1db60b9` |
| `rsa384_root.der` | 894 | `3f0926496f514adde67a409a6223c83839c89b5e233ca9e58f376d0ee5837b10` |
| `rsa384_inter.der` | 903 | `a98a38d534cf7cb27f75e6a85e362ebf0599c4fe18f1c2519ec5d1bb7c1accd5` |
| `rsa384_leaf.der` | 921 | `00e35130ebcb6979c72f92b1d85c22d4aaa0f7ea3a90be5721aa34313bc0b66e` |
| `rsa512_root.der` | 894 | `3825f3a6f8548daba0af8c890acdd65460a67532f27f9086d459bdbcb1fc9934` |
| `rsa512_inter.der` | 903 | `b6dcf944f545f1e56b31f65200783a68b1f4d9a5bf1094f35221bcee73809c2b` |
| `rsa512_leaf.der` | 922 | `382a21a7029d65d5016504a4dbe11ecaa9c63d3c167dd43ef96cfd4468e34864` |
| `sha2_vectors.json` | 10586 | `25737eadcfecc9c447f5f1b638705717785bf845f384c92a2a276dc03d3e532f` |
| `ecdsa_p384_p521.json` | 23249 | `7c89b69e9ebad135495ae1b80e2e33fecf9b4666d4313e8352dda35b682f78cf` |

The byte counts above are read from disk by `work/make_manifest.py` at the moment this file is written; `work/verify_chains.py` re-reads them independently and asserts the same values.

## 2. `sha2_vectors.json`

| array | 条数 | 说明 |
|---|---:|---|
| `sha384` | 9 | FIPS 180-4 SHA-384, all positive |
| `sha512` | 9 | FIPS 180-4 SHA-512, all positive |

There are no negative (must-fail) vectors in this file — a hash has no reject case, so every record is a positive vector.

| name | msg bytes | sha384 digest (first 32 hex) | sha512 digest (first 32 hex) |
|---|---:|---|---|
| `empty` | 0 | `38b060a751ac96384cd9327eb1b1e36a…` | `cf83e1357eefb8bdf1542850d66d8007…` |
| `abc` | 3 | `cb00753f45a35e8bb5a03d699ac65007…` | `ddaf35a193617abacc417349ae204131…` |
| `fips180_2block_112B` | 112 | `09330c33f71147e83d192fc782cd1b47…` | `8e959b75dae313da8cf4f72814fc143f…` |
| `long_128B_00_7f` | 128 | `ca2385773319124534111a36d0581fc3…` | `1dffd5e3adb71d45d2245939665521ae…` |
| `a_repeat_111B` | 111 | `3c37955051cb5c3026f94d551d5b5e2a…` | `fa9121c7b32b9e01733d034cfc78cbf6…` |
| `a_repeat_112B` | 112 | `187d4e07cb306103c69967bf544d0dfb…` | `c01d080efd492776a1c43bd23dd99d0a…` |
| `a_repeat_127B` | 127 | `9bd06b1763c2cf7aef40e795dc65bc96…` | `828613968b501dc00a97e08c73b118aa…` |
| `a_repeat_128B` | 128 | `edb12730a366098b3b2beac75a3bef1b…` | `b73d1929aa615934e61a871596b3f3b3…` |
| `a_repeat_129B` | 129 | `39b6f5a7b0e781dbc419f72e49b30eaa…` | `4f681e0bd53cda4b5a2041cc8a06f2ea…` |

Message construction, so the vectors are reproducible without shipping a source file:

- `empty` (0 B): zero-length message (CAVP SHA*ShortMsg/LongMsg style edge case)
- `abc` (3 B): FIPS 180-4 Appendix / FIPS 180-2 worked example 1
- `fips180_2block_112B` (112 B): FIPS 180-2 SHA-512 example 2: 112 ASCII bytes 'a'..'u' in a bcdefgh repeating pattern; 112 bytes is the largest length that still fits padding in a single 128-byte block minus one, i.e. the first length needing a 2nd block
- `long_128B_00_7f` (128 B): 128-byte message = every byte value 0x00..0x7F exactly once
- `a_repeat_111B` (111 B): 111 bytes of 0x61 ('a' * 111)
- `a_repeat_112B` (112 B): 112 bytes of 0x61 ('a' * 112)
- `a_repeat_127B` (127 B): 127 bytes of 0x61 ('a' * 127)
- `a_repeat_128B` (128 B): 128 bytes of 0x61 ('a' * 128)
- `a_repeat_129B` (129 B): 129 bytes of 0x61 ('a' * 129)

Boundary rationale (see `notes` in the JSON): SHA-512 hashes 128-byte blocks and pads with `0x80 || zeros || 16-byte length`, so a message of L bytes fits in one block iff `L + 1 + 16 <= 128`, i.e. `L <= 111`. 111 → 1 block, 112 → 2 blocks, 127 → 2 blocks, 128 → 3 blocks, 129 → 3 blocks. This is the SHA-512 analogue of SHA-256's 55/56 boundary.

**Cross-check result: every digest matches `hashlib`.** Each `msg_hex` was decoded back to bytes and re-hashed with Python's `hashlib` (CPython 3.12.14) — 9/9 SHA-384 and 9/9 SHA-512 reproduced exactly. As a second, independent implementation every message was also fed to `openssl dgst -sha384` / `-sha512`, which agreed on all 18 records.

## 3. `ecdsa_p384_p521.json`

| array | 条数 | Result=P | Result=F | 来源 |
|---|---:|---:|---:|---|
| `p384_sha384` | 5 | 3 | 2 | SigVer.rsp `[P-384,SHA-384]` |
| `p521_sha512` | 5 | 3 | 2 | SigVer.rsp `[P-521,SHA-512]` |
| `p384_sha384_extra_valid` | 1 | 1 | 0 | SigGen.rsp `[P-384,SHA-384]` (supplement) |
| `ecdh_p384` | 2 | 2 | 0 | KAS_ECC_CDH_PrimitiveTest.txt `[P-384]` |
| `ecdh_p521` | 2 | 2 | 0 | KAS_ECC_CDH_PrimitiveTest.txt `[P-521]` |

### 3.1 Field widths (measured, not assumed)

| array | qx / qy hex width | r / s hex width |
|---|---|---|
| `p384_sha384` | [96] (all records) | r [96], s [96] |
| `p521_sha512` | [132] (all records) | r [131], s [131] |
| `p384_sha384_extra_valid` | [96] (all records) | r [96], s [96] |

`qx`/`qy` are exactly **96 hex chars (48 bytes)** for P-384 and **132 hex chars (66 bytes)** for P-521 in every emitted record, asserted at build time — this is the width the task required.

### 3.2 Cross-check against OpenSSL

`work/verify_vectors.py` rebuilds each key from `qx`/`qy`, each signature from `r`/`s`, and each ECDH key pair from `priv`/`peer_x`/`peer_y`, then asks OpenSSL — a completely independent implementation from the JSON author:

- every `Result=P` record (3 + 3 + 1) reports `Verified OK`
- every `Result=F` record (2 + 2) reports `Verification Failure`
- every ECDH `dIUT * QCAVS` reproduces `shared` byte for byte (48 bytes for P-384, 66 bytes for P-521)

Full output: `work/verify_report.txt` is the certificate report; the vector cross-check prints the same tables shown in section 3.3 below.

### 3.3 Known CAVP quirks a consumer must handle

1. **`msg_hex` is the raw message, not a digest.** Measured `Msg` length is 128 bytes in both sections (neither 48 nor 64), so the implementation hashes it: SHA-384 for `p384_sha384`, SHA-512 for `p521_sha512`. `openssl dgst -sha384 -verify` over the raw bytes confirms this — it verifies the P records, which it would not do under the digest reading.
2. **P-521 `r`/`s` are 131 hex characters — an odd number.** A P-521 scalar is below 2^521, so it needs at most 131 hex digits, and CAVP does not pad. `bytes.fromhex(r)` therefore fails on every P-521 record even though `int(r, 16)` is fine. Each record additionally carries `r_padded` / `s_padded`, left-zero-padded to the full field width (96 / 132 hex chars), which are safe to `bytes.fromhex()` directly. `r` and `s` themselves are the untouched CAVP text.
3. **The ECC CDH file over-pads P-521 to 68 bytes.** In `KAS_ECC_CDH_PrimitiveTest.txt` the `[P-521]` fields `QCAVSx`, `QCAVSy`, `dIUT`, `QIUTx`, `QIUTy` are all exactly **136 hex chars = 68 bytes** — two bytes wider than the 66-byte field — while `ZIUT` is 132 hex chars = 66 bytes. The `[P-384]` section is uniform at 96 hex chars. `peer_x`/`peer_y` were therefore stripped of leading zeros and re-padded to 132 hex chars so the promised 132-char width holds; no significant digit was lost.

## 4. Certificate chains

### 4.1 Per-certificate table

| 文件 | 字节数 | 角色 | subject | issuer | SAN | notBefore | notAfter | 公钥类型与位数 | 签名算法 | 期望校验结果 |
|---|---:|---|---|---|---|---|---|---|---|---|
| `p384_root.der` | 554 | root | CN=JC2 P384 Root | CN=JC2 P384 Root | DNS:p384-root.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | EC P-384 (secp384r1), 384-bit | ecdsa-with-SHA384 | trust anchor (self-signed) |
| `p384_inter.der` | 564 | intermediate | CN=JC2 P384 Intermediate | CN=JC2 P384 Root | DNS:p384-inter.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | EC P-384 (secp384r1), 384-bit | ecdsa-with-SHA384 | valid |
| `p384_leaf.der` | 581 | leaf | CN=p384.example.com | CN=JC2 P384 Intermediate | DNS:p384.example.com | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | EC P-384 (secp384r1), 384-bit | ecdsa-with-SHA384 | valid |
| `p521_root.der` | 628 | root | CN=JC2 P521 Root | CN=JC2 P521 Root | DNS:p521-root.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | EC P-521 (secp521r1), 521-bit | ecdsa-with-SHA384 | trust anchor (self-signed) |
| `p521_inter.der` | 636 | intermediate | CN=JC2 P521 Intermediate | CN=JC2 P521 Root | DNS:p521-inter.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | EC P-521 (secp521r1), 521-bit | ecdsa-with-SHA384 | valid |
| `p521_leaf.der` | 616 | leaf | CN=p521.example.com | CN=JC2 P521 Intermediate | DNS:p521.example.com | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | EC P-384 (secp384r1), 384-bit | ecdsa-with-SHA384 | valid |
| `rsa384_root.der` | 894 | root | CN=JC2 RSA384 Root | CN=JC2 RSA384 Root | DNS:rsa384-root.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | RSA, 2048-bit | sha384WithRSAEncryption | trust anchor (self-signed) |
| `rsa384_inter.der` | 903 | intermediate | CN=JC2 RSA384 Intermediate | CN=JC2 RSA384 Root | DNS:rsa384-inter.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | RSA, 2048-bit | sha384WithRSAEncryption | valid |
| `rsa384_leaf.der` | 921 | leaf | CN=rsa384.example.com | CN=JC2 RSA384 Intermediate | DNS:rsa384.example.com | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | RSA, 2048-bit | sha384WithRSAEncryption | valid |
| `rsa512_root.der` | 894 | root | CN=JC2 RSA512 Root | CN=JC2 RSA512 Root | DNS:rsa512-root.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | RSA, 2048-bit | sha512WithRSAEncryption | trust anchor (self-signed) |
| `rsa512_inter.der` | 903 | intermediate | CN=JC2 RSA512 Intermediate | CN=JC2 RSA512 Root | DNS:rsa512-inter.jc2.test | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | RSA, 2048-bit | sha512WithRSAEncryption | valid |
| `rsa512_leaf.der` | 922 | leaf | CN=rsa512.example.com | CN=JC2 RSA512 Intermediate | DNS:rsa512.example.com | Jan  1 00:00:00 2020 GMT | Jan  1 00:00:00 2035 GMT | RSA, 2048-bit | sha512WithRSAEncryption | valid |

`O=JC2 PKI Vector Fixtures` is prefixed to every subject and stripped above for width. `notBefore`/`notAfter` are `Jan  1 00:00:00 2020 GMT` and `Jan  1 00:00:00 2035 GMT` for all twelve certificates.

Full extension detail as read back from each file:

| 文件 | serial | basicConstraints | keyUsage | extKeyUsage | SKI | AKI |
|---|---|---|---|---|---|---|
| `p384_root.der` | 1000 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `10:7F:C0:5F:B1:C5:B6:7D…` | `10:7F:C0:5F:B1:C5:B6:7D…` |
| `p384_inter.der` | 1001 | CA:TRUE, pathlen:0 | Certificate Sign, CRL Sign | No extensions in certificate | `E5:2F:00:61:29:8E:92:2F…` | `10:7F:C0:5F:B1:C5:B6:7D…` |
| `p384_leaf.der` | 2000 | CA:FALSE | Digital Signature | TLS Web Server Authentication | `2C:D8:B8:ED:4B:BD:E9:7D…` | `E5:2F:00:61:29:8E:92:2F…` |
| `p521_root.der` | 3000 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `05:B7:E7:98:22:A3:B3:89…` | `05:B7:E7:98:22:A3:B3:89…` |
| `p521_inter.der` | 3001 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `70:D2:E5:46:18:04:48:F6…` | `05:B7:E7:98:22:A3:B3:89…` |
| `p521_leaf.der` | 4000 | CA:FALSE | Digital Signature | TLS Web Server Authentication | `E6:D1:C9:04:54:23:FB:DF…` | `70:D2:E5:46:18:04:48:F6…` |
| `rsa384_root.der` | 5000 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `1A:72:FA:3C:45:A5:20:B6…` | `1A:72:FA:3C:45:A5:20:B6…` |
| `rsa384_inter.der` | 5001 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `C4:E9:41:56:FE:DE:8F:8D…` | `1A:72:FA:3C:45:A5:20:B6…` |
| `rsa384_leaf.der` | 6000 | CA:FALSE | Digital Signature, Key Encipherment | TLS Web Server Authentication | `0A:D8:69:7A:49:56:FC:26…` | `C4:E9:41:56:FE:DE:8F:8D…` |
| `rsa512_root.der` | 7000 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `F5:82:F4:C5:DC:89:3F:37…` | `F5:82:F4:C5:DC:89:3F:37…` |
| `rsa512_inter.der` | 7001 | CA:TRUE | Certificate Sign, CRL Sign | No extensions in certificate | `90:7E:FD:DC:9A:05:F2:37…` | `F5:82:F4:C5:DC:89:3F:37…` |
| `rsa512_leaf.der` | 8000 | CA:FALSE | Digital Signature, Key Encipherment | TLS Web Server Authentication | `57:45:3D:41:9C:56:3B:C5…` | `90:7E:FD:DC:9A:05:F2:37…` |

SKI/AKI are truncated to the first 20 hex digits for readability; the full values are in `work/cert_facts.json`. For the four roots SKI == AKI, which is the expected self-signed shape.

### 4.2 Chains and `openssl verify` — actual output

#### Chain 1 — 纯 P-384 链 — pure P-384 (root, intermediate and leaf all P-384)

| # | 文件 | 角色 |
|---:|---|---|
| 1 | `p384_leaf.der` | leaf |
| 2 | `p384_inter.der` | intermediate |
| 3 | `p384_root.der` | root (trust anchor) |

信任根: `p384_root.der`  ·  期望: **valid**

leaf → intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p384_root.pem -untrusted D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p384_inter.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p384_leaf.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p384_leaf.der: OK

exit 0
```

intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p384_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p384_inter.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p384_inter.der: OK

exit 0
```

negative control — the same leaf **without** the intermediate (expect failure, proving the intermediate is load-bearing)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p384_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p384_leaf.der   # no -untrusted

CN=p384.example.com, O=JC2 PKI Vector Fixtures
error 20 at 0 depth lookup: unable to get local issuer certificate
error D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p384_leaf.der: verification failed

exit 2
```

#### Chain 2 — P-521 intermediate + P-384 leaf

| # | 文件 | 角色 |
|---:|---|---|
| 1 | `p521_leaf.der` | leaf |
| 2 | `p521_inter.der` | intermediate |
| 3 | `p521_root.der` | root (trust anchor) |

信任根: `p521_root.der`  ·  期望: **valid**

leaf → intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p521_root.pem -untrusted D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p521_inter.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p521_leaf.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p521_leaf.der: OK

exit 0
```

intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p521_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p521_inter.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p521_inter.der: OK

exit 0
```

negative control — the same leaf **without** the intermediate (expect failure, proving the intermediate is load-bearing)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\p521_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p521_leaf.der   # no -untrusted

CN=p521.example.com, O=JC2 PKI Vector Fixtures
error 20 at 0 depth lookup: unable to get local issuer certificate
error D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\p521_leaf.der: verification failed

exit 2
```

#### Chain 3 — SHA-384 RSA chain (sha384WithRSAEncryption, RSA-2048)

| # | 文件 | 角色 |
|---:|---|---|
| 1 | `rsa384_leaf.der` | leaf |
| 2 | `rsa384_inter.der` | intermediate |
| 3 | `rsa384_root.der` | root (trust anchor) |

信任根: `rsa384_root.der`  ·  期望: **valid**

leaf → intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa384_root.pem -untrusted D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa384_inter.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa384_leaf.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa384_leaf.der: OK

exit 0
```

intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa384_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa384_inter.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa384_inter.der: OK

exit 0
```

negative control — the same leaf **without** the intermediate (expect failure, proving the intermediate is load-bearing)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa384_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa384_leaf.der   # no -untrusted

CN=rsa384.example.com, O=JC2 PKI Vector Fixtures
error 20 at 0 depth lookup: unable to get local issuer certificate
error D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa384_leaf.der: verification failed

exit 2
```

#### Chain 4 (optional) — SHA-512 RSA chain (sha512WithRSAEncryption, RSA-2048)

| # | 文件 | 角色 |
|---:|---|---|
| 1 | `rsa512_leaf.der` | leaf |
| 2 | `rsa512_inter.der` | intermediate |
| 3 | `rsa512_root.der` | root (trust anchor) |

信任根: `rsa512_root.der`  ·  期望: **valid**

leaf → intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa512_root.pem -untrusted D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa512_inter.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa512_leaf.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa512_leaf.der: OK

exit 0
```

intermediate → root (expect OK)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa512_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa512_inter.der

D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa512_inter.der: OK

exit 0
```

negative control — the same leaf **without** the intermediate (expect failure, proving the intermediate is load-bearing)

```
$ openssl verify -CAfile D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\work\pem\rsa512_root.pem D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa512_leaf.der   # no -untrusted

CN=rsa512.example.com, O=JC2 PKI Vector Fixtures
error 20 at 0 depth lookup: unable to get local issuer certificate
error D:\VS2022\Junk Calculator 2\data\crypto_vectors\pki\rsa512_leaf.der: verification failed

exit 2
```

### 4.3 Signature algorithm — confirmed with `openssl x509 -text`

Every certificate's `-text` output was checked; the relevant lines are archived verbatim in `work/x509_text_evidence.txt`.

| 文件 | 期望签名算法 | `x509 -text` 实测 | 一致 |
|---|---|---|---|
| `p384_root.der` | `ecdsa-with-SHA384` | `ecdsa-with-SHA384` | ✅ |
| `p384_inter.der` | `ecdsa-with-SHA384` | `ecdsa-with-SHA384` | ✅ |
| `p384_leaf.der` | `ecdsa-with-SHA384` | `ecdsa-with-SHA384` | ✅ |
| `p521_root.der` | `ecdsa-with-SHA384` | `ecdsa-with-SHA384` | ✅ |
| `p521_inter.der` | `ecdsa-with-SHA384` | `ecdsa-with-SHA384` | ✅ |
| `p521_leaf.der` | `ecdsa-with-SHA384` | `ecdsa-with-SHA384` | ✅ |
| `rsa384_root.der` | `sha384WithRSAEncryption` | `sha384WithRSAEncryption` | ✅ |
| `rsa384_inter.der` | `sha384WithRSAEncryption` | `sha384WithRSAEncryption` | ✅ |
| `rsa384_leaf.der` | `sha384WithRSAEncryption` | `sha384WithRSAEncryption` | ✅ |
| `rsa512_root.der` | `sha512WithRSAEncryption` | `sha512WithRSAEncryption` | ✅ |
| `rsa512_inter.der` | `sha512WithRSAEncryption` | `sha512WithRSAEncryption` | ✅ |
| `rsa512_leaf.der` | `sha512WithRSAEncryption` | `sha512WithRSAEncryption` | ✅ |

Assertion result: **all twelve match the table**. In particular `rsa384_*.der` is `sha384WithRSAEncryption` and every `p384_*.der` / `p521_*.der` is `ecdsa-with-SHA384`.

### 4.4 Public key type and size — confirmed with `openssl x509 -text`

| 文件 | 期望公钥 | `x509 -text` 实测 | 一致 |
|---|---|---|---|
| `p384_root.der` | EC P-384 (secp384r1), 384-bit | EC secp384r1, 384-bit (id-ecPublicKey) | ✅ |
| `p384_inter.der` | EC P-384 (secp384r1), 384-bit | EC secp384r1, 384-bit (id-ecPublicKey) | ✅ |
| `p384_leaf.der` | EC P-384 (secp384r1), 384-bit | EC secp384r1, 384-bit (id-ecPublicKey) | ✅ |
| `p521_root.der` | EC P-521 (secp521r1), 521-bit | EC secp521r1, 521-bit (id-ecPublicKey) | ✅ |
| `p521_inter.der` | EC P-521 (secp521r1), 521-bit | EC secp521r1, 521-bit (id-ecPublicKey) | ✅ |
| `p521_leaf.der` | EC P-384 (secp384r1), 384-bit | EC secp384r1, 384-bit (id-ecPublicKey) | ✅ |
| `rsa384_root.der` | RSA, 2048-bit | RSA, 2048-bit (rsaEncryption) | ✅ |
| `rsa384_inter.der` | RSA, 2048-bit | RSA, 2048-bit (rsaEncryption) | ✅ |
| `rsa384_leaf.der` | RSA, 2048-bit | RSA, 2048-bit (rsaEncryption) | ✅ |
| `rsa512_root.der` | RSA, 2048-bit | RSA, 2048-bit (rsaEncryption) | ✅ |
| `rsa512_inter.der` | RSA, 2048-bit | RSA, 2048-bit (rsaEncryption) | ✅ |
| `rsa512_leaf.der` | RSA, 2048-bit | RSA, 2048-bit (rsaEncryption) | ✅ |

Assertion result: **all twelve match the table**. Note `p521_leaf.der` deliberately carries a **P-384** key under its P-521 issuer while still being signed with SHA-384 — that is the point of chain 2.

### 4.5 Byte counts

Every `.der` byte count in the table of section 4.1 was re-read from disk by `work/verify_chains.py` and matches; the same values were then re-read a third time by `work/make_manifest.py` when writing this file.

| 文件 | 字节数 |
|---|---:|
| `p384_root.der` | 554 |
| `p384_inter.der` | 564 |
| `p384_leaf.der` | 581 |
| `p521_root.der` | 628 |
| `p521_inter.der` | 636 |
| `p521_leaf.der` | 616 |
| `rsa384_root.der` | 894 |
| `rsa384_inter.der` | 903 |
| `rsa384_leaf.der` | 921 |
| `rsa512_root.der` | 894 |
| `rsa512_inter.der` | 903 |
| `rsa512_leaf.der` | 922 |

Note: **ECDSA signature lengths are not byte-reproducible** — a P-384 or P-521 signature's DER length varies by 1–2 bytes between runs. So the byte counts of `p384_*`, `p521_*` change if you regenerate; the RSA ones (`rsa384_*`, `rsa512_*`) are stable at this key size. Tests should assert semantic results, not ECDSA byte lengths. (Same caveat as `data/crypto_vectors/x509/README.md`.)

### 4.6 No private key material in any `.der`

Three independent checks per file, all passing:

1. the raw DER bytes contain none of the ASCII markers `PRIVATE KEY`, `PRIVATE`, `RSA PRIVATE`, `EC PRIVATE`, `BEGIN`
2. `openssl rsa -inform DER` and `openssl ec -inform DER` both **fail** on it (exit 1), i.e. it does not parse as a private key
3. `openssl asn1parse -inform DER` succeeds and the top-level element is a `SEQUENCE` — a `Certificate`

| 文件 | ASCII markers | `rsa -inform DER` | `ec -inform DER` | asn1parse |
|---|---|---|---|---|
| `p384_root.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `p384_inter.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `p384_leaf.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `p521_root.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `p521_inter.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `p521_leaf.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `rsa384_root.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `rsa384_inter.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `rsa384_leaf.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `rsa512_root.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `rsa512_inter.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |
| `rsa512_leaf.der` | none | exit 1 (rejected) | exit 1 (rejected) | SEQUENCE ✅ |

Private keys exist only in `keys/`:

| 文件 | 字节数 | PEM header | 位数 |
|---|---:|---|---:|
| `keys/p384_inter.key.pem` | 306 | `-----BEGIN PRIVATE KEY-----` | 384 |
| `keys/p384_leaf.key.pem` | 306 | `-----BEGIN PRIVATE KEY-----` | 384 |
| `keys/p384_root.key.pem` | 306 | `-----BEGIN PRIVATE KEY-----` | 384 |
| `keys/p521_inter.key.pem` | 384 | `-----BEGIN PRIVATE KEY-----` | 521 |
| `keys/p521_leaf.key.pem` | 306 | `-----BEGIN PRIVATE KEY-----` | 384 |
| `keys/p521_root.key.pem` | 384 | `-----BEGIN PRIVATE KEY-----` | 521 |
| `keys/rsa384_inter.key.pem` | 1704 | `-----BEGIN PRIVATE KEY-----` | 2048 |
| `keys/rsa384_leaf.key.pem` | 1704 | `-----BEGIN PRIVATE KEY-----` | 2048 |
| `keys/rsa384_root.key.pem` | 1704 | `-----BEGIN PRIVATE KEY-----` | 2048 |
| `keys/rsa512_inter.key.pem` | 1704 | `-----BEGIN PRIVATE KEY-----` | 2048 |
| `keys/rsa512_leaf.key.pem` | 1704 | `-----BEGIN PRIVATE KEY-----` | 2048 |
| `keys/rsa512_root.key.pem` | 1704 | `-----BEGIN PRIVATE KEY-----` | 2048 |

These are throwaway test keys with no real-world use, kept so `work/make_chains.py` can be re-run. Do not treat them as secrets and do not use them for anything real.

## 5. How to reproduce

```powershell
# 1. official NIST CAVP sources -> work/
python data/crypto_vectors/pki/work/fetch_cavp.py

# 2. the two vector JSON files
python data/crypto_vectors/pki/work/make_sha2_vectors.py
python data/crypto_vectors/pki/work/make_ecdsa_vectors.py

# 3. independent OpenSSL cross-check of every vector
python data/crypto_vectors/pki/work/verify_vectors.py

# 4. the certificate chains (skips keys that already exist)
python data/crypto_vectors/pki/work/make_chains.py

# 5. verify the chains and rebuild this manifest
python data/crypto_vectors/pki/work/verify_chains.py
python data/crypto_vectors/pki/work/make_manifest.py

# 6. final self-check: every number in this manifest vs. disk
python data/crypto_vectors/pki/work/check_manifest.py
```

`fetch_cavp.py` downloads three archives: `186-4ecdsatestvectors.zip` (the ECDSA SigVer/SigGen source), `ecccdhtestvectors.zip` (the ECC CDH primitive source) and `186-2ecdsatestvectors.zip` (kept only as evidence for the 3-P-per-section claim in section 6.1).

The originally guessed URL `…/components/CAVPKAS_ECC_CDH_PrimitiveTest.zip` returns HTTP 404 on csrc.nist.gov and was not used; see section 6.2.

## 6. Deviations from the task text (nothing hidden)

### 6.1 `[P-384,SHA-384]` has only 3 `Result=P`, not 4

The task asked for **4** `Result=P` records from `SigVer.rsp` `[P-384,SHA-384]`. No official CAVP ECDSA SigVer file contains 4. Measured:

- `SigVer.rsp`, `[P-384,SHA-384]`: **3 P / 12 F** (15 records, lines 1934–2040)
- `SigVer.rsp`, `[P-521,SHA-512]`: **3 P / 12 F** (lines 2576–2682)
- all **75** sections of that file: 3 P / 12 F — file totals 225 P / 900 F
- `186-2ecdsatestvectors.zip` (FIPS 186-2, downloaded and checked separately): also 3 P / 12 F per section

The layout is deliberate: 3 valid signatures plus 3 mutations each of `1 - Message changed`, `2 - R changed`, `3 - S changed`, `4 - Q changed`. So the request as written is unsatisfiable and a 4th `Result=P` was **not invented**. `p384_sha384` ships the 3 that exist. To still reach 4 valid P-384/SHA-384 verification vectors, one extra record is supplied in the clearly separated **`p384_sha384_extra_valid`** array; it comes from `SigGen.rsp` `[P-384,SHA-384]` in the *same* archive, is a valid-by-construction generated signature, and was confirmed `Verified OK` by OpenSSL. It is labelled `origin: SigGen.rsp …` and is **not** a CAVP-printed `Result`. `p521_sha512` needs no such supplement: it has exactly the requested 3 P / 2 F.

### 6.2 The ECDH data is not in `KASTestVectorsECC2014.zip`

The task named the file `KAS_ECC_CDH_PrimitiveTest.txt` (correct) and implied the KAS archive. `KASTestVectorsECC2014.zip` was downloaded and unpacked — it contains only SP 800-56A **scheme-level** `.fax` files and no `KAS_ECC_CDH_PrimitiveTest.txt`. The primitive-level file ships in `ecccdhtestvectors.zip`, linked from the CAVP component-testing page as "ECCCDH Primitive Test Vectors". That archive is the one used.

### 6.3 P-521 coordinates in the CDH file are 68 bytes, not 66

The task said P-521 coordinates are zero-padded to 66 bytes in the vector file. True for `SigVer.rsp`; **not** true for `KAS_ECC_CDH_PrimitiveTest.txt`, where the measured widths are:

| section | field | hex width | bytes |
|---|---|---:|---:|
| `[P-384]` | QCAVSx, QCAVSy, dIUT, QIUTx, QIUTy, ZIUT | 96 | 48 |
| `[P-521]` | QCAVSx, QCAVSy, dIUT, QIUTx, QIUTy | 136 | **68** |
| `[P-521]` | ZIUT | 132 | 66 |

`peer_x`/`peer_y` were stripped of leading zeros and re-padded to 132 hex chars to honour the required width. Values are unchanged; only the byte-string form was normalised.

### 6.4 `r`/`s` are not width-normalised in `r`/`s`

The task fixed a width only for `qx`/`qy`, so `r`/`s` are verbatim CAVP text. Because P-521 `r`/`s` are 131 hex chars (odd), the auxiliary `r_padded`/`s_padded` fields were added rather than silently changing `r`/`s`. See section 3.3 item 2.

### 6.5 Extra fields and keys beyond the requested schema

All additions are additive; every field the task named is present with the requested meaning.

- `sha2_vectors.json`: `msg_len`, `msg_desc` added per record
- `ecdsa_p384_p521.json`: `result_reason` added per ECDSA record; `r_padded`/`s_padded` added per ECDSA record; the extra top-level array `p384_sha384_extra_valid`
- the extra `ecdh_p384`/`ecdh_p521` keys *were* requested

