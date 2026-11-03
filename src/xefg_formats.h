// xefg_formats.h - which DXGI formats the own generator takes for the tagged motion vectors and depth, and how (xefg_own.h CopyIn,
// xefg_convert.hlsl). Host-only (no GPU, no other header of ours), so tests\convert_test.cpp checks the table itself.
//
// The table follows what Intel's library does with the same tag, measured with tests\repro_test.cpp (REPRO_MVFMT / REPRO_DEPTHFMT,
// F5LOW_GENERATOR=intel, the moving-bar probe; docs\GENERATORS.md "Motion and depth formats"): Intel reads the motion texture through
// a typed view as two floats, so every format a float view decodes works (typeless ones as float, UNORM / SNORM scaled to [0, 1] /
// [-1, 1], R11G11B10_FLOAT) with the motion scale the application tags, and the integer formats (UINT, SINT) are taken without
// error but the integers are read as float bit patterns: wrong motion. Intel never refuses a format at the tag (TagFrameResource
// returns success even for BC1); we refuse the integer ones, because a game cannot mean what they would then produce.
//
// Depth is read the same way: every depth format of a game works (D32_FLOAT, R32_FLOAT / TYPELESS, D24S8, D16, R16_FLOAT / UNORM,
// D32S8, R32G8X24); an integer or colour format does not name a depth.
#pragma once

#include <cstdint>
#include <dxgiformat.h>

namespace xefgown
{
// What the own generator does with a tagged motion / depth texture.
enum class Take
{
    No,         // refused: no frames are generated while it is tagged (xefg_own.h logs why and tells XeLL generation is off)
    Raw,        // already the pack's layout (motion R16G16_FLOAT, depth 32-bit float): copied as it is
    Convert,    // copied raw and turned into the pack's layout on our list (xefg_convert.hlsl, kind and stride below)
};

// Bytes per texel of the formats copied as they are (the pack's own buffers and the 4-byte colour formats of the HUD-less colour
// and UI texture). 0: not one of them. R16G16_UNORM is not here: it is not the pack's R16G16_FLOAT, it goes through Convert.
inline uint32_t TexelBytesOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case 27: case 28: case 29:                 // R8G8B8A8
    case 87: case 90: case 91:                 // B8G8R8A8 (HUD-less colour / UI texture of a B8G8R8A8 swap chain)
    case 23: case 24: case 25:                 // R10G10B10A2 (HDR10)
    case 33: case 34:                          // R16G16 typeless / float (motion)
    case 39: case 40: case 41:                 // R32 (depth)
    case 19: case 20: case 21:                 // R32G8X24, plane 0 is the 32-bit float
        return 4;
    default: return 0;
    }
}

// The motion vectors. kind as xefg_convert.hlsl's Kind (all of them write the pack's R16G16_FLOAT); stride: bytes per texel of the
// raw copy. The first two channels are the vector; the motion scale the application tags multiplies it afterwards, as with Intel.
inline Take MotionTake(DXGI_FORMAT f, uint32_t& kind, uint32_t& stride)
{
    kind = 0;
    stride = 0;
    switch (f)
    {
    case 33: case 34: return Take::Raw;                             // R16G16 typeless / float
    case 9: case 10: kind = 0; stride = 8; return Take::Convert;    // R16G16B16A16 typeless / float: the first two halves
    case 15: case 16: kind = 1; stride = 8; return Take::Convert;   // R32G32 typeless / float
    case 5: case 6: kind = 1; stride = 12; return Take::Convert;    // R32G32B32 typeless / float
    case 1: case 2: kind = 1; stride = 16; return Take::Convert;    // R32G32B32A32 typeless / float
    case 35: kind = 6; stride = 4; return Take::Convert;            // R16G16_UNORM
    case 11: kind = 6; stride = 8; return Take::Convert;            // R16G16B16A16_UNORM
    case 37: kind = 7; stride = 4; return Take::Convert;            // R16G16_SNORM
    case 13: kind = 7; stride = 8; return Take::Convert;            // R16G16B16A16_SNORM
    case 48: case 49: kind = 8; stride = 2; return Take::Convert;   // R8G8 typeless / UNORM
    case 27: case 28: kind = 8; stride = 4; return Take::Convert;   // R8G8B8A8 typeless / UNORM (not the sRGB one: a typed read would decode gamma)
    case 51: kind = 9; stride = 2; return Take::Convert;            // R8G8_SNORM
    case 31: kind = 9; stride = 4; return Take::Convert;            // R8G8B8A8_SNORM
    case 23: case 24: kind = 10; stride = 4; return Take::Convert;  // R10G10B10A2 typeless / UNORM
    case 26: kind = 11; stride = 4; return Take::Convert;           // R11G11B10_FLOAT
    default: return Take::No;                                       // integer (UINT / SINT), sRGB, one channel, depth, compressed, ...
    }
}

