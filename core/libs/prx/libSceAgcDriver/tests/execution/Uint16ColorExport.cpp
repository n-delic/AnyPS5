#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 64;
constexpr std::uint32_t ScissorWidth = 128;
constexpr std::uint32_t PixelBytes = 8;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t SurfaceBytes = std::size_t{Width} * Height * PixelBytes;
constexpr std::uint32_t Uint16x4Info = (12u << 2u) | (4u << 8u);
constexpr std::uint32_t LinearAttrib3 = 0x09000000u;
constexpr std::uint32_t UintOutputMode = 7;
constexpr std::uint8_t Kept = 0x5a;
constexpr std::array<std::uint16_t, 4> Exported{0x0001u, 0x8002u, 0x7fffu, 0xffffu};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0x7e0802ffu, static_cast<std::uint32_t>(Exported[0]) | (static_cast<std::uint32_t>(Exported[1]) << 16u),
    0x7e0a02ffu, static_cast<std::uint32_t>(Exported[2]) | (static_cast<std::uint32_t>(Exported[3]) << 16u),
    0xf8001c0fu, 0x00000504u,
    0xbf810000u,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes), watched(AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (watched) {
            data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
            GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
        } else {
            data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            void* raw = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(raw != MAP_FAILED, "cannot map the color target");
            const auto begin = reinterpret_cast<std::uintptr_t>(raw);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(raw, aligned - begin);
            if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
        } else {
            data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
        }
#endif
        Require(data != nullptr, "cannot allocate the color target");
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true);
    }
    ~Block() {
        AgcDriver::Graphics::StorageTexture::FlushPending(Address(), bytes, nullptr, "test release");
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(data, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
        } else {
            VirtualFree(data, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(data, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        } else {
            std::free(data);
        }
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    bool watched;
    std::uint8_t* data = nullptr;
};

AgcDriver::Graphics::ColorTarget DecodeTarget(const Block& block) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(block.Address() >> 8u);
    cx[0x31b] = 0;
    cx[0x31c] = Uint16x4Info;
    cx[0x31d] = 0;
    cx[0x323] = 0;
    cx[0x324] = 0;
    cx[0x3b0] = ((Width - 1u) << 14u) | (Height - 1u);
    cx[0x3b8] = LinearAttrib3;
    cx[0x390] = static_cast<std::uint32_t>(block.Address() >> 40u);
    auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    color.exportIndex = 0;
    return color;
}

void Draw(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::ColorTarget& color) {
    const auto target = device.Target();
    constexpr std::uint32_t waveSize = 64;
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(Triangle.data(), 16u, static_cast<std::uint32_t>(Triangle.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode.at(0) = UintOutputMode;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {waveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, vertexPush, 128 - vertexPush}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = 0xf;
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, waveSize, waveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {ScissorWidth, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blends = {blend};
    state.blend = blend;
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(AgcDriver::Graphics::ColorTargetLayout(Width, Height, AgcDriver::Graphics::ColorTileMode::Linear, PixelBytes).Bytes() == SurfaceBytes, "the linear 16_16_16_16 target is padded");
        Block block(SurfaceBytes);
        std::memset(block.data, Kept, SurfaceBytes);
        const auto color = DecodeTarget(block);
        Require(color.format == VK_FORMAT_R16G16B16A16_UINT && color.elementBytes == PixelBytes, "the 16_16_16_16 UINT target did not decode as R16G16B16A16_UINT");
        Draw(*device, color);
        AgcDriver::Graphics::StorageTexture::FlushPending(block.Address(), SurfaceBytes, nullptr, "test read-back");
        device->WaitIdle();
        std::vector<std::uint8_t> stored(SurfaceBytes);
        AgcDriver::GuestMemory::Read(block.Address(), std::as_writable_bytes(std::span(stored)), 1);
        for (std::size_t offset = 0; offset < SurfaceBytes; ++offset) {
            const bool inside = (offset / PixelBytes) % Width < ScissorWidth;
            const auto channel = Exported[(offset % PixelBytes) / 2u];
            const std::uint8_t expected = inside ? static_cast<std::uint8_t>(channel >> ((offset % 2u) * 8u)) : Kept;
            Require(stored[offset] == expected, "UINT16_ABGR export into a 16_16_16_16 UINT target holds " + std::to_string(stored[offset]) + " at byte " + std::to_string(offset) + ", expected " + std::to_string(expected));
        }
        std::puts("UINT16_ABGR color export tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
