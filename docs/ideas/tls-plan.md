# 设计文档：自写 TLS 1.3 客户端与 https 支持（未实施）

> **状态：P0 / P1 已实施（2026-10-01，见 §14、§15），P2 起待实施。** 2026-10-01 定稿。
> **性质：内部设计记录**，不是用户侧帮助。这里**故意**写设计理由与被否方案，
> 与 `data/documentation.json`（只写契约）的规则不同，勿混。
> 起因：`http.httpGet` 只支持明文 HTTP，`https://` 被直接拒绝（见 §1）。

---

## 1. 问题与现状

`modules/http.jc2` 的整条链路是**裸 TCP**：

```
http.httpGet → net.TcpSocket → socket.connect（原生）→ 明文 HTTP 文本协议
_Http.parse_url 见到 "https://" → throw Exception("HttpError", …)
```

仓库里**没有任何加密代码**（`src/lib/` 12 个原生模块；`SHA256|AES|RSA|X509|TLS|SSL` 全库零命中）。
可直接复用的地基（均为实测定位）：

| 已有 | 位置 | 用途 |
|---|---|---|
| `BigInt::modPow(base, exp, mod)` | `src/math/BigInt.h:1659` | RSA-PSS/PKCS#1 验签、P-256 模运算 |
| BigInt 大数底座（FFT 乘法/牛顿迭代/`divmod`） | `src/math/BigInt.h` | 上述一切的算术层 |
| `bytes.fromHex` / `bytes.fromBase64` | `src/lib/bytes/bytes_module.cpp` | PEM/DER 解析 |
| `deflateDecompress` | `src/utils/deflate/Deflate.h:18` | P4 的 gzip 解压 |
| 系统库链接先例 | `CMakeLists.txt:153/174-176`（`ws2_32/User32/Gdi32/imm32`，均在 `if(WIN32)` 内） | 新增 `bcrypt/crypt32` 不破坏"零第三方依赖" |

**缺失**：SHA-256/MD5/HMAC/HKDF（0）、AES-GCM/ChaCha20（0）、CRC32（0）、EC 曲线运算（0）、
DER/X.509（0）、TLS 记录层与握手（0）、`BigInt::modInverse`（0）。

---

## 2. 目标与非目标

**目标**：`httpGet("https://…")` 可用；证书校验严格（**校验不过就断连**）；零第三方依赖；
实现全部在仓库内自写 C++。

**v1 非目标**：TLS 1.2、客户端证书、会话恢复/PSK、0-RTT、CRL/OCSP 撤销、跨平台信任库、
AES-NI 优化、HTTP 层的 chunked/gzip/重定向（后者另立 P4）。

---

## 3. 决策集（终稿）

| # | 决策 | 终稿 | 说明 |
|---|---|---|---|
| D1 | 协议版本 | **只 TLS 1.3** | 1.2 留作可选 P5 |
| D2 | 曲线 | **X25519 + secp256r1** | 与 D4 捆绑（群运算共用） |
| D3 | 密码套件 | **只 `TLS_AES_128_GCM_SHA256`** | ChaCha20（+~400 行）与 AES-256-GCM（需 SHA-384，+~250 行）延后 |
| D4 | 签名算法 | **RSA-PSS/PKCS1 + ECDSA-P256** | 只支持 RSA 会漏掉一部分 ECDSA 链 |
| D5 | 撤销检查 | **不做** | 浏览器同为软失败；文档写明 |
| D6 | 原语测试载体 | **公开原生模块 `crypto`** | 测试走项目自己的 runner，保持"三套测试全绿"的验收方式 |
| D7 | 原生模块名 | **`tls`** | 不能叫 `http`（见 §7.1） |
| D8 | 原语来源 | **全自写**（不借 CNG） | 变体 E 见 §11.3 |
| D9 | HRR | **不实现**，收到即以 alert 失败 | 靠 ClientHello 同时携带两个 key_share 规避 |
| D10 | "忽略证书错误"开关 | **不提供** | 否则"能连上"与"验过了"再也分不清 |

