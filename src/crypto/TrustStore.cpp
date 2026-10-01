#include "TrustStore.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

namespace jc2::crypto {

const std::vector<Bytes>& systemRootCertificates() {
    static const std::vector<Bytes> roots = [] {
        std::vector<Bytes> out;
#if defined(_WIN32)
        HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
        if (store == nullptr) return out;
        PCCERT_CONTEXT ctx = nullptr;
        while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
            if (ctx->pbCertEncoded != nullptr && ctx->cbCertEncoded > 0) {
                out.emplace_back(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
            }
        }
        CertCloseStore(store, 0);
#endif
        return out;
    }();
    return roots;
}

} // namespace jc2::crypto
