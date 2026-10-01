#include "Der.h"

#include <cstring>

namespace jc2::crypto {

namespace {

// Howard Hinnant 的 days_from_civil：把公历年月日转成自 1970-01-01 起的天数。
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parseDigits(const uint8_t* p, size_t n, int& out) {
    if (n == 0) return false;
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (p[i] - '0');
    }
    out = v;
    return true;
}

} // namespace

bool DerReader::read(uint8_t& tag, const uint8_t*& content, size_t& contentLen) {
    if (!ok || pos >= size) {
        ok = false;
        return false;
    }
    const uint8_t t = data[pos++];

    // 不支持高标签号形式（X.509 用不到）
    if ((t & 0x1f) == 0x1f) {
        ok = false;
        return false;
    }
    if (pos >= size) {
        ok = false;
        return false;
    }

    const uint8_t first = data[pos++];
    size_t len = 0;
    if ((first & 0x80) == 0) {
        len = first;
    } else {
        const size_t numBytes = static_cast<size_t>(first & 0x7f);
        if (numBytes == 0 || numBytes > sizeof(size_t)) {
            ok = false;   // 不定长或过长长度域
            return false;
        }
        if (pos + numBytes > size) {
            ok = false;
            return false;
        }
        for (size_t i = 0; i < numBytes; ++i) {
            const uint8_t b = data[pos++];
            if (i == 0 && b == 0x00) {
                ok = false;   // DER 不允许长度域前导零
                return false;
            }
            len = (len << 8) | b;
        }
        // DER 不允许非最短长度编码（前导零）
        if (len < 128) {
            ok = false;
            return false;
        }
    }

    if (len > size - pos) {
        ok = false;
        return false;
    }

    tag = t;
    content = data + pos;
    contentLen = len;
    pos += len;
    return true;
}

bool DerReader::readExpected(uint8_t want, const uint8_t*& content, size_t& contentLen) {
    uint8_t tag = 0;
    if (!read(tag, content, contentLen)) return false;
    if (tag != want) {
        ok = false;
        return false;
    }
    return true;
}

bool DerReader::peek(uint8_t& tag) const {
    if (!ok || pos >= size) return false;
    tag = data[pos];
    return true;
}

bool DerReader::skip() {
    uint8_t tag = 0;
    const uint8_t* content = nullptr;
    size_t len = 0;
    return read(tag, content, len);
}

bool derIntegerToBytes(const uint8_t* p, size_t n, Bytes& out) {
    if (n == 0) return false;
    if ((p[0] & 0x80) != 0) return false;   // 负数
    size_t i = 0;
    while (i + 1 < n && p[i] == 0x00) ++i;   // 去前导零
    out.assign(p + i, p + n);
    return true;
}

bool derIntegerToInt64(const uint8_t* p, size_t n, int64_t& out) {
    Bytes b;
    if (!derIntegerToBytes(p, n, b)) return false;
    if (b.size() > 8) return false;
    int64_t v = 0;
    for (uint8_t byte : b) {
        v = (v << 8) | static_cast<int64_t>(byte);
    }
    out = v;
    return true;
}

bool derTimeToUnix(uint8_t tag, const uint8_t* p, size_t n, int64_t& out) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (tag == DER_UTC_TIME) {
        if (n < 11) return false;
        int yy = 0;
        if (!parseDigits(p, 2, yy) || !parseDigits(p + 2, 2, month) || !parseDigits(p + 4, 2, day) ||
            !parseDigits(p + 6, 2, hour) || !parseDigits(p + 8, 2, minute)) {
            return false;
        }
        if (n >= 13 && p[10] != 'Z') {
            if (!parseDigits(p + 10, 2, second)) return false;
            if (n < 13 || p[12] != 'Z') return false;
        }
        year = (yy < 50) ? 2000 + yy : 1900 + yy;
    } else if (tag == DER_GENERALIZED_TIME) {
        if (n < 13) return false;
        if (!parseDigits(p, 4, year) || !parseDigits(p + 4, 2, month) || !parseDigits(p + 6, 2, day) ||
            !parseDigits(p + 8, 2, hour) || !parseDigits(p + 10, 2, minute)) {
            return false;
        }
        if (n >= 15) {
            if (!parseDigits(p + 12, 2, second)) return false;
            if (p[14] != 'Z') return false;
        } else if (p[12] != 'Z') {
            return false;
        }
    } else {
        return false;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    out = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
          hour * 3600 + minute * 60 + second;
    return true;
}

std::string derOidToString(const uint8_t* p, size_t n) {
    if (n == 0) return std::string();
    std::string out;
    const unsigned first = p[0];
    out += std::to_string(first / 40);
    out += '.';
    out += std::to_string(first % 40);
    uint64_t value = 0;
    for (size_t i = 1; i < n; ++i) {
        value = (value << 7) | static_cast<uint64_t>(p[i] & 0x7f);
        if ((p[i] & 0x80) == 0) {
            out += '.';
            out += std::to_string(value);
            value = 0;
        }
    }
    return out;
}

bool derForEachExtension(const uint8_t* p, size_t n,
                         bool (*fn)(const std::string& oid, bool critical, const uint8_t* value, size_t valueLen,
                                    void* user),
                         void* user) {
    DerReader ext(p, n);
    while (!ext.atEnd()) {
        const uint8_t* extSeq = nullptr;
        size_t extSeqLen = 0;
        if (!ext.readExpected(DER_SEQUENCE, extSeq, extSeqLen)) return false;

        DerReader one(extSeq, extSeqLen);
        const uint8_t* oidP = nullptr;
        size_t oidLen = 0;
        if (!one.readExpected(DER_OID, oidP, oidLen)) return false;
        const std::string oid = derOidToString(oidP, oidLen);

        bool critical = false;
        uint8_t tag = 0;
        if (!one.peek(tag)) return false;
        if (tag == DER_BOOLEAN) {
            const uint8_t* boolP = nullptr;
            size_t boolLen = 0;
            if (!one.readExpected(DER_BOOLEAN, boolP, boolLen)) return false;
            if (boolLen != 1) return false;
            critical = (boolP[0] != 0x00);
        }

        const uint8_t* valP = nullptr;
        size_t valLen = 0;
        if (!one.readExpected(DER_OCTET_STRING, valP, valLen)) return false;

        if (!fn(oid, critical, valP, valLen, user)) break;
    }
    return ext.ok;
}

} // namespace jc2::crypto