**两处修订的由来**（2026-10-01 评审）：

- D2+D4 合并推进：P-256 的点运算/模逆**同时**服务 ECDHE 与 ECDSA 验签，一起做只比单做多约
  200 行；而"只 RSA"会连不上一部分真实站点。因此原 P1b 并入 P1。
- D9 不做 HRR：既然 D2 已具备 P-256，ClientHello 直接带 `x25519` 与 `secp256r1` 两个
  key_share，服务器有可用组就不会发 HRR。HRR 于是从"功能点"降级为**负例测试**
  （必须明确失败，不允许卡死/崩溃）。

---

## 4. 范围与算法

| 项 | v1 选择 | 备注 |
|---|---|---|
| 协议 | TLS 1.3（RFC 8446） | `supported_versions = [0x0304]` |
| 套件 | `TLS_AES_128_GCM_SHA256` | 单套件 |
| 密钥交换 | `x25519`(0x001d) + `secp256r1`(0x0017) | 两者都发 key_share |
| 签名算法 | `rsa_pss_rsae_sha256`、`rsa_pkcs1_sha256`、`ecdsa_secp256r1_sha256` | 覆盖链上各层 |
| 证书校验 | 逐级验签 + 有效期 + basicConstraints/keyUsage + RFC 6125 主机名（SAN 含通配符） | 不做 CN 兜底、不做撤销 |
| 信任锚 | Windows ROOT 存储（CryptoAPI 枚举，进程内缓存） | 主流做法，见 §12 |
| 熵 | `BCryptGenRandom` | 绝不使用 `mt19937`（现有 `random` 模块用的就是它） |
| SNI / ALPN | 必发 SNI；ALPN 发 `http/1.1` | 为将来 http/2 留位 |

---

## 5. 分层与文件布局

```
src/crypto/                       纯算法层（不依赖 JC2 运行时，可独立测试）
  Sha256.{h,cpp}                  SHA-256（增量 + 一次性）
  Hmac.{h,cpp}                    HMAC-SHA256
  Hkdf.{h,cpp}                    HKDF-Extract / Expand / Expand-Label
  AesGcm.{h,cpp}                  AES-128-GCM（含 GHASH）
  X25519.{h,cpp}                  RFC 7748
  P256.{h,cpp}                    secp256r1 点运算 + ECDH + ECDSA 验签
  Rsa.{h,cpp}                     RSA-PSS / PKCS#1 v1.5 验签（吃 BigInt::modPow）
  Der.{h,cpp}                     DER TLV 游标（严格边界检查）
  X509.{h,cpp}                    证书解析 + 链校验
  Hostname.{h,cpp}                RFC 6125 SAN 匹配
  TrustStore.{h,cpp}              Windows ROOT 枚举（CryptoAPI）+ 缓存
  Random.{h,cpp}                  BCryptGenRandom
  ConstTime.{h}                   常数时间比较/清零
src/net/TcpStream.{h,cpp}         最小 TCP：getaddrinfo/connect/select 超时/send/recv
src/tls/TlsClient.{h,cpp}         记录层 + 握手状态机 + 密钥调度 + alert 映射
src/lib/crypto/crypto_module.cpp  原生模块：公开 P0 原语（测试载体，D6）
src/lib/tls/tls_module.cpp        原生模块：导出 TlsSocket
modules/net.jc2                   + class TlsSocket（与 TcpSocket 接口对齐）
modules/http.jc2                  https 分支接线
tests/modules/test_crypto.jc2     原语向量测试
tests/modules/test_tls.jc2        握手回放 + 接线测试
data/crypto_vectors/              夹具：证书链、录制的握手字节
```

