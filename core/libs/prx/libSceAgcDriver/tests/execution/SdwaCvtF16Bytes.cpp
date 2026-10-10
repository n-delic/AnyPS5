#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::array<std::uint32_t, 2> Opcodes{0x50u, 0x51u};
constexpr std::uint32_t ByteSelectors = 4;
constexpr std::array<std::uint32_t, 2> DestinationSelectors{4u, 5u};
constexpr std::uint32_t UnusedModes = 3;
constexpr std::uint32_t Combinations = static_cast<std::uint32_t>(Opcodes.size()) * ByteSelectors * 2u * static_cast<std::uint32_t>(DestinationSelectors.size()) * UnusedModes;
constexpr std::uint32_t MaxLanes = 64;
constexpr std::uint32_t Inputs = 2;
constexpr std::uint32_t Results = Combinations;
constexpr std::uint32_t InputIndexVector = 1;
constexpr std::uint32_t OutputIndexVector = 2;
constexpr std::uint32_t SourceVector = 4;
constexpr std::uint32_t PreviousVector = 5;
constexpr std::uint32_t DataVector = 6;
constexpr std::uint32_t ThreadVector = 0;
constexpr std::uint32_t InputRegister = 0;
constexpr std::uint32_t OutputRegister = 4;
constexpr std::uint32_t ConstantZero = 0x80u;
constexpr std::uint32_t ConstantOne = 0x81u;
constexpr std::uint32_t Literal = 0xffu;
constexpr std::uint32_t Sdwa = 0xf9u;
constexpr std::uint32_t VectorSource = 0x100u;
constexpr std::uint32_t Endpgm = 0xbf810000u;
constexpr std::size_t CodeWords = 8u + Combinations * 5u + 1u;

constexpr std::array<std::uint32_t, 32> Sources{
    0x00000000u, 0xffffffffu, 0x80808080u, 0x7f7f7f7fu, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u,
    0x00000080u, 0x00008000u, 0x00800000u, 0x80000000u, 0x0000007fu, 0x00007fffu, 0x007fffffu, 0x7fffffffu,
    0x01020384u, 0x8504f306u, 0x12345678u, 0x9abcdef0u, 0xfedcba98u, 0x76543210u, 0x00ff00ffu, 0xff00ff00u,
    0x80007fffu, 0x7fff8000u, 0xc3a5815au, 0x5a81a5c3u, 0x0f1e2d3cu, 0xf0e1d2c3u, 0x40c08001u, 0xdeadbeefu,
};

alignas(256) std::array<std::uint32_t, MaxLanes * Inputs> Input{};
alignas(256) std::array<std::uint32_t, MaxLanes * Results> Output{};

constexpr std::uint32_t Vop1(std::uint32_t opcode, std::uint32_t destination, std::uint32_t source) {
    return 0x7e000000u | (destination << 17u) | (opcode << 9u) | source;
}

constexpr std::uint32_t Vop2(std::uint32_t opcode, std::uint32_t destination, std::uint32_t source0, std::uint32_t source1) {
    return (opcode << 25u) | (destination << 17u) | (source1 << 9u) | source0;
}

constexpr std::uint32_t SdwaModifier(std::uint32_t source, std::uint32_t destinationSelector, std::uint32_t unused, std::uint32_t sourceSelector, std::uint32_t signExtend) {
    return source | (destinationSelector << 8u) | (unused << 11u) | (sourceSelector << 16u) | (signExtend << 19u);
}

constexpr std::uint32_t BufferAccess(std::uint32_t data, std::uint32_t index, std::uint32_t descriptor) {
    return (ConstantZero << 24u) | ((descriptor / 4u) << 16u) | (data << 8u) | index;
}

constexpr std::uint32_t CombinationIndex(std::uint32_t op, std::uint32_t sourceSelector, std::uint32_t signExtend, std::uint32_t destination, std::uint32_t unused) {
    return (((op * ByteSelectors + sourceSelector) * 2u + signExtend) * static_cast<std::uint32_t>(DestinationSelectors.size()) + destination) * UnusedModes + unused;
}

constexpr std::array<std::uint32_t, CodeWords> BuildCode() {
    constexpr std::uint32_t vMovB32 = 0x01u;
    constexpr std::uint32_t vMulU32U24 = 0x0bu;
    constexpr std::uint32_t vLshlrevB32 = 0x1au;
    constexpr std::uint32_t bufferLoadDwordIndexed = 0xe0302000u;
    constexpr std::uint32_t bufferStoreDwordIndexed = 0xe0702000u;
    constexpr std::uint32_t waitVmcnt = 0xbf8c3f70u;
    std::array<std::uint32_t, CodeWords> code{};
    std::size_t count = 0;
    code[count++] = Vop2(vLshlrevB32, InputIndexVector, ConstantOne, ThreadVector);
    code[count++] = Vop2(vMulU32U24, OutputIndexVector, Literal, ThreadVector);
    code[count++] = Results;
    code[count++] = bufferLoadDwordIndexed;
    code[count++] = BufferAccess(SourceVector, InputIndexVector, InputRegister);
    code[count++] = bufferLoadDwordIndexed | 4u;
    code[count++] = BufferAccess(PreviousVector, InputIndexVector, InputRegister);
    code[count++] = waitVmcnt;
    for (std::uint32_t op = 0; op < Opcodes.size(); ++op) {
        for (std::uint32_t sourceSelector = 0; sourceSelector < ByteSelectors; ++sourceSelector) {
            for (std::uint32_t signExtend = 0; signExtend < 2u; ++signExtend) {
                for (std::uint32_t destination = 0; destination < DestinationSelectors.size(); ++destination) {
                    for (std::uint32_t unused = 0; unused < UnusedModes; ++unused) {
                        code[count++] = Vop1(vMovB32, DataVector, VectorSource | PreviousVector);
                        code[count++] = Vop1(Opcodes[op], DataVector, Sdwa);
                        code[count++] = SdwaModifier(SourceVector, DestinationSelectors[destination], unused, sourceSelector, signExtend);
                        code[count++] = bufferStoreDwordIndexed | (CombinationIndex(op, sourceSelector, signExtend, destination, unused) * 4u);
                        code[count++] = BufferAccess(DataVector, OutputIndexVector, OutputRegister);
                    }
                }
            }
        }
    }
    code[count++] = Endpgm;
    return code;
}

