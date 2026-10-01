# X.509 测试夹具（P1）

供 `tests/modules/test_x509.jc2` 使用的证书链夹具，全部由 openssl 3.2.1 生成、**离线**使用。

## 内容

| 路径 | 说明 |
|---|---|
| `*.der` | 22 个 DER 证书：4 条正向链（RSA / EC / 混合 / 通配符）+ 9 类负向场景 |
| `manifest.md` | 逐文件清单：subject / issuer / SAN / 有效期 / 公钥类型 / **期望校验结果与失败原因**，以及每条链的文件顺序和 openssl 实测输出 |
| `generate.ps1` | 重新生成全部夹具；`-Check` 只做自检（核对 manifest 与磁盘、重跑 19 条关键 `openssl verify`） |
| `openssl.cnf` | 生成脚本使用的 CA 配置 |
| `keys/*.pem` | 生成时用的私钥 |
| `csr/`、`work/` | 生成过程中的中间产物（未纳入版本库，见 `.gitignore`） |

## 关于私钥

`keys/` 里的私钥是**一次性测试密钥**，只用于生成这些证书，没有任何现实用途，也不保护任何真实资产。
它们被保留是为了让 `generate.ps1` 能重跑；请勿把它们当作机密，也不要用于任何真实场景。

## 重新生成与自检

```powershell
# 只自检（不重新生成）：核对磁盘字节数与 manifest，并重跑关键 openssl verify
powershell -File data\crypto_vectors\x509\generate.ps1 -Check

# 完全重新生成（会改写 *.der 与 manifest.md）
powershell -File data\crypto_vectors\x509\generate.ps1
```

注意：**ECDSA 证书的 DER 长度不保证逐字节可复现**（P-256 签名每次的 DER 长度可能差 1–2 字节），
所以测试断言的是**语义结果**（是否通过、失败原因），不依赖文件字节数；RSA 证书长度稳定。

## 有效期与时间

所有证书的有效期都是固定的（2020-01-01 起、多为 2035-01-01 止；过期/未生效两个负例例外）。
测试统一用固定时间 `1717200000`（2024-06-01T00:00:00Z）做判定，因此**与本机当前时间无关**。

## 期望结果的口径

`manifest.md` 里的"期望失败原因"是 **openssl** 的判定。本项目的校验器（`crypto.x509Verify`）
返回自己的原因码（`expired` / `hostname-mismatch` / `issuer-not-ca` / `path-len-exceeded` /
`no-trust-anchor` / `signature-invalid` 等），二者的对应关系写在
`tests/modules/test_x509.jc2` 的断言里。