`src/net/TcpStream` 的由来：`src/lib/socket/socket_module.cpp` 是自包含的单文件，没有可共享的
头；`tls` 模块要么复制约 60 行 winsock 连接逻辑，要么抽出这一层。选后者（顺带让 socket 模块的
连接逻辑只剩一处）。

---

## 6. 各层内容与关键接口

| 层 | 关键接口（草案） | 行数估 |
|---|---|---|
| SHA-256 | `sha256(p, n, out32)`、增量 `Sha256Ctx` | 300 |
| HMAC / HKDF | `hmacSha256(k, m)`、`hkdfExpandLabel(secret, label, ctx, len)` | 350 |
| AES-128-GCM | `aesGcmSeal/Open(key16, nonce12, aad, …)`；GHASH 用 128 位乘法 | 600 |
| X25519 | `x25519(out32, scalar32, u32)`（clamp + Montgomery ladder） | 350 |
| P-256 | `p256Ecdh(out33, priv32, peer65)`、`p256EcdsaVerify(pub65, hash32, r, s)` | 600 |
| `BigInt::modInverse`（新增） | 扩展欧几里得；ECDSA 验签与 P-256 点运算都要 | 50 |
| DER | `DerReader`（TLV 游标 + 长度/边界严格检查） | 350 |
| X.509 | `Certificate::parse(der)`、`verifyChain(leaf, intermediates, roots, host) → 结果码` | 700 |
| 主机名 | RFC 6125 SAN 匹配（`*.example.com` 语义；不做 CN 兜底） | 150 |
| RSA 验签 | `rsaVerifyPss/Pkcs1(n, e, msg, sig)`（严格填充检查） | 350 |
| 信任库 / 熵 | `enumerateRoots()`（CryptoAPI + 缓存）、`randomBytes()`（BCrypt） | 200 |
| TLS 1.3 | `TlsClient::connect(host, port, opts)`；`sendRecord/recvRecord`、transcript、密钥调度、alert | 1800 |
| `tls` 模块 | `tls.connect(host, port, {"sni","alpn","timeout_ms"}) → TlsSocket{send,recv,close}` | 350 |
| net / http 接线 | `net.TlsSocket`；`parse_url` 允许 https | 140 |

握手要点（避免踩坑的清单）：record 层 5 字节头与 16KB 上限、跨记录分片与多记录合并、
transcript 哈希、HKDF-Expand-Label 的 `"tls13 "` 前缀、ChangeCipherSpec 兼容记录需忽略、
CertificateVerify 的签名输入（64 空格 + context string + 0x00 + transcript）、
Finished 的 HMAC、`close_notify` 与 alert 映射、`select` + `SO_RCVTIMEO` 超时。

---

## 7. 接线

### 7.1 原生模块只能叫 `tls`

`execImport` 的查找顺序是 **`<exe>/lib/<name>.dll` → 当前目录原生库 → `.jcb` → `.jc2`**
（`src/vm/VM.cpp:2703` 起）。因此原生模块若命名为 `http`，会**遮蔽 `modules/http.jc2`**。

### 7.2 构建系统三处注册

1. `add_library(tls SHARED "src/lib/tls/tls_module.cpp")`（`crypto` 同理）；
2. 加入 `foreach(mod IN ITEMS …)` 列表（符号收集器）；
3. 加入 `set(JC2_LIBS …)`；
4. 系统库放在 `if(WIN32)`：`tls` → `ws2_32 bcrypt crypt32`，`crypto` → 无（纯算法）。

### 7.3 脚本侧

- `modules/net.jc2`：新增 `class TlsSocket`，接口与 `TcpSocket` 对齐（`send/recv/close/connected`）；
  维持"脚本描述 API、原生提供能力"的既有分工。
- `modules/http.jc2`：`parse_url` 允许 `https://`；`httpGet` 的 https 分支用 `net.TlsSocket`；
  **`HttpResponse` 与 `httpGet` 的公开面完全不变**，用户代码不用改。

