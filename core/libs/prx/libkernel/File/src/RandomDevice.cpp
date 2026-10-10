#include "prx/libkernel/File/include/RandomDevice.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint16_t GuestCharacterDevice = 0020000;
constexpr std::uint16_t GuestReadWriteAll = 0666;

std::mutex g_mutex;
std::set<int> g_descriptors;

}

namespace File {

bool IsRandomDevicePath(const char* path) {
    if (path == nullptr) return false;
    const std::string_view guest(path);
    return guest == "/dev/urandom" || guest == "/dev/random";
}

int OpenRandomDevice() {
#ifdef _WIN32
    const int fd = ::_open("NUL", _O_RDWR | _O_BINARY);
#else
    const int fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
#endif
    if (fd < 0) return -1;
    std::lock_guard lock(g_mutex);
    g_descriptors.insert(fd);
    return fd;
}

bool IsRandomDevice(int fd) {
    std::lock_guard lock(g_mutex);
    return g_descriptors.contains(fd);
}

void ForgetRandomDevice(int fd) {
    std::lock_guard lock(g_mutex);
    g_descriptors.erase(fd);
}

void ReadRandomDevice(void* buffer, std::size_t size) {
    static std::mutex sourceMutex;
    static std::random_device source;
    std::lock_guard lock(sourceMutex);
    auto* out = static_cast<unsigned char*>(buffer);
    while (size > 0) {
        const auto word = static_cast<std::uint32_t>(source());
        const std::size_t chunk = size < sizeof(word) ? size : sizeof(word);
        std::memcpy(out, &word, chunk);
        out += chunk;
        size -= chunk;
    }
}

void FillRandomDeviceStat(FileStat* sb) {
    *sb = FileStat{};
    sb->st_mode = GuestCharacterDevice | GuestReadWriteAll;
    sb->st_nlink = 1;
    sb->st_blksize = 4096;
}

}