// The depth. kind 2..4 as xefg_convert.hlsl's Kind (to 32-bit float). Where a texel is not 4 bytes of float the raw copy has the
// plane 0 / texel size in stride.
inline Take DepthTake(DXGI_FORMAT f, uint32_t& kind, uint32_t& stride)
{
    kind = 0;
    stride = 0;
    switch (f)
    {
    case 39: case 40: case 41: return Take::Raw;                    // R32_TYPELESS / D32_FLOAT / R32_FLOAT
    case 19: case 20: case 21: return Take::Raw;                    // R32G8X24_TYPELESS / D32_FLOAT_S8X24_UINT: plane 0 is the float
    case 44: case 45: case 46: kind = 2; stride = 4; return Take::Convert; // R24G8_TYPELESS / D24_UNORM_S8_UINT, plane 0
    case 53: case 55: case 56: kind = 3; stride = 2; return Take::Convert; // R16_TYPELESS / D16_UNORM / R16_UNORM
    case 54: kind = 4; stride = 2; return Take::Convert;            // R16_FLOAT
    default: return Take::No;
    }
}

// Whether the motion vectors / depth the application tags can be used, decided once per frame at Present (not per tag: a frame may tag
// twice, and alternating good and bad tags must not flip XeLL's frame generation on and off every frame). `tagged`: the frame tagged
// motion or depth at all; `bad`: one of them was tagged and none of that kind was taken. A bad frame refuses at once; a refused state
// ends after kGoodFramesToRecover frames with tags that were all usable (the rule of lastUsable: one good frame is not trusted), frames
// without tags change nothing. Returns whether the inputs are refused after this frame.
inline constexpr int kGoodFramesToRecover = 2;
struct InputsState
{
    bool refused = false;
    int good = 0;
};
inline bool StepInputs(InputsState& s, bool tagged, bool bad)
{
    if (bad)
    {
        s.refused = true;
        s.good = 0;
    }
    else if (tagged && s.refused && ++s.good >= kGoodFramesToRecover)
    {
        s.refused = false;
        s.good = 0;
    }
    return s.refused;
}

// The name of a format in a log line (the ones a motion vector or depth texture can plausibly be, else the number alone).
inline const char* FormatName(DXGI_FORMAT f)
{
    switch (static_cast<int>(f))
    {
    case 1: return "R32G32B32A32_TYPELESS";   case 2: return "R32G32B32A32_FLOAT";   case 3: return "R32G32B32A32_UINT";
    case 4: return "R32G32B32A32_SINT";       case 5: return "R32G32B32_TYPELESS";   case 6: return "R32G32B32_FLOAT";
    case 7: return "R32G32B32_UINT";          case 8: return "R32G32B32_SINT";
    case 9: return "R16G16B16A16_TYPELESS";   case 10: return "R16G16B16A16_FLOAT";  case 11: return "R16G16B16A16_UNORM";
    case 12: return "R16G16B16A16_UINT";      case 13: return "R16G16B16A16_SNORM";  case 14: return "R16G16B16A16_SINT";
    case 15: return "R32G32_TYPELESS";        case 16: return "R32G32_FLOAT";        case 17: return "R32G32_UINT";
    case 18: return "R32G32_SINT";            case 19: return "R32G8X24_TYPELESS";   case 20: return "D32_FLOAT_S8X24_UINT";
    case 21: return "R32_FLOAT_X8X24_TYPELESS";
    case 23: return "R10G10B10A2_TYPELESS";   case 24: return "R10G10B10A2_UNORM";   case 25: return "R10G10B10A2_UINT";
    case 26: return "R11G11B10_FLOAT";        case 27: return "R8G8B8A8_TYPELESS";   case 28: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";    case 30: return "R8G8B8A8_UINT";       case 31: return "R8G8B8A8_SNORM";
    case 32: return "R8G8B8A8_SINT";          case 33: return "R16G16_TYPELESS";     case 34: return "R16G16_FLOAT";
    case 35: return "R16G16_UNORM";           case 36: return "R16G16_UINT";         case 37: return "R16G16_SNORM";
    case 38: return "R16G16_SINT";            case 39: return "R32_TYPELESS";        case 40: return "D32_FLOAT";
    case 41: return "R32_FLOAT";              case 42: return "R32_UINT";            case 43: return "R32_SINT";
    case 44: return "R24G8_TYPELESS";         case 45: return "D24_UNORM_S8_UINT";   case 46: return "R24_UNORM_X8_TYPELESS";
    case 48: return "R8G8_TYPELESS";          case 49: return "R8G8_UNORM";          case 50: return "R8G8_UINT";
    case 51: return "R8G8_SNORM";             case 52: return "R8G8_SINT";           case 53: return "R16_TYPELESS";
    case 54: return "R16_FLOAT";              case 55: return "D16_UNORM";           case 56: return "R16_UNORM";
    case 57: return "R16_UINT";               case 58: return "R16_SNORM";           case 59: return "R16_SINT";
    case 60: return "R8_TYPELESS";            case 61: return "R8_UNORM";
    default: return "";
    }
}
}