### 7.4 诚实记录的局限

即使 https 打通，`http.jc2` 仍然**只处理 `Connection: close` 的简单响应**（读到 EOF 再切
`\r\n\r\n`），没有 chunked 解分块、没有 gzip、没有 3xx 重定向 —— 这三项在 P4（复用
`deflateDecompress` + 新增 CRC32），与 TLS 本身无关。

---

## 8. 测试与验收标准

| 期 | 可测验收 |
|---|---|
| P0 | SHA-256：FIPS 180-4 向量 + 长度边界（0/55/56/63/64/65/1MB）；HMAC：RFC 4231 全 7 组；HKDF：RFC 5869 A.1–A.3；AES-GCM：NIST CAVP 15 组 + 空明文/空 AAD + **tag 篡改必须失败**；X25519：RFC 7748 §5.2 两组 + 迭代 1/1000 次 |
| P1 ✅ | 20 条链（10 正 / 10 负：过期、主机名不匹配、缺中间证书、`CA:FALSE` 签发、pathLen 超限、自签不在信任库、签名被篡改、DER 截断），其中含 ECDSA 签发与 RSA/ECDSA 混合链；P-256：NIST CAVP ECDH 向量 + RFC 6979 ECDSA 验签向量；DER 解析器对截断/超长输入不越界（配 ≤10⁶ 次随机模糊）（实际：13 条链 + CAVP SigVer 向量 + 1500 次模糊，见 §15） |
| P2 | 录制回放 6 组：RSA 链 / ECDSA 链 / **HRR（必须明确失败）** / 会话中 alert / 跨记录分片 / 多记录合并；每组断言"派生密钥 + Finished 校验结果"；另加 3 个真实站点（手动步骤，不进 CI） |
| P3 | `tests/modules/test_tls.jc2` 离线端到端；`httpGet("https://…")` 手动验证 |
| P4 | chunked / gzip / 重定向的脚本单测（本地夹具，不联网） |

测试载体：P0 原语经原生模块 `crypto` 从脚本调用，向量断言写在 `tests/modules/test_crypto.jc2`，
因此原语层的验收天然进入 `jc2.exe test tests`（三套测试之一）。
P1/P2 的夹具放 `data/crypto_vectors/`（证书链用真实站点抓取并去私钥，握手字节为录制回放）。

---

## 9. 分期与工作量

| 期 | 内容 | 行数估 | 工时估 |
|---|---|---|---|
| P0 ✅ | SHA-256 / HMAC / HKDF / AES-128-GCM / X25519 + 向量测试（见 §14，实际约 1.25k 行） | ~2.0k | 2–4 天 |
| P1 ✅ | DER / X.509 / 链校验 / 主机名 / RSA / **P-256 + ECDSA** / 信任库（见 §15，实际约 2.2k 行） | ~2.8k | 5–8 天 |
| P2 | TLS 1.3 握手 + 记录层 + 回放测试（不含 HRR） | ~1.8k | 4–7 天 |
| P3 | `tls` 模块 + `net.TlsSocket` + http 接线 | ~0.5k | 1–2 天 |
| P4（可选） | chunked / gzip / 重定向 | ~0.3k | 0.5–1 天 |
| **合计** | | **~7.4k 行** | **12–21 天** |

覆盖面是**最小子集**：只 1.3、单套件族、无撤销/会话恢复/PSK/0-RTT。
参照量级：Go 的 `crypto/tls` 为万行量级，其原语（`crypto/aes`、`sha256`、`ecdsa`、`x25519`…）
再十几 k 行 —— 本方案的估算与之相比并不虚。

