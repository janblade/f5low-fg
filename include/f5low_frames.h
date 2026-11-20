// f5low_frames.h - "frames only" API of f5low.dll: the XeFG Tensor generator without a swapchain.
//
// The host (OptiScaler's Vulkan output, or a test) owns presenting and pacing. f5low.dll only turns two app frames
// into 1..5 generated frames between them, on the host's D3D12 device. A Vulkan host shares its images with that
// device (D3D12-created shared resources imported into Vulkan) and syncs through shared D3D12 fences (imported as
// Vulkan timeline semaphores).
//
// Threading: one context is used from one host thread at a time. Every call returns without waiting on the GPU,
// except Destroy and Resize (they wait until the context's GPU work is done) and a Generate that has to rebuild the
// generator for a new render size or new depth/motion flags (see F5LowFramesInput; about 0.2 s, once per change).
//
// Version 1. The generator is always f5low's own (XeFG Tensor); the frames are built from the player's
// libxess_fg.dll found as for the XeFG API (XEFG_CUDA_INTEL_LIBRARY), in memory, in the background.
#pragma once

#include <stdint.h>

#include <d3d12.h>
#include <dxgiformat.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define F5LOW_FRAMES_API_VERSION 1u
#define F5LOW_FRAMES_MAX_GENERATED 5u

#ifdef F5LOW_FRAMES_EXPORTS
#define F5LOW_FRAMES_API __declspec(dllexport)
#else
#define F5LOW_FRAMES_API __declspec(dllimport)
#endif

typedef struct F5LowFramesContext_T* F5LowFramesContext;

typedef enum F5LowFramesResult
{
    F5LOW_FRAMES_OK = 0,
    F5LOW_FRAMES_NOT_READY = 1,          // the generator is still being built: Generate made nothing, try next frame
    F5LOW_FRAMES_ERROR_ARGUMENT = -1,    // a null or out-of-range field; f5lowFramesLastError says which
    F5LOW_FRAMES_ERROR_UNSUPPORTED = -2, // this GPU/driver/format cannot run it (not NVIDIA, colour format, version)
    F5LOW_FRAMES_ERROR_BUILD = -3,       // building the generator failed for good (libxess_fg.dll missing or unknown)
    F5LOW_FRAMES_ERROR_DEVICE = -4,      // a D3D12 call failed (device removed included)
    F5LOW_FRAMES_ERROR_INTERNAL = -5,
} F5LowFramesResult;

// F5LowFramesCreateInfo::flags
#define F5LOW_FRAMES_FLAG_HDR10 0x1u // colour is R10G10B10A2 in HDR10 (PQ); else SDR
// Where the first frames will come from, so the generator is built right away for them (each Generate may still differ: f5low then
// rebuilds). Without them the first Generate with other values waits for a rebuild.
#define F5LOW_FRAMES_FLAG_DEPTH_INVERTED 0x2u  // F5LowFramesInput::depthInverted will be 1
#define F5LOW_FRAMES_FLAG_MOTION_JITTERED 0x4u // F5LowFramesInput::motionJittered will be 1

typedef struct F5LowFramesCreateInfo
{
    uint32_t apiVersion;         // F5LOW_FRAMES_API_VERSION
    ID3D12Device* device;        // the host's device; f5low takes a reference and makes its own queue(s) on it
    uint32_t width;              // size of colour and of the generated frames (the display size)
    uint32_t height;
    DXGI_FORMAT colorFormat;     // R8G8B8A8_UNORM(_SRGB), B8G8R8A8_UNORM(_SRGB) or R10G10B10A2_UNORM
    uint32_t maxGenerated;       // 1..F5LOW_FRAMES_MAX_GENERATED, the most a Generate call will ask for
    uint32_t flags;              // F5LOW_FRAMES_FLAG_*
    uint32_t renderWidth;        // size of the motion vectors the first frames will have (0: the display size)
    uint32_t renderHeight;
} F5LowFramesCreateInfo;

