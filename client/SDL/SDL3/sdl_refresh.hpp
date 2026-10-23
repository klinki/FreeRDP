/** Manual refresh methods. Copyright 2026 FreeRDP contributors. Apache-2.0. */
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

enum class SdlRefreshMode : unsigned { Local, Textures, Server, Updates, Combined };
inline constexpr std::array<SdlRefreshMode, 5> sdlRefreshModes{
    SdlRefreshMode::Local, SdlRefreshMode::Textures, SdlRefreshMode::Server,
    SdlRefreshMode::Updates, SdlRefreshMode::Combined
};
inline constexpr uint32_t sdlRefreshBit(SdlRefreshMode mode)
{
    return 1U << static_cast<unsigned>(mode);
}
inline constexpr const char* sdlRefreshName(SdlRefreshMode mode)
{
    switch (mode)
    {
        case SdlRefreshMode::Local: return "local";
        case SdlRefreshMode::Textures: return "textures";
        case SdlRefreshMode::Server: return "server";
        case SdlRefreshMode::Updates: return "updates";
        case SdlRefreshMode::Combined: return "combined";
    }
    return "invalid";
}
inline bool sdlParseRefreshMode(std::string_view text, SdlRefreshMode& result)
{
    for (const auto mode : sdlRefreshModes)
        if (text == sdlRefreshName(mode)) { result = mode; return true; }
    return false;
}