---

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| **校验有洞**（能连上但没真验证）——最致命 | 10 条负例链；不带"忽略证书错误"开关（D10）；宁可失败 |
| AEAD tag / MAC 比较非恒定时间 | 统一走 `ConstTime` 入口 |
| BigInt 版 X25519 / ECDSA 有计时侧信道 | 客户端临时密钥风险低，记录在案；必要时后续换固定 limb 实现 |
| DER 解析越界 | 严格边界检查 + 随机模糊测试 |
| 服务器不选我们提供的组 | D9：收到 HRR 以明确 alert 失败，不卡死、不崩溃（负例测试覆盖） |
| 记录层粘包/半包/分片 | 显式状态机 + 回放测试覆盖 |
| 无 AES-NI 时吞吐约 20–60 MB/s | 对脚本语言的 HTTP 客户端够用；后续可加 AES-NI |
| 阻塞 IO 卡死 | `select` + `SO_RCVTIMEO` 统一超时 |

---

## 11. 被否与备选方案

### 11.1 WinHTTP（系统 HTTP 客户端）

`WinHttpOpen/Connect/OpenRequest/SendRequest/ReceiveResponse/ReadData`，约 200–300 行即可让
`httpGet` 支持 https，并**白拿** chunked/gzip/重定向/代理，证书校验走系统实现随 Windows 更新。
**否掉的理由**：它只是"HTTP 能用了"，不产生任何自写能力（本项目已有自写 DEFLATE/BigInt/正则 VM 的脉络）；
且它无法服务 HTTP 以外的协议。保留为**对照实现/兜底**的选项（见 §13）。

### 11.2 SChannel 通用 TLS

`AcquireCredentialsHandle` + `InitializeSecurityContext` + `EncryptMessage/DecryptMessage`，
约 600–1200 行，可给任意协议加 TLS。**暂不采用**：它是"借系统实现 TLS"，与 D8（全自写）冲突；
若将来要 SMTP/IMAP 上 TLS，可再评估（届时它与自写层可并存）。

### 11.3 变体 E：协议自写 + 原语借 CNG（rustls 模式）

TLS 协议逻辑全自写，只把 SHA/AEAD/ECDH 换成 `BCryptHash`/`BCryptEncrypt`/`BCryptSecretAgreement`：
省约 1.5k 行，P0 缩到约 0.5k，总计约 5k 行。代价是**侧信道最敏感的部分交给 OS**，"自写"纯度下降；
但它是系统 API（与已用的 `ws2_32` 同类），零依赖叙述不受影响，且与 rustls 的取舍一致。
**当前否掉**（D8 选了全自写），记录在案备查。

### 11.4 内嵌第三方 TLS（BearSSL / mbedTLS）

集成 1–2 天、体积与许可证成本。**否掉**：破坏"零依赖"叙述。

### 11.5 JC2 脚本层 TLS

用脚本实现握手与原语，作为语言能力的展示。**否掉**：性能不可用、常数时间无法保证，
且把安全关键代码放在最容易被改坏的一层。

### 11.6 外调 curl / PowerShell

`CreateProcess` + 管道（仓库目前没有进程+管道能力），约 150 行。**否掉**：依赖外部程序存在、
无流式控制、错误映射粗糙，不算语言能力。

---

## 12. 依据：主流实现怎么做

本机 CPython 实测（bundled 发行版）：

```
ssl 实现        : …/Lib/ssl.py            ← 纯 Python 包装层
_ssl 模块       : …/DLLs/_ssl.pyd         ← C 扩展，链接 OpenSSL
OpenSSL 版本    : OpenSSL 3.5.8           ← 握手/套件/证书链校验都在这里
hashlib.sha256  : _hashlib.openssl_sha256 ← 连哈希都借 OpenSSL
_sha256 后备模块: 不存在（该发行版未编入）
load_default_certs() 在 win32 上遍历系统证书存储（enum_certificates）
```