// One app frame. All resources are on CreateInfo::device and in D3D12_RESOURCE_STATE_COMMON; f5low only reads them,
// from its own queue, after readyFence reaches readyValue, and signals consumedFence = consumedValue when it no longer
// needs them (the host may then overwrite them). f5low keeps its own copy of what it needs for the next pair.
typedef struct F5LowFramesInput
{
    ID3D12Resource* color;       // the finished picture (with HUD), CreateInfo size and colorFormat
    ID3D12Resource* hudless;     // optional (nullptr): the picture without HUD, same size and format
    ID3D12Resource* motion;      // motion vectors, R16G16_FLOAT or R32G32_FLOAT (others: ERROR_UNSUPPORTED)
    uint32_t motionWidth;        // used area of motion (render or display size). A size other than the one the generator was built
    uint32_t motionHeight;       // for (CreateInfo render size) rebuilds it: that Generate waits for the GPU, takes history only (count 0).
    ID3D12Resource* depth;       // depth, R32_FLOAT, D32_FLOAT, R16_UNORM or D24/R24 typeless family
    uint32_t depthWidth;
    uint32_t depthHeight;

    float motionScale[2];        // pixels = value * scale (in motion's own size; f5low scales to colour size)
    float jitter[2];             // this frame's jitter in render pixels (0 when unknown)
    float cameraNear;
    float cameraFar;
    float cameraFovY;            // radians, 0 when unknown
    uint32_t depthInverted;      // 1: near = 1.  These two pick the generator's variant: a value other than the one it was built for
    uint32_t motionJittered;     // 1: motion includes the jitter.  rebuilds it like a new motionWidth/Height.
    uint32_t reset;              // 1: camera cut, no history (Generate then makes no frames, returns OK, count 0)
    uint64_t frameId;            // increasing by 1 per app frame; a gap counts as a reset

    ID3D12Fence* readyFence;     // waited (GPU side) before reading the inputs; nullptr: the inputs are ready already
    uint64_t readyValue;
    ID3D12Fence* consumedFence;  // signalled when the inputs were read
    uint64_t consumedValue;
} F5LowFramesInput;

// Where the generated frames go: textures on CreateInfo::device, CreateInfo size and colorFormat, COMMON state,
// with ALLOW_UNORDERED_ACCESS or plain copy-dest use (f5low copies into them). targets[k] is the frame at
// t = (k + 1) / (count + 1) between the previous Generate's input and this one's.
typedef struct F5LowFramesOutput
{
    uint32_t count;              // 0..maxGenerated (0: only take the input as history)
    ID3D12Resource* targets[F5LOW_FRAMES_MAX_GENERATED];
    ID3D12Fence* doneFence;      // signalled when every target is written
    uint64_t doneValue;
    uint32_t generated;          // out: how many targets were written (0 on the first frame, a reset or NOT_READY)
} F5LowFramesOutput;

F5LOW_FRAMES_API F5LowFramesResult f5lowFramesCreate(const F5LowFramesCreateInfo* info, F5LowFramesContext* out);

// OK when the generator is built, NOT_READY while building, ERROR_BUILD when that failed.
F5LOW_FRAMES_API F5LowFramesResult f5lowFramesBuildStatus(F5LowFramesContext context);

// Records and submits the work (on f5low's own queue: it waits for readyFence there, the host's queues never wait for f5low
// except through the fences it chooses to wait on). The generator builds the generated frames at t = (k + 1) / (count + 1) of the
// distance between the previous Generate's picture and this one's.
// consumedFence and doneFence are signalled on every return except ERROR_ARGUMENT for a null pointer in input/output/consumedFence/doneFence
// (nothing is submitted then), also on NOT_READY and on errors: a host may always wait on them. On an error the targets hold nothing.
F5LOW_FRAMES_API F5LowFramesResult f5lowFramesGenerate(F5LowFramesContext context, const F5LowFramesInput* input,
                                                     F5LowFramesOutput* output);

// New colour size or format (window resize): waits idle, drops the history.
F5LOW_FRAMES_API F5LowFramesResult f5lowFramesResize(F5LowFramesContext context, uint32_t width, uint32_t height,
                                                   DXGI_FORMAT colorFormat, uint32_t flags);

// Waits idle, releases everything. Null is fine.
F5LOW_FRAMES_API void f5lowFramesDestroy(F5LowFramesContext context);

// The last error's text for this context (or for Create when context is null), UTF-8, valid until the next call.
F5LOW_FRAMES_API const char* f5lowFramesLastError(F5LowFramesContext context);

#ifdef __cplusplus
}
#endif
