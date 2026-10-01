// 通用短 Weierstrass 椭圆曲线（NIST P-256 / P-384 / P-521）：点运算、ECDH、ECDSA 验签。
//
// 坐标系统：**Jacobian 射影坐标**（Z=1 表示仿射），一次标量乘只在最后做一次模逆。
// 早期 P-256 用仿射坐标（每步点加/倍点各一次费马求逆），在 P-384/P-521 上会慢到不可用
// （一次验签数秒），所以这里统一换成 Jacobian。
//
// 侧信道：标量乘是 double-and-add，非恒定时间。ECDSA 验签用的都是公开数据；
// ECDH 的私钥是本进程生成的临时密钥 —— 与项目既有实现一致，不做侧信道加固。
#pragma once

#include "BigNum.h"
#include "BytesUtil.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace jc2::crypto {

enum class EcCurve { P256, P384, P521 };

struct EcPoint {
    BigNum x;
    BigNum y;
    bool infinity = true;
};

struct EcGroup {
    EcCurve id;
    const char* name;      // "P-256"
    const char* oid;       // 曲线 OID（点的命名曲线参数）
    BigNum p;
    BigNum a;              // 三条曲线的 a 都是 p - 3
    BigNum b;
    BigNum n;              // 基点阶
    BigNum gx;
    BigNum gy;
    size_t fieldBytes;     // 32 / 48 / 66
    size_t bits;           // 256 / 384 / 521
};

const EcGroup& ecGroup(EcCurve curve);
bool ecCurveFromOid(const std::string& oid, EcCurve& out);
std::string ecCurveName(EcCurve curve);

// 未压缩点：0x04 || X || Y（长度必须精确等于 1 + 2*fieldBytes），且必须在曲线上。
bool ecDecodePoint(const EcGroup& g, const uint8_t* data, size_t len, EcPoint& out);
bool ecPointToBytes(const EcGroup& g, const EcPoint& p, Bytes& out);

bool ecIsOnCurve(const EcGroup& g, const EcPoint& p);
EcPoint ecScalarMul(const EcGroup& g, const Bytes& k, const EcPoint& p);
bool ecBasePointMul(const EcGroup& g, const Bytes& privateKey, Bytes& outUncompressed);

// ECDH：输出共享点的 X 坐标（fieldBytes 字节）。
bool ecEcdh(const EcGroup& g, const Bytes& privateKey, const uint8_t* peerPoint, size_t peerLen, Bytes& shared);

// ECDSA 验签。digest 是消息摘要（SHA-256/384/512 都行），r/s 为大端字节。
bool ecEcdsaVerify(const EcGroup& g, const EcPoint& pub, const uint8_t* digest, size_t digestLen, const Bytes& r,
                   const Bytes& s);

// 从 DER SEQUENCE { INTEGER r, INTEGER s } 里取出 r、s。
bool ecEcdsaSignatureFromDer(const Bytes& der, Bytes& r, Bytes& s);

} // namespace jc2::crypto