alignas(256) constexpr std::array<std::uint32_t, CodeWords> Code = BuildCode();

std::uint32_t Source(std::uint32_t tid) {
    return std::rotl(Sources[tid % 32u], static_cast<int>(tid / 32u) * 8);
}

std::uint32_t Previous(std::uint32_t tid) {
    return 0xabcd1234u ^ (tid * 0x01030507u);
}

void FillInput() {
    for (std::uint32_t tid = 0; tid < MaxLanes; ++tid) {
        Input[tid * Inputs] = Source(tid);
        Input[tid * Inputs + 1u] = Previous(tid);
    }
}

std::uint32_t SelectByte(std::uint32_t value, std::uint32_t selector, bool signExtend) {
    const std::uint32_t field = (value >> (selector * 8u)) & 0xffu;
    return signExtend && (field & 0x80u) != 0u ? field | 0xffffff00u : field;
}

std::uint32_t HalfBits(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const std::uint32_t magnitude = bits & 0x7fffffffu;
    if (magnitude == 0u) return sign;
    const int exponent = static_cast<int>(magnitude >> 23u) - 127 + 15;
    if (exponent >= 31) return sign | 0x7c00u;
    std::uint32_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
    std::uint32_t half;
    std::uint32_t shift;
    if (exponent <= 0) {
        shift = static_cast<std::uint32_t>(14 - exponent);
        if (shift > 24u) return sign;
        half = mantissa >> shift;
    } else {
        shift = 13u;
        half = (static_cast<std::uint32_t>(exponent) << 10u) | ((mantissa >> 13u) & 0x3ffu);
    }
    const std::uint32_t remainder = mantissa & ((1u << shift) - 1u);
    const std::uint32_t halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (half & 1u) != 0u)) ++half;
    return sign | half;
}

std::uint32_t Convert(std::uint32_t op, std::uint32_t selected) {
    const std::uint32_t low = selected & 0xffffu;
    if (Opcodes[op] == 0x50u) return HalfBits(static_cast<float>(low));
    return HalfBits(static_cast<float>(static_cast<std::int16_t>(low)));
}

std::uint32_t PlaceWord(std::uint32_t result, std::uint32_t selector, std::uint32_t unused, std::uint32_t previous) {
    const std::uint32_t offset = selector == 5u ? 16u : 0u;
    const std::uint32_t field = result & 0xffffu;
    switch (unused) {
        case 0u: return field << offset;
        case 1u: return ((field & 0x8000u) != 0u ? field | 0xffff0000u : field) << offset;
        default: return (previous & ~(0xffffu << offset)) | (field << offset);
    }
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x11016facu};
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize, const ShaderRecompiler::SpirvTarget& target) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(input.begin(), input.end(), userData.begin() + InputRegister);
    std::copy(output.begin(), output.end(), userData.begin() + OutputRegister);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{waveSize, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(std::uint32_t waveSize, const char* run) {
    constexpr const char* names[2] = {"v_cvt_f16_u16", "v_cvt_f16_i16"};
    for (std::uint32_t tid = 0; tid < waveSize; ++tid) {
        const std::uint32_t source = Source(tid);
        const std::uint32_t previous = Previous(tid);
        const auto* out = &Output[tid * Results];
        for (std::uint32_t op = 0; op < Opcodes.size(); ++op) {
            for (std::uint32_t sourceSelector = 0; sourceSelector < ByteSelectors; ++sourceSelector) {
                for (std::uint32_t signExtend = 0; signExtend < 2u; ++signExtend) {
                    const std::uint32_t converted = Convert(op, SelectByte(source, sourceSelector, signExtend != 0u));
                    for (std::uint32_t destination = 0; destination < DestinationSelectors.size(); ++destination) {
                        for (std::uint32_t unused = 0; unused < UnusedModes; ++unused) {
                            const std::uint32_t index = CombinationIndex(op, sourceSelector, signExtend, destination, unused);
                            const std::uint32_t expected = PlaceWord(converted, DestinationSelectors[destination], unused, previous);
                            Require(out[index] == expected, std::string("sdwa f16 conversion of a byte ") + run + ": lane " + std::to_string(tid) + " " + names[op] +
                                " src0_sel:BYTE_" + std::to_string(sourceSelector) + (signExtend != 0u ? " sext" : "") + " dst_sel:WORD_" + std::to_string(DestinationSelectors[destination] - 4u) +
                                " dst_unused:" + std::to_string(unused) + " source " + Hex(source) + " previous " + Hex(previous) + " result " + Hex(out[index]) + ", expected " + Hex(expected));
                        }
                    }
                }
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        FillInput();
        Run(*device, 32, device->Target());
        Check(32, "wave32");
        Run(*device, 64, device->Target());
        Check(64, "wave64");
        std::puts("sdwa f16 conversion of a byte tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
