#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_RANDOMDEVICE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_RANDOMDEVICE_HPP

#include <cstddef>
#include "SceTypes.hpp"

namespace File {

bool IsRandomDevicePath(const char* path);
int OpenRandomDevice();
bool IsRandomDevice(int fd);
void ForgetRandomDevice(int fd);
void ReadRandomDevice(void* buffer, std::size_t size);
void FillRandomDeviceStat(FileStat* sb);

}

#endif