| 实现 | TLS 协议 | 密码学原语 | 信任库 |
|---|---|---|---|
| CPython | OpenSSL | OpenSSL | Windows 存储 + OpenSSL 默认路径 |
| Node.js / .NET(Win) | OpenSSL / SChannel | 同上 | 系统 |
| **Go** | **自写 `crypto/tls`** | **自写 `crypto/*`** | 系统存储（Windows 走 CryptoAPI） |
| **rustls** | 自写 | 借 ring / aws-lc-rs | webpki-roots / 系统验证器 |
| tlslite-ng | 纯 Python 自写 | 纯 Python 自写 | 自带 CA bundle |
| PyCryptodome | 不提供 TLS | 自写 C 原语 | — |

结论：

1. **主流是"不自己写 TLS"**（Python/Node/.NET 全是包 OpenSSL 或系统）。要的是"能用且校验可靠"，
   那就是 §11.1/§11.2 的路线。
2. **"自写 TLS"的成熟先例是 Go 与 rustls**：Go 连原语都自写，rustls 只自写协议。
   **两者都使用系统信任库** —— 所以本方案"信任锚借 CryptoAPI 枚举 ROOT"是主流做法，不是偷懒。
3. **纯自写全栈的先例是 tlslite-ng（纯 Python）**：能用，但定位一直是自测/教学。
   本方案接受同样的定位取舍：**能力与门面**，而不是"在可靠性上与 OpenSSL 比"。

---

## 13. 开放问题（不阻塞 P0）

1. P3 之后是否同时保留 WinHTTP 作为**对照实现/兜底**（测试时两实现比对结果，生产可切换）。
   好处：多一层保险与互操作参照；代价：两条路径行为需保持一致。
2. TLS 1.2 是否最终要做（现实里仍有少量 1.2-only 服务器）：若做，估 +1.5–3k 行 / +3–6 天。
3. AES-NI 优化与 ChaCha20 套件是否补（分别约 250 / 400 行）。
4. ~~`crypto` 模块（D6）的公开面~~ **已定（P0 实施时）**：二进制一律十六进制字符串，
   进出同格式；公开 `sha256 / hmacSha256 / hkdfExtract / hkdfExpand / hkdfExpandLabel /
   aesGcmSeal / aesGcmOpen / x25519 / x25519Base`，帮助文本写在模块内（原生模块的惯例，
   `data/documentation.json` 只收录 `sys/math/cas/random`）。

---

## 14. P0 实施记录（2026-10-01）

**落地文件**

| 文件 | 内容 |
|---|---|
| `src/crypto/ConstTime.h` | 常数时间比较 `ctEqual` 与清零 `ctZero` |
| `src/crypto/Sha256.{h,cpp}` | 增量 SHA-256，final 就地填充（不重入 update） |
| `src/crypto/Hmac.{h,cpp}` | HMAC-SHA256（RFC 2104，长密钥先压缩） |
| `src/crypto/Hkdf.{h,cpp}` | HKDF-Extract / Expand + TLS 1.3 HKDF-Expand-Label |
| `src/crypto/AesGcm.{h,cpp}` | AES-128-GCM（正向分组 + GHASH + CTR；非 96 位 IV 按 SP 800-38D 推导 J0） |
| `src/crypto/X25519.{h,cpp}` | 5×51 位肢体 Montgomery 阶梯（MSVC 无 `__int128`，用 `_umul128`） |
| `src/lib/crypto/crypto_module.cpp` | 原生模块：hex 进 hex 出，`CryptoError` 表达失败 |
| `tests/modules/test_crypto.jc2` | 官方向量 + 负例 |

**构建**：新增静态库 `jc2_crypto`（`/W4 /WX /utf-8`），`crypto` 模块链接它并进 `JC2_LIBS`；
符号收集器同样链接它（收集器直接 `#include` 模块源码），产物 `lib/crypto.json` 已生成。

**验收（全部通过，三套测试 56/56、56/56、18/18）**

