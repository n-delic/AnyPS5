#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

// No network is emulated: contexts, templates and requests can be created, but any request
// that would touch the network fails with the library's network error.
static constexpr int ERROR_NETWORK = static_cast<int>(0x80435001);
static std::atomic<int> g_nextHandle{1};
static std::mutex g_contextsMutex;
static std::unordered_map<int, std::uint64_t> g_contextPoolSizes;

namespace {

constexpr int ERROR_NOT_FOUND = static_cast<int>(0x8095F004);
constexpr int ERROR_INVALID_ARG = static_cast<int>(0x8095177A);

struct SslData {
    char* ptr;
    size_t size;
};

struct SslCaCerts {
    SslData* certs;
    size_t num;
    void* pool;
};

}

extern "C" {

int APS5_VABI sceSslFreeCaCerts(int ssl_ctx_id, void* ca_certs) {
    (void)ssl_ctx_id;
    if (!ca_certs) return ERROR_INVALID_ARG;
    *static_cast<SslCaCerts*>(ca_certs) = {};
    return 0;
}

int APS5_VABI sceSslGetCaCerts(int ssl_ctx_id, void* ca_certs) {
    (void)ssl_ctx_id;
    if (!ca_certs) return ERROR_INVALID_ARG;
    *static_cast<SslCaCerts*>(ca_certs) = {};
    return ERROR_NOT_FOUND;
}

int APS5_VABI sceSslInit_nid_postfix(uint64_t pool_size) {
    const int id = g_nextHandle.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_contextsMutex);
    g_contextPoolSizes[id] = pool_size;
    return id;
}

int APS5_VABI sceSslTerm_nid_postfix(int ssl_ctx_id) {
    std::lock_guard lock(g_contextsMutex);
    g_contextPoolSizes.erase(ssl_ctx_id);
    return 0;
}

int APS5_VABI sceSslClose() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSslGetSerialNumber() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSslLoadCert() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetMemoryPoolStats(int ssl_ctx_id, MemoryPoolStats* current_stat) {
    if (current_stat == nullptr) throw std::invalid_argument("sceSslGetMemoryPoolStats: current_stat is null");
    std::lock_guard lock(g_contextsMutex);
    const auto found = g_contextPoolSizes.find(ssl_ctx_id);
    if (found == g_contextPoolSizes.end()) throw std::invalid_argument("sceSslGetMemoryPoolStats: unknown context id " + std::to_string(ssl_ctx_id));
    *current_stat = MemoryPoolStats{found->second, 0, 0};
    return 0;
}

int APS5_VABI sceSslFreeSslCertName(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetIssuerName(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetNameEntryCount(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetNameEntryInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetPem(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetSubjectName(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