- SHA-256：FIPS 180-4 四组 + 10 个长度边界（55/56/63/64/65/119/120/127/128/1000）
- HMAC-SHA256：RFC 4231 §4.2–4.8 全 7 组（case 5 的 RFC 印刷值截断到 128 位，断言用完整 256 位）
- HKDF：RFC 5869 A.1（PRK+OKM）、A.2（长输入）、A.3（空 salt/info），另加 Expand-Label 结构自检
- AES-128-GCM：NIST CAVP `gcmEncryptExtIV128.rsp` 八组（IV 8/96/1024 位，明文 0/13/16/51 字节，AAD 0/16/20 字节）
  + 每条向量都断言"篡改 tag / 篡改 AAD / 篡改密文必须抛 CryptoError"
- X25519：RFC 7748 §5.2 两组 + 迭代测试（1 次与 1000 次）+ §6.1 的 DH 双方公钥与共享密钥
  + 低阶点（全零输出）必须报错
- 参数校验：奇数长度 hex、非 hex 字符、AES 密钥长度、非字符串实参（TypeError）

**反向验证**：X25519 的 a24 配对写错、以及把 GCM 的 tag 校验旁路，都会让测试变红。
后者还先暴露出测试自身的一个缺陷 —— 篡改 tag 时只保留了 31 个 hex 字符，被长度校验
"抢先"拦下（断言通过但理由错误）；已改为翻转最后一个 nibble，保持 32 字符。

**与估算的差异**：P0 实际约 1.25k 行（原估 ~2.0k）。GHASH 用逐位实现（未做 4 位表），
1 MB 载荷约需 6.5 万次 128 轮循环；当前只服务握手与 HTTP 客户端，够用，AES-NI/查表优化
留待需要时再做（见 §13.3）。


---

## 15. P1 实施记录（2026-10-01）

**落地文件**

| 文件 | 内容 |
|---|---|
| `src/crypto/BigNum.{h,cpp}` | 自足定长大数（≤4096 位 / 65 肢）：学校乘法 + 逐位长除规约、addMod/subMod/mulMod/expMod、费马模逆 |
| `src/crypto/BytesUtil.h` | 十六进制与字节工具（不再依赖任何 JC2 运行时类型） |
| `src/crypto/Der.{h,cpp}` | DER TLV 游标：拒绝不定长与非最短长度编码，全部读取带边界检查 |
| `src/crypto/X509.{h,cpp}` | 证书解析（TBSCertificate/SPKI/扩展）+ 链校验 + 原因码 |
| `src/crypto/Hostname.{h,cpp}` | RFC 6125 dNSName 匹配 |
| `src/crypto/Rsa.{h,cpp}` | PKCS#1 v1.5 与 RSASSA-PSS（SHA-256 / MGF1-SHA-256）验签 |
| `src/crypto/P256.{h,cpp}` | 仿射坐标点运算、ECDH、ECDSA 验签 |
| `src/crypto/Random.{h,cpp}` | `BCryptGenRandom` |
| `src/crypto/TrustStore.{h,cpp}` | CryptoAPI 枚举 Windows ROOT 存储（进程内缓存） |
| `src/lib/crypto/crypto_module.cpp` | 新增 9 个公开函数（见下） |
| `tests/modules/test_x509.jc2` | 全部 P1 验收断言 |
| `data/crypto_vectors/x509/` | 22 个 DER 夹具 + manifest + generate.ps1 + README |

**新增公开面**：`p256Ecdh / ecdsaVerifyP256 / rsaVerifyPkcs1 / rsaVerifyPss / hostnameMatch /
x509Parse / x509Verify / systemRoots / randomBytes`（约定同 P0：二进制一律十六进制字符串）。

**构建**：`jc2_crypto` 增加 12 个源文件并链接 `bcrypt`、`crypt32`（系统库）。

**两个关键实现取舍**

1. **不复用 `jc::BigInt`**：`src/crypto` 按设计文档 §5 是纯算法层，而 `jc::BigInt` 依赖主程序的
   `jc::err::*` 符号；一旦引入，模块 DLL 与符号收集器都会 LNK2001（实测）。因此自写 BigNum。
   实现取"显然正确"路线（学校乘法 + 逐位规约），RSA 只做公开指数很小的验签、P-256 一次验签约
   十几毫秒，够用；需要更快时再换 Montgomery。
2. **信任锚由调用方注入**：`x509Verify(leaf, intermediates, roots, hostname, time)` —— 生产用
   `systemRoots()`（系统 ROOT），测试用固定夹具；时间也是参数，测试因此**与本机当前时间无关**。

**验收（全部通过，三套测试 57/57、57/57、18/18）**

- 主机名：精确/大小写/尾点、通配符只替换一个标签、不得匹配公共后缀、禁止部分通配符
- P-256 ECDH：NIST CAVP `KAS_ECC_CDH_PrimitiveTest` `[P-256]` 前 3 组；非曲线点必须拒绝
- ECDSA P-256/SHA-256：NIST CAVP SigVer 3 正 2 负；另测 r=0、公钥不在曲线上
- RSA-PSS：本机 openssl 生成的固定向量；PSS 签名不得当作 PKCS#1 v1.5 通过
- 证书解析：subject/issuer/SAN/有效期/CA/pathLen/公钥类型/签名算法 OID
- 证书链：**4 正**（RSA、EC、EC 叶 + RSA 中间、通配符）+ **9 负**（过期、未生效、主机名不匹配、
  CA:FALSE 签发、pathLen 违规、自签不受信、链不落到指定根、签名被改一字节、缺中间证书），
  逐条断言失败原因码
- DER 健壮性：截断与垃圾输入抛 `CryptoError`；1500 次确定性 nibble 翻转模糊下不得出现其它异常
- 反向验证：把 `verifyOneSignature` 旁路成恒真后，"签名被篡改"负例立即变红（证明该负例测的是真校验）

**实施中查出并修掉的三个真缺陷**（都由上面的向量/夹具钉出来）

1. **P-256 生成元 x 坐标写错**（多了一个 nibble，65 位十六进制）：ECDH 向量仍然全过 —— 因为
   ECDH 根本不用 G；只有 ECDSA 用的是 G，于是 CAVP SigVer 立刻变红。用"曲线方程自检 + n·G = 无穷远"
   两条独立判据定位。
2. **SubjectPublicKeyInfo 多解析了一层**：把 SPKI 内容当成了还带 SEQUENCE 包装，导致**所有**证书
   解析失败（用逐文件解析探针定位到阶段号）。
3. **tbsCertificate 切片错位**：先是把外层 Certificate 头算进切片，修完又发现 `DerReader::pos` 是
   相对 Certificate 内容的偏移而非整个缓冲区 —— 两处都表现为"RSA 链签名校验失败"。修正后
   4 条正向链全部通过。

**夹具口径（详见 `data/crypto_vectors/x509/README.md`）**

- openssl 3.2.1；`keys/*.pem` 是一次性测试密钥，不是机密
- 有效期固定在 2020-01-01 起，测试统一用 `1717200000`（2024-06-01T00:00:00Z）判定
- ECDSA 证书 DER 长度不保证逐字节复现 → 测试只断言语义结果，不依赖字节数
- 未做 `neg_crypto` 之外的撤销类负例（D5：v1 不做 CRL/OCSP）

**未覆盖 / 已知边界**

- 4016 位以上的 RSA 模数超出 BigNum 上限（RSA-4096 在限内；更大者会被拒，不是静默出错）
- RSA-PSS **证书**未进夹具（曲线外场景少），PSS 由独立向量覆盖；链上的 RSA 均为 PKCS#1 v1.5
- `systemRoots()` 的内容与机器相关，测试只断言接口形状与"至少一个能解析"
- 签名算法只支持 SHA-256 家族（SHA-1 系一律判为 `unsupported-algorithm`，符合现代取舍）
