// ============================================================================
// Trails of Cold Steel III - Character Face HBAO+ Smudge Fix (dxgi.dll proxy)
// ============================================================================
// Eliminates HBAO+ depth-derivative & horizon-cone smudging on character faces,
// mouths, and eyeballs (including 8x MSAA + TSSAA) while keeping 100% of
// HBAO+ shading intact on hair, hats, glasses frames, clothing, and world.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// ----------------------------------------------------------------------------
// Configuration & Globals
// ----------------------------------------------------------------------------
struct Config {
    bool     enabled          = true;
    float    maskStrength     = 1.0f;
    float    depthEpsilon     = 0.000004f;
    int      edgeDilatePixels = 0;
    float    dilateDepthEps   = 0.00012f;
    bool     maskEyes         = true;
    bool     maskFaceParts    = true;
    int      toggleKey        = VK_F10; // 0x79
    int      reloadIniKey     = VK_F11; // 0x7A
    bool     toggleBeep       = true;
    bool     logging          = true;
    bool     visualizeMask    = false;
};

static Config  g_cfg;
static wchar_t g_dllDir[MAX_PATH]  = {};
static wchar_t g_iniPath[MAX_PATH] = {};
static wchar_t g_logPath[MAX_PATH] = {};
static SRWLOCK g_logLock           = SRWLOCK_INIT;

static void LogMsg(const char* fmt, ...) {
    if (!g_cfg.logging || g_logPath[0] == 0) return;
    AcquireSRWLockExclusive(&g_logLock);
    FILE* f = _wfopen(g_logPath, L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

static float ReadIniFloat(const wchar_t* section, const wchar_t* key, float defVal) {
    wchar_t buf[64] = {};
    wchar_t defBuf[64] = {};
    swprintf(defBuf, 64, L"%.7f", defVal);
    GetPrivateProfileStringW(section, key, defBuf, buf, 63, g_iniPath);
    wchar_t* endPtr = nullptr;
    float v = wcstof(buf, &endPtr);
    return (endPtr != buf) ? v : defVal;
}

static int ReadIniInt(const wchar_t* section, const wchar_t* key, int defVal) {
    wchar_t buf[64] = {};
    wchar_t defBuf[64] = {};
    swprintf(defBuf, 64, L"%d", defVal);
    GetPrivateProfileStringW(section, key, defBuf, buf, 63, g_iniPath);
    wchar_t* endPtr = nullptr;
    long v = wcstol(buf, &endPtr, 0); // supports both decimal and 0x hex
    return (endPtr != buf) ? (int)v : defVal;
}

static void LoadConfig() {
    if (g_iniPath[0] == 0) return;
    g_cfg.enabled          = ReadIniInt(L"HBAOFaceFix", L"Enabled", 1) != 0;
    g_cfg.maskStrength     = ReadIniFloat(L"HBAOFaceFix", L"MaskStrength", 1.0f);
    if (g_cfg.maskStrength < 0.0f) g_cfg.maskStrength = 0.0f;
    if (g_cfg.maskStrength > 1.0f) g_cfg.maskStrength = 1.0f;
    g_cfg.depthEpsilon     = ReadIniFloat(L"HBAOFaceFix", L"DepthEpsilon", 0.000004f);
    g_cfg.edgeDilatePixels = ReadIniInt(L"HBAOFaceFix", L"EdgeDilatePixels", 0);
    if (g_cfg.edgeDilatePixels < 0) g_cfg.edgeDilatePixels = 0;
    if (g_cfg.edgeDilatePixels > 2) g_cfg.edgeDilatePixels = 2;
    g_cfg.dilateDepthEps   = ReadIniFloat(L"HBAOFaceFix", L"DilateDepthEpsilon", 0.00012f);
    g_cfg.maskEyes         = ReadIniInt(L"HBAOFaceFix", L"MaskEyes", 1) != 0;
    g_cfg.maskFaceParts    = ReadIniInt(L"HBAOFaceFix", L"MaskFaceParts", 1) != 0;

    g_cfg.toggleKey        = ReadIniInt(L"Hotkeys", L"ToggleKey", VK_F10);
    g_cfg.reloadIniKey     = ReadIniInt(L"Hotkeys", L"ReloadIniKey", VK_F11);
    g_cfg.toggleBeep       = ReadIniInt(L"Hotkeys", L"ToggleBeep", 1) != 0;

    g_cfg.logging          = ReadIniInt(L"Debug", L"Logging", 1) != 0;
    g_cfg.visualizeMask    = ReadIniInt(L"Debug", L"VisualizeMask", 0) != 0;
}

static DWORD WINAPI BeepThreadProc(LPVOID param) {
    DWORD freq = (DWORD)(uintptr_t)param;
    Beep(freq, 45);
    return 0;
}

static void PlayAsyncBeep(DWORD freq) {
    if (!g_cfg.toggleBeep) return;
    HANDLE h = CreateThread(nullptr, 0, BeepThreadProc, (LPVOID)(uintptr_t)freq, 0, nullptr);
    if (h) CloseHandle(h);
}

// ----------------------------------------------------------------------------
// System32 dxgi.dll Proxy Forwarding (All 20 Windows 10/11 DXGI Exports)
// ----------------------------------------------------------------------------
enum DxgiExportSlot {
    SLOT_ApplyCompatResolutionQuirking = 0,
    SLOT_CompatString,
    SLOT_CompatValue,
    SLOT_CreateDXGIFactory,
    SLOT_CreateDXGIFactory1,
    SLOT_CreateDXGIFactory2,
    SLOT_DXGID3D10CreateDevice,
    SLOT_DXGID3D10CreateLayeredDevice,
    SLOT_DXGID3D10GetLayeredDeviceSize,
    SLOT_DXGID3D10RegisterLayers,
    SLOT_DXGIDeclareAdapterRemovalSupport,
    SLOT_DXGIDisableVBlankVirtualization,
    SLOT_DXGIDumpJournal,
    SLOT_DXGIGetDebugInterface1,
    SLOT_DXGIReportAdapterConfiguration,
    SLOT_PIXBeginCapture,
    SLOT_PIXEndCapture,
    SLOT_PIXGetCaptureState,
    SLOT_SetAppCompatStringPointer,
    SLOT_UpdateHMDEmulationStatus,
    SLOT_COUNT
};

static const char* const kDxgiExportNames[SLOT_COUNT] = {
    "ApplyCompatResolutionQuirking",
    "CompatString",
    "CompatValue",
    "CreateDXGIFactory",
    "CreateDXGIFactory1",
    "CreateDXGIFactory2",
    "DXGID3D10CreateDevice",
    "DXGID3D10CreateLayeredDevice",
    "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers",
    "DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization",
    "DXGIDumpJournal",
    "DXGIGetDebugInterface1",
    "DXGIReportAdapterConfiguration",
    "PIXBeginCapture",
    "PIXEndCapture",
    "PIXGetCaptureState",
    "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus"
};

extern "C" {
    void* g_dxgiProcs[SLOT_COUNT] = {};
}

static HMODULE   g_sysDxgiMod = nullptr;
static INIT_ONCE g_dxgiInitOnce = INIT_ONCE_STATIC_INIT;

static void InstallExeIatHooks();

static BOOL CALLBACK InitSystemDxgiCallback(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t fullPath[MAX_PATH] = {};
    swprintf(fullPath, MAX_PATH, L"%s\\dxgi.dll", sysDir);
    g_sysDxgiMod = LoadLibraryW(fullPath);
    if (g_sysDxgiMod) {
        for (int i = 0; i < SLOT_COUNT; ++i) {
            g_dxgiProcs[i] = (void*)GetProcAddress(g_sysDxgiMod, kDxgiExportNames[i]);
        }
        LogMsg("Loaded system dxgi.dll from %ls", fullPath);
    } else {
        LogMsg("ERROR: Failed to load system dxgi.dll from %ls (err=%lu)", fullPath, GetLastError());
    }
    InstallExeIatHooks();
    return TRUE;
}

extern "C" void EnsureSystemDxgi() {
    InitOnceExecuteOnce(&g_dxgiInitOnce, InitSystemDxgiCallback, nullptr, nullptr);
}

using PFN_CreateDXGIFactory  = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIGetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIDeclareAdapterRemovalSupport = HRESULT(WINAPI*)();

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    EnsureSystemDxgi();
    auto fn = (PFN_CreateDXGIFactory)g_dxgiProcs[SLOT_CreateDXGIFactory];
    return fn ? fn(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    EnsureSystemDxgi();
    auto fn = (PFN_CreateDXGIFactory1)g_dxgiProcs[SLOT_CreateDXGIFactory1];
    return fn ? fn(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    EnsureSystemDxgi();
    auto fn = (PFN_CreateDXGIFactory2)g_dxgiProcs[SLOT_CreateDXGIFactory2];
    return fn ? fn(Flags, riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    EnsureSystemDxgi();
    auto fn = (PFN_DXGIGetDebugInterface1)g_dxgiProcs[SLOT_DXGIGetDebugInterface1];
    return fn ? fn(Flags, riid, pDebug) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    EnsureSystemDxgi();
    auto fn = (PFN_DXGIDeclareAdapterRemovalSupport)g_dxgiProcs[SLOT_DXGIDeclareAdapterRemovalSupport];
    return fn ? fn() : S_OK;
}

#define DEFINE_DXGI_PASSTHROUGH(exportName, slotIdx) \
extern "C" __attribute__((naked)) void exportName() { \
    __asm__ __volatile__( \
        "movq g_dxgiProcs+" #slotIdx "*8(%%rip), %%rax\n\t" \
        "testq %%rax, %%rax\n\t" \
        "jz 1f\n\t" \
        "jmpq *%%rax\n\t" \
        "1:\n\t" \
        "subq $40, %%rsp\n\t" \
        "movq %%rcx, 48(%%rsp)\n\t" \
        "movq %%rdx, 56(%%rsp)\n\t" \
        "movq %%r8,  64(%%rsp)\n\t" \
        "movq %%r9,  72(%%rsp)\n\t" \
        "call EnsureSystemDxgi\n\t" \
        "movq 48(%%rsp), %%rcx\n\t" \
        "movq 56(%%rsp), %%rdx\n\t" \
        "movq 64(%%rsp), %%r8\n\t" \
        "movq 72(%%rsp), %%r9\n\t" \
        "addq $40, %%rsp\n\t" \
        "movq g_dxgiProcs+" #slotIdx "*8(%%rip), %%rax\n\t" \
        "testq %%rax, %%rax\n\t" \
        "jz 2f\n\t" \
        "jmpq *%%rax\n\t" \
        "2:\n\t" \
        "xorl %%eax, %%eax\n\t" \
        "retq\n\t" \
        ::: "memory" \
    ); \
}

DEFINE_DXGI_PASSTHROUGH(ApplyCompatResolutionQuirking, 0)
DEFINE_DXGI_PASSTHROUGH(CompatString, 1)
DEFINE_DXGI_PASSTHROUGH(CompatValue, 2)
DEFINE_DXGI_PASSTHROUGH(DXGID3D10CreateDevice, 6)
DEFINE_DXGI_PASSTHROUGH(DXGID3D10CreateLayeredDevice, 7)
DEFINE_DXGI_PASSTHROUGH(DXGID3D10GetLayeredDeviceSize, 8)
DEFINE_DXGI_PASSTHROUGH(DXGID3D10RegisterLayers, 9)
DEFINE_DXGI_PASSTHROUGH(DXGIDisableVBlankVirtualization, 11)
DEFINE_DXGI_PASSTHROUGH(DXGIDumpJournal, 12)
DEFINE_DXGI_PASSTHROUGH(DXGIReportAdapterConfiguration, 14)
DEFINE_DXGI_PASSTHROUGH(PIXBeginCapture, 15)
DEFINE_DXGI_PASSTHROUGH(PIXEndCapture, 16)
DEFINE_DXGI_PASSTHROUGH(PIXGetCaptureState, 17)
DEFINE_DXGI_PASSTHROUGH(SetAppCompatStringPointer, 18)
DEFINE_DXGI_PASSTHROUGH(UpdateHMDEmulationStatus, 19)

// ----------------------------------------------------------------------------
// Shadow ID3D11DeviceContext Vtable (256 Dynamic Forwarding Thunks)
// ----------------------------------------------------------------------------
// On Windows 10/11 + NVIDIA UMD (nvwgf2umx.dll), ID3D11DeviceContext::lpVtbl
// points to an embedded mutable dispatch array at (pCtx + 8) whose entries are
// overwritten in-place on every Flush() and Present() (amortized / fast-path
// switching). Redirecting *(void***)pCtx to g_shadowCtxVtbl while forwarding
// through g_origCtxVtbl[slot] preserves 100% of driver fast-path transitions
// while keeping our draw/state hooks permanently active across all frames!
extern "C" {
    void** g_origCtxVtbl = nullptr;
}

#define DEFINE_CTX_THUNK(idx) \
extern "C" __attribute__((naked)) void CtxThunk_##idx() { \
    __asm__ __volatile__( \
        "movq g_origCtxVtbl(%%rip), %%rax\n\t" \
        "movq " #idx "*8(%%rax), %%rax\n\t" \
        "jmpq *%%rax\n\t" \
        ::: "memory" \
    ); \
}

#define DEFINE_CTX_THUNK_16(base) \
    DEFINE_CTX_THUNK(base##0) DEFINE_CTX_THUNK(base##1) DEFINE_CTX_THUNK(base##2) DEFINE_CTX_THUNK(base##3) \
    DEFINE_CTX_THUNK(base##4) DEFINE_CTX_THUNK(base##5) DEFINE_CTX_THUNK(base##6) DEFINE_CTX_THUNK(base##7) \
    DEFINE_CTX_THUNK(base##8) DEFINE_CTX_THUNK(base##9) DEFINE_CTX_THUNK(base##A) DEFINE_CTX_THUNK(base##B) \
    DEFINE_CTX_THUNK(base##C) DEFINE_CTX_THUNK(base##D) DEFINE_CTX_THUNK(base##E) DEFINE_CTX_THUNK(base##F)

DEFINE_CTX_THUNK_16(0x0)
DEFINE_CTX_THUNK_16(0x1)
DEFINE_CTX_THUNK_16(0x2)
DEFINE_CTX_THUNK_16(0x3)
DEFINE_CTX_THUNK_16(0x4)
DEFINE_CTX_THUNK_16(0x5)
DEFINE_CTX_THUNK_16(0x6)
DEFINE_CTX_THUNK_16(0x7)
DEFINE_CTX_THUNK_16(0x8)
DEFINE_CTX_THUNK_16(0x9)
DEFINE_CTX_THUNK_16(0xA)
DEFINE_CTX_THUNK_16(0xB)
DEFINE_CTX_THUNK_16(0xC)
DEFINE_CTX_THUNK_16(0xD)
DEFINE_CTX_THUNK_16(0xE)
DEFINE_CTX_THUNK_16(0xF)

#define CTX_THUNK_PTR_16(base) \
    (void*)&CtxThunk_##base##0, (void*)&CtxThunk_##base##1, (void*)&CtxThunk_##base##2, (void*)&CtxThunk_##base##3, \
    (void*)&CtxThunk_##base##4, (void*)&CtxThunk_##base##5, (void*)&CtxThunk_##base##6, (void*)&CtxThunk_##base##7, \
    (void*)&CtxThunk_##base##8, (void*)&CtxThunk_##base##9, (void*)&CtxThunk_##base##A, (void*)&CtxThunk_##base##B, \
    (void*)&CtxThunk_##base##C, (void*)&CtxThunk_##base##D, (void*)&CtxThunk_##base##E, (void*)&CtxThunk_##base##F

static void* g_shadowCtxVtbl[256] = {
    CTX_THUNK_PTR_16(0x0), CTX_THUNK_PTR_16(0x1), CTX_THUNK_PTR_16(0x2), CTX_THUNK_PTR_16(0x3),
    CTX_THUNK_PTR_16(0x4), CTX_THUNK_PTR_16(0x5), CTX_THUNK_PTR_16(0x6), CTX_THUNK_PTR_16(0x7),
    CTX_THUNK_PTR_16(0x8), CTX_THUNK_PTR_16(0x9), CTX_THUNK_PTR_16(0xA), CTX_THUNK_PTR_16(0xB),
    CTX_THUNK_PTR_16(0xC), CTX_THUNK_PTR_16(0xD), CTX_THUNK_PTR_16(0xE), CTX_THUNK_PTR_16(0xF)
};

// ----------------------------------------------------------------------------
// Character Texture & Pixel Shader Reflection Tracking (Thread-Safe)
// ----------------------------------------------------------------------------
// Exact FNV1a-64 hashes of first 1024 bytes of Mip 0 in CS3 assets.pka:
static constexpr uint64_t HASH_DLIGHT_SKIN_CONV = 0x6E8A824949D77969ULL; // 512x16  BC7
static constexpr uint64_t HASH_DLIGHT_SKIN_BC1  = 0x43765C0E18883EF9ULL; // 512x16  BC1
static constexpr uint64_t HASH_DLIGHT_CONV      = 0x19D39F3FC7E78A80ULL; // 256x8   BC7 (Ennea C_CHR039 toon ramp)
static constexpr uint64_t HASH_ENNEA_FACE_DIFF  = 0xBBEA58A059203757ULL; // 512x512 BC7 (Ennea chr039_00_conv.dds)
static constexpr uint64_t HASH_EYES_SPHERE_CONV = 0xDF9BCEAC149B6609ULL; // 128x128 BC7
static constexpr uint64_t HASH_EYES_SPHERE_BC1  = 0x722814B15B79E7EBULL; // 128x128 BC1

static uint64_t Fnv1a64(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

struct PsBindInfo {
    int8_t cartoonSlot  = -1; // CartoonMapSampler
    int8_t sphereSlot   = -1; // SphereMapSampler
    int8_t normalSlot   = -1; // NormalMapSampler
    int8_t cubeSlot     = -1; // CubeMapSampler
    int8_t diffuseSlot  = -1; // DiffuseMapSampler
    int8_t diffuse2Slot = -1; // DiffuseMap2Sampler
    bool   relevant     = false;
};

static SRWLOCK g_assetLock = SRWLOCK_INIT;
static std::unordered_set<ID3D11Resource*>           g_skinRampTex;
static std::unordered_set<ID3D11Resource*>           g_eyeSphereTex;
static std::unordered_set<ID3D11Resource*>           g_dlightConvTex;
static std::unordered_set<ID3D11Resource*>           g_enneaFaceDiffTex;
static std::unordered_set<ID3D11ShaderResourceView*> g_skinRampSRVs;
static std::unordered_set<ID3D11ShaderResourceView*> g_eyeSphereSRVs;
static std::unordered_set<ID3D11ShaderResourceView*> g_dlightConvSRVs;
static std::unordered_set<ID3D11ShaderResourceView*> g_enneaFaceDiffSRVs;

static SRWLOCK g_psLock = SRWLOCK_INIT;
static std::unordered_map<ID3D11PixelShader*, PsBindInfo> g_psMap;
static std::atomic<uint32_t> g_psEpoch{1};
static std::atomic<uint32_t> g_statCartoonPsCount{0};
static std::atomic<uint32_t> g_statSkinSrvCount{0};

// Parse DXBC -> RDEF chunk to find exact bind slots for character samplers
static PsBindInfo ParsePixelShaderRdef(const void* bytecode, size_t len) {
    PsBindInfo info;
    if (!bytecode || len < 36) return info;
    const uint8_t* b = (const uint8_t*)bytecode;
    if (memcmp(b, "DXBC", 4) != 0) return info;

    uint32_t totalSz   = *(const uint32_t*)(b + 24);
    uint32_t chunkCnt  = *(const uint32_t*)(b + 28);
    if (totalSz > len || chunkCnt > 64 || 32u + chunkCnt * 4u > len) return info;

    for (uint32_t c = 0; c < chunkCnt; ++c) {
        uint32_t cOff = *(const uint32_t*)(b + 32 + c * 4);
        if (cOff + 8 > len) continue;
        const uint8_t* cp = b + cOff;
        if (memcmp(cp, "RDEF", 4) != 0) continue;

        uint32_t cSz = *(const uint32_t*)(cp + 4);
        if (cOff + 8 + cSz > len || cSz < 28) continue;

        const uint8_t* rbase = cp + 8;
        uint32_t rbCount = *(const uint32_t*)(rbase + 8);
        uint32_t rbOff   = *(const uint32_t*)(rbase + 12);
        uint16_t shType  = *(const uint16_t*)(rbase + 18); // 0xFFFF = Pixel Shader
        if (shType != 0xFFFF || rbCount > 256) continue;
        if ((uint64_t)rbOff + (uint64_t)rbCount * 32ull > cSz) continue;

        for (uint32_t r = 0; r < rbCount; ++r) {
            const uint8_t* e = rbase + rbOff + r * 32;
            uint32_t nameOff   = *(const uint32_t*)(e + 0);
            uint32_t inputType = *(const uint32_t*)(e + 4);  // 2 = D3D_SIT_TEXTURE
            uint32_t bindPt    = *(const uint32_t*)(e + 20); // t# register
            if (inputType != 2 || bindPt >= 16 || nameOff >= cSz) continue;

            const char* name = (const char*)(rbase + nameOff);
            size_t maxStr = cSz - nameOff;
            size_t slen = strnlen(name, maxStr);
            if (slen == maxStr) continue;

            if (strcmp(name, "CartoonMapSampler") == 0) {
                info.cartoonSlot = (int8_t)bindPt;
            } else if (strcmp(name, "SphereMapSampler") == 0) {
                info.sphereSlot = (int8_t)bindPt;
            } else if (strcmp(name, "NormalMapSampler") == 0) {
                info.normalSlot = (int8_t)bindPt;
            } else if (strcmp(name, "CubeMapSampler") == 0) {
                info.cubeSlot = (int8_t)bindPt;
            } else if (strcmp(name, "DiffuseMapSampler") == 0) {
                info.diffuseSlot = (int8_t)bindPt;
            } else if (strcmp(name, "DiffuseMap2Sampler") == 0) {
                info.diffuse2Slot = (int8_t)bindPt;
            }
        }
        break;
    }
    info.relevant = (info.cartoonSlot >= 0 || info.sphereSlot >= 0);
    return info;
}

// ----------------------------------------------------------------------------
// Runtime Shader Compilation & Face-Depth / Cleanse GPU Resources
// ----------------------------------------------------------------------------
using PFN_D3DCompile = HRESULT(WINAPI*)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

static const char* kFullscreenVSCode = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

static const char* kCleansePSCode = R"(
cbuffer CleanseCB : register(b0) {
    float g_strength;
    float g_depthEps;
    uint  g_sampleCount;
    uint  g_reverseZ;
    int   g_depthW;
    int   g_depthH;
    int   g_aoW;
    int   g_aoH;
    int   g_dilatePx;
    float g_dilateDepthEps;
    uint  g_visualize;
    float g_pad;
};

#if MSAA_PASS
Texture2DMS<float> SceneDepthTex : register(t0);
Texture2DMS<float> FaceDepthTex  : register(t1);
#else
Texture2D<float>   SceneDepthTex : register(t0);
Texture2D<float>   FaceDepthTex  : register(t1);
#endif

bool IsWritten(float fz) {
    return (g_reverseZ != 0u) ? (fz > 0.000001f) : (fz < 0.999999f);
}

float EvalPixelCoverage(int2 p, out float avgSceneZ) {
#if MSAA_PASS
    uint hits = 0u;
    float sumZ = 0.0f;
    [loop]
    for (uint s = 0u; s < g_sampleCount; ++s) {
        float sz = SceneDepthTex.Load(p, s);
        float fz = FaceDepthTex.Load(p, s);
        sumZ += sz;
        if (IsWritten(fz) && abs(sz - fz) <= g_depthEps) {
            hits++;
        }
    }
    avgSceneZ = sumZ / float(max(g_sampleCount, 1u));
    return float(hits) / float(max(g_sampleCount, 1u));
#else
    float sz = SceneDepthTex.Load(int3(p, 0));
    float fz = FaceDepthTex.Load(int3(p, 0));
    avgSceneZ = sz;
    return (IsWritten(fz) && abs(sz - fz) <= g_depthEps) ? 1.0f : 0.0f;
#endif
}

float4 main(float4 svPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    int2 p = (g_aoW == g_depthW && g_aoH == g_depthH)
           ? int2(svPos.xy)
           : int2(uv * float2(g_depthW, g_depthH));
    p = clamp(p, int2(0, 0), int2(g_depthW - 1, g_depthH - 1));

    float centerSceneZ = 1.0f;
    float cov = EvalPixelCoverage(p, centerSceneZ);

    if (g_dilatePx > 0 && cov < 1.0f) {
        [loop]
        for (int dy = -g_dilatePx; dy <= g_dilatePx; ++dy) {
            [loop]
            for (int dx = -g_dilatePx; dx <= g_dilatePx; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(g_depthW - 1, g_depthH - 1));
#if MSAA_PASS
                float nfz = FaceDepthTex.Load(q, 0);
#else
                float nfz = FaceDepthTex.Load(int3(q, 0));
#endif
                if (IsWritten(nfz) && abs(centerSceneZ - nfz) <= g_dilateDepthEps) {
                    cov = max(cov, 0.85f);
                }
            }
        }
    }

    float alpha = saturate(cov * g_strength);
    if (alpha <= 0.001f) discard;

    if (g_visualize != 0u) {
        return float4(0.0f, 0.0f, 0.0f, alpha);
    }
    return float4(1.0f, 1.0f, 1.0f, alpha);
}
)";

struct alignas(16) CleanseCBData {
    float    strength;
    float    depthEps;
    uint32_t sampleCount;
    uint32_t reverseZ;
    int32_t  depthW;
    int32_t  depthH;
    int32_t  aoW;
    int32_t  aoH;
    int32_t  dilatePx;
    float    dilateDepthEps;
    uint32_t visualize;
    float    pad;
};

struct GpuState {
    ID3D11Device*             device           = nullptr;
    ID3D11VertexShader*       fullscreenVS     = nullptr;
    ID3D11PixelShader*        cleansePS_Single = nullptr;
    ID3D11PixelShader*        cleansePS_MSAA   = nullptr;
    ID3D11Buffer*             cleanseCB        = nullptr;
    ID3D11BlendState*         cleanseBS        = nullptr;
    ID3D11RasterizerState*    cleanseRS        = nullptr;
    ID3D11DepthStencilState*  disabledDSS      = nullptr;
    ID3D11DepthStencilState*  writeLessEqDSS   = nullptr;
    ID3D11DepthStencilState*  writeGreaterEqDSS= nullptr;

    ID3D11Texture2D*          faceDepthTex     = nullptr;
    ID3D11DepthStencilView*   faceDepthDSV     = nullptr;
    ID3D11ShaderResourceView* faceDepthSRV     = nullptr;
    UINT                      faceW            = 0;
    UINT                      faceH            = 0;
    UINT                      faceSamples      = 0;
    UINT                      faceQuality      = 0;
    DXGI_FORMAT               faceTypelessFmt  = DXGI_FORMAT_UNKNOWN;
    bool                      shadersFailed    = false;

    void ReleaseDepthBuffers() {
        if (faceDepthSRV) { faceDepthSRV->Release(); faceDepthSRV = nullptr; }
        if (faceDepthDSV) { faceDepthDSV->Release(); faceDepthDSV = nullptr; }
        if (faceDepthTex) { faceDepthTex->Release(); faceDepthTex = nullptr; }
        faceW = faceH = faceSamples = faceQuality = 0;
        faceTypelessFmt = DXGI_FORMAT_UNKNOWN;
    }

    void ReleaseAll() {
        ReleaseDepthBuffers();
        if (fullscreenVS)      { fullscreenVS->Release();      fullscreenVS = nullptr; }
        if (cleansePS_Single)  { cleansePS_Single->Release();  cleansePS_Single = nullptr; }
        if (cleansePS_MSAA)    { cleansePS_MSAA->Release();    cleansePS_MSAA = nullptr; }
        if (cleanseCB)         { cleanseCB->Release();         cleanseCB = nullptr; }
        if (cleanseBS)         { cleanseBS->Release();         cleanseBS = nullptr; }
        if (cleanseRS)         { cleanseRS->Release();         cleanseRS = nullptr; }
        if (disabledDSS)       { disabledDSS->Release();       disabledDSS = nullptr; }
        if (writeLessEqDSS)    { writeLessEqDSS->Release();    writeLessEqDSS = nullptr; }
        if (writeGreaterEqDSS) { writeGreaterEqDSS->Release(); writeGreaterEqDSS = nullptr; }
        device = nullptr;
        shadersFailed = false;
    }
};

static GpuState g_gpu;

// Immediate-context render-thread tracking
static ID3D11DeviceContext*      g_trackedCtx          = nullptr;
static ID3D11ShaderResourceView* g_boundPsSRVs[16]     = {};
static ID3D11PixelShader*        g_boundPS             = nullptr;
static ID3D11PixelShader*        g_cachedPS            = nullptr;
static PsBindInfo                g_cachedPsInfo        = {};
static uint32_t                  g_cachedPsEpoch       = 0;

static ID3D11DepthStencilView*   g_lastSceneDSV        = nullptr;
static ID3D11Resource*           g_lastSceneDepthRes   = nullptr; // non-owning identity ptr
static UINT                      g_lastAoDepthW        = 0;
static UINT                      g_lastAoDepthH        = 0;
static UINT                      g_lastAoDepthSamples  = 0;
static bool                      g_faceDepthNeedsClear = true;
static bool                      g_isReverseZ          = false;
static uint32_t                  g_faceDrawsSinceClear = 0;
static uint64_t                  g_totalDrawCalls      = 0;
static bool                      g_inInternalDraw      = false;
static uint64_t                  g_renderAoCount       = 0;

// Function Pointer Types
using PFN_CreateTexture2D = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_CreateShaderResourceView = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using PFN_CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

using PFN_PSSetShaderResources = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PFN_PSSetShader = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstancedIndirect = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_DrawInstancedIndirect = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_ClearDepthStencilView = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_ClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);

static PFN_CreateTexture2D          Orig_CreateTexture2D          = nullptr;
static PFN_CreateShaderResourceView Orig_CreateShaderResourceView = nullptr;
static PFN_CreatePixelShader        Orig_CreatePixelShader        = nullptr;

// Helpers to invoke the live underlying ID3D11DeviceContext dispatch table
static inline void CallOrig_PSSetShaderResources(
    ID3D11DeviceContext* pCtx, UINT s, UINT n, ID3D11ShaderResourceView* const* srvs) {
    ((PFN_PSSetShaderResources)g_origCtxVtbl[8])(pCtx, s, n, srvs);
}
static inline void CallOrig_PSSetShader(
    ID3D11DeviceContext* pCtx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) {
    ((PFN_PSSetShader)g_origCtxVtbl[9])(pCtx, ps, ci, n);
}
static inline void CallOrig_Draw(ID3D11DeviceContext* pCtx, UINT v, UINT s) {
    ((PFN_Draw)g_origCtxVtbl[13])(pCtx, v, s);
}
static inline void CallOrig_ClearDepthStencilView(
    ID3D11DeviceContext* pCtx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT d, UINT8 st) {
    ((PFN_ClearDepthStencilView)g_origCtxVtbl[53])(pCtx, dsv, flags, d, st);
}

static bool EnsureGpuPipelineResources(ID3D11Device* dev) {
    if (g_gpu.device != dev) {
        g_gpu.ReleaseAll();
        g_gpu.device = dev;
    }
    if (g_gpu.shadersFailed) return false;
    if (g_gpu.fullscreenVS && g_gpu.cleansePS_Single && g_gpu.cleansePS_MSAA) return true;

    HMODULE d3dcomp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dcomp) {
        LogMsg("ERROR: Could not load d3dcompiler_47.dll");
        g_gpu.shadersFailed = true;
        return false;
    }
    auto pD3DCompile = (PFN_D3DCompile)GetProcAddress(d3dcomp, "D3DCompile");
    if (!pD3DCompile) {
        LogMsg("ERROR: Could not find D3DCompile in d3dcompiler_47.dll");
        g_gpu.shadersFailed = true;
        return false;
    }

    ID3DBlob* blob = nullptr;
    ID3DBlob* err  = nullptr;

    // 1. Fullscreen VS
    HRESULT hr = pD3DCompile(kFullscreenVSCode, strlen(kFullscreenVSCode), "CleanseVS",
                             nullptr, nullptr, "main", "vs_4_0", (1 << 15), 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        LogMsg("ERROR: VS compile failed (hr=0x%08X): %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        g_gpu.shadersFailed = true;
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gpu.fullscreenVS);
    blob->Release(); blob = nullptr;
    if (FAILED(hr)) { g_gpu.shadersFailed = true; return false; }

    // 2. Single-sample Cleanse PS
    D3D_SHADER_MACRO singleMacros[] = { {"MSAA_PASS", "0"}, {nullptr, nullptr} };
    hr = pD3DCompile(kCleansePSCode, strlen(kCleansePSCode), "CleansePS_1x",
                     singleMacros, nullptr, "main", "ps_4_0", (1 << 15), 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        LogMsg("ERROR: PS 1x compile failed (hr=0x%08X): %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        g_gpu.shadersFailed = true;
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = Orig_CreatePixelShader
       ? Orig_CreatePixelShader(dev, blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gpu.cleansePS_Single)
       : dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gpu.cleansePS_Single);
    blob->Release(); blob = nullptr;
    if (FAILED(hr)) { g_gpu.shadersFailed = true; return false; }

    // 3. MSAA Cleanse PS (ps_5_0 supports unsized Texture2DMS<float>; fallback to ps_4_1)
    D3D_SHADER_MACRO msaaMacros[] = { {"MSAA_PASS", "1"}, {nullptr, nullptr} };
    hr = pD3DCompile(kCleansePSCode, strlen(kCleansePSCode), "CleansePS_MSAA",
                     msaaMacros, nullptr, "main", "ps_5_0", (1 << 15), 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        if (err) { err->Release(); err = nullptr; }
        hr = pD3DCompile(kCleansePSCode, strlen(kCleansePSCode), "CleansePS_MSAA",
                         msaaMacros, nullptr, "main", "ps_4_1", (1 << 15), 0, &blob, &err);
    }
    if (FAILED(hr) || !blob) {
        LogMsg("ERROR: PS MSAA compile failed (hr=0x%08X): %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        g_gpu.shadersFailed = true;
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = Orig_CreatePixelShader
       ? Orig_CreatePixelShader(dev, blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gpu.cleansePS_MSAA)
       : dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gpu.cleansePS_MSAA);
    blob->Release(); blob = nullptr;
    if (FAILED(hr)) { g_gpu.shadersFailed = true; return false; }

    // 4. Constant Buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(CleanseCBData);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cbd, nullptr, &g_gpu.cleanseCB);

    // 5. Alpha-Blend State (Out = 1.0 * alpha + OrigAO * (1 - alpha))
    D3D11_BLEND_DESC bd = {};
    bd.AlphaToCoverageEnable                 = FALSE;
    bd.IndependentBlendEnable                = FALSE;
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &g_gpu.cleanseBS);

    // 6. Rasterizer State
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable   = FALSE;
    dev->CreateRasterizerState(&rd, &g_gpu.cleanseRS);

    // 7. DepthStencil States
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable    = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc      = D3D11_COMPARISON_ALWAYS;
    dsd.StencilEnable  = FALSE;
    dev->CreateDepthStencilState(&dsd, &g_gpu.disabledDSS);

    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dsd, &g_gpu.writeLessEqDSS);

    dsd.DepthFunc      = D3D11_COMPARISON_GREATER_EQUAL;
    dev->CreateDepthStencilState(&dsd, &g_gpu.writeGreaterEqDSS);

    LogMsg("Initialized Face HBAO Cleanse GPU pipeline resources on device %p", dev);
    return true;
}

static bool ResolveDepthFormats(DXGI_FORMAT inFmt, DXGI_FORMAT* outTypeless, DXGI_FORMAT* outDSV, DXGI_FORMAT* outSRV) {
    switch (inFmt) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            *outTypeless = DXGI_FORMAT_R24G8_TYPELESS;
            *outDSV      = DXGI_FORMAT_D24_UNORM_S8_UINT;
            *outSRV      = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            return true;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            *outTypeless = DXGI_FORMAT_R32_TYPELESS;
            *outDSV      = DXGI_FORMAT_D32_FLOAT;
            *outSRV      = DXGI_FORMAT_R32_FLOAT;
            return true;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            *outTypeless = DXGI_FORMAT_R32G8X24_TYPELESS;
            *outDSV      = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            *outSRV      = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            return true;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
            *outTypeless = DXGI_FORMAT_R16_TYPELESS;
            *outDSV      = DXGI_FORMAT_D16_UNORM;
            *outSRV      = DXGI_FORMAT_R16_UNORM;
            return true;
        default:
            return false;
    }
}

static bool EnsureFaceDepthBuffer(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& sceneDesc) {
    DXGI_FORMAT typelessFmt, dsvFmt, srvFmt;
    if (!ResolveDepthFormats(sceneDesc.Format, &typelessFmt, &dsvFmt, &srvFmt)) {
        return false;
    }
    if (g_gpu.device == dev && g_gpu.faceDepthTex &&
        g_gpu.faceW == sceneDesc.Width && g_gpu.faceH == sceneDesc.Height &&
        g_gpu.faceSamples == sceneDesc.SampleDesc.Count &&
        g_gpu.faceQuality == sceneDesc.SampleDesc.Quality &&
        g_gpu.faceTypelessFmt == typelessFmt) {
        return true;
    }

    if (!EnsureGpuPipelineResources(dev)) return false;
    g_gpu.ReleaseDepthBuffers();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width              = sceneDesc.Width;
    td.Height             = sceneDesc.Height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = typelessFmt;
    td.SampleDesc.Count   = sceneDesc.SampleDesc.Count;
    td.SampleDesc.Quality = sceneDesc.SampleDesc.Quality;
    td.Usage              = D3D11_USAGE_DEFAULT;
    td.BindFlags          = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = Orig_CreateTexture2D
               ? Orig_CreateTexture2D(dev, &td, nullptr, &g_gpu.faceDepthTex)
               : dev->CreateTexture2D(&td, nullptr, &g_gpu.faceDepthTex);
    if (FAILED(hr) || !g_gpu.faceDepthTex) {
        LogMsg("ERROR: Failed to create FaceDepthTex (%ux%u %uxMSAA fmt=%d hr=0x%08X)",
               td.Width, td.Height, td.SampleDesc.Count, (int)typelessFmt, (unsigned)hr);
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format = dsvFmt;
    dsvd.ViewDimension = (td.SampleDesc.Count > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = dev->CreateDepthStencilView(g_gpu.faceDepthTex, &dsvd, &g_gpu.faceDepthDSV);
    if (FAILED(hr) || !g_gpu.faceDepthDSV) {
        g_gpu.ReleaseDepthBuffers();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = srvFmt;
    if (td.SampleDesc.Count > 1) {
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
    }
    hr = Orig_CreateShaderResourceView
       ? Orig_CreateShaderResourceView(dev, g_gpu.faceDepthTex, &srvd, &g_gpu.faceDepthSRV)
       : dev->CreateShaderResourceView(g_gpu.faceDepthTex, &srvd, &g_gpu.faceDepthSRV);
    if (FAILED(hr) || !g_gpu.faceDepthSRV) {
        g_gpu.ReleaseDepthBuffers();
        return false;
    }

    g_gpu.faceW           = td.Width;
    g_gpu.faceH           = td.Height;
    g_gpu.faceSamples     = td.SampleDesc.Count;
    g_gpu.faceQuality     = td.SampleDesc.Quality;
    g_gpu.faceTypelessFmt = typelessFmt;
    g_faceDepthNeedsClear = true;
    g_faceDrawsSinceClear = 0;

    LogMsg("Created FaceDepthBuffer: %ux%u, %ux MSAA (q=%u), fmt=%d",
           td.Width, td.Height, td.SampleDesc.Count, td.SampleDesc.Quality, (int)typelessFmt);
    return true;
}

// ----------------------------------------------------------------------------
// Character Face Draw Detection & Hardware Depth Capture
// ----------------------------------------------------------------------------
static bool g_loggedFirstCartoonPS   = false;
static bool g_loggedFirstActiveClean = false;

static ID3D11ShaderResourceView* GetBoundOrQueryPsSRV(ID3D11DeviceContext* pCtx, int8_t slot) {
    if (slot < 0 || slot >= 16) return nullptr;
    ID3D11ShaderResourceView* srv = g_boundPsSRVs[slot];
    if (!srv && pCtx) {
        pCtx->PSGetShaderResources((UINT)slot, 1, &srv);
        if (srv) {
            g_boundPsSRVs[slot] = srv;
            srv->Release();
        }
    }
    return srv;
}

static bool IsCurrentDrawCharacterFace(ID3D11DeviceContext* pCtx) {
    ID3D11PixelShader* curPS = g_boundPS;
    if (!curPS && pCtx) {
        pCtx->PSGetShader(&curPS, nullptr, nullptr);
        if (curPS) {
            g_boundPS = curPS;
            curPS->Release();
        }
    }
    if (!curPS) return false;

    uint32_t curEpoch = g_psEpoch.load(std::memory_order_acquire);
    if (curPS != g_cachedPS || curEpoch != g_cachedPsEpoch) {
        AcquireSRWLockShared(&g_psLock);
        auto it = g_psMap.find(curPS);
        g_cachedPsInfo = (it != g_psMap.end()) ? it->second : PsBindInfo{};
        ReleaseSRWLockShared(&g_psLock);
        g_cachedPS      = curPS;
        g_cachedPsEpoch = curEpoch;
    }

    const PsBindInfo& ps = g_cachedPsInfo;
    if (!ps.relevant) return false;

    // Across all 678 character/NPC models in CS3:
    //   - 0 / 1,556 face materials, 0 / 312 faceparts, and 0 eyeballs use NormalMapSampler!
    //   - NormalMapSampler >= 0 exclusively appears on clothing, hair, ribbons, armor, and metal
    //     (including the 83 clothing/ribbon materials where artists left dlight_skin bound and
    //      the 2 metal/jewel materials that reuse eyes_sphere).
    if (ps.normalSlot >= 0 || ps.cubeSlot >= 0) return false;

    // 1. Check Eyeball shader FIRST (eyeballs bind SphereMapSampler = eyes_sphere* with normalSlot == -1)
    if (ps.sphereSlot >= 0) {
        if (!g_cfg.maskEyes) return false;
        ID3D11ShaderResourceView* sSrv = GetBoundOrQueryPsSRV(pCtx, ps.sphereSlot);
        if (!sSrv) return false;
        AcquireSRWLockShared(&g_assetLock);
        bool isEye = (g_eyeSphereSRVs.find(sSrv) != g_eyeSphereSRVs.end());
        ReleaseSRWLockShared(&g_assetLock);
        return isEye; // Never fall through on sphere-mapped materials (hair/glossy clothes)
    }

    // 2. Main CartoonMapSampler pass (OpaqueChr DefaultFPShader):
    //    Faces, mouth/teeth (faceparts), and clean non-normal-mapped skin bind dlight_skin*
    //    with normalSlot == -1 && sphereSlot == -1.
    //    NOTE: Do NOT match cartoonSlot == -1 passes! In CS3, cartoonSlot == -1 on character
    //    atlases is OutlineDefaultFPShader (back-face toon outline) & ForceTransparentDepthFPShader,
    //    which previously leaked onto clothes whenever body skin shared a diffuse atlas with clothes!
    if (ps.cartoonSlot >= 0) {
        ID3D11ShaderResourceView* cSrv = GetBoundOrQueryPsSRV(pCtx, ps.cartoonSlot);
        if (!cSrv) return false;

        AcquireSRWLockShared(&g_assetLock);
        bool isSkin       = (g_skinRampSRVs.find(cSrv) != g_skinRampSRVs.end());
        bool isDlightConv = (!isSkin && ps.diffuseSlot >= 0 &&
                             g_dlightConvSRVs.find(cSrv) != g_dlightConvSRVs.end());
        ReleaseSRWLockShared(&g_assetLock);

        if (isSkin) return true;
        if (isDlightConv) {
            ID3D11ShaderResourceView* dSrv = GetBoundOrQueryPsSRV(pCtx, ps.diffuseSlot);
            if (!dSrv) return false;
            AcquireSRWLockShared(&g_assetLock);
            bool isEnneaFace = (g_enneaFaceDiffSRVs.find(dSrv) != g_enneaFaceDiffSRVs.end());
            ReleaseSRWLockShared(&g_assetLock);
            return isEnneaFace;
        }
        return false;
    }

    return false;
}

template <typename DrawCallable>
static void ExecuteDrawWithFaceCapture(ID3D11DeviceContext* pCtx, DrawCallable&& doOrigDraw) {
    g_totalDrawCalls++;

    // Always execute the game's original draw untouched first!
    doOrigDraw();

    if (g_inInternalDraw || !g_cfg.enabled || pCtx != g_trackedCtx) return;
    if (!IsCurrentDrawCharacterFace(pCtx)) return;

    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* origDSV = nullptr;
    pCtx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &origDSV);

    auto releaseTargets = [&]() {
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (rtvs[i]) rtvs[i]->Release();
        }
        if (origDSV) origDSV->Release();
    };

    if (!origDSV) {
        releaseTargets();
        return;
    }

    ID3D11Resource* dsvRes = nullptr;
    origDSV->GetResource(&dsvRes);
    if (!dsvRes) {
        releaseTargets();
        return;
    }
    ID3D11Texture2D* dsvTex = nullptr;
    D3D11_TEXTURE2D_DESC dsvDesc = {};
    if (FAILED(dsvRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&dsvTex)) || !dsvTex) {
        dsvRes->Release();
        releaseTargets();
        return;
    }
    dsvTex->GetDesc(&dsvDesc);
    dsvTex->Release();
    bool exactSceneResMatch = (g_lastSceneDepthRes != nullptr && dsvRes == g_lastSceneDepthRes);
    dsvRes->Release();

    // Reject depth-only passes (shadow maps, ForceTransparentDepth camera-fade prepasses) where rtvs[0] == nullptr
    // and offscreen reflections/thumbnails whose dimensions/MSAA differ from SceneDepth!
    if (!rtvs[0]) {
        releaseTargets();
        return;
    }

    bool sizeMatchesScene = false;
    if (exactSceneResMatch) {
        sizeMatchesScene = true;
    } else if (g_lastAoDepthW > 0 && g_lastAoDepthH > 0) {
        sizeMatchesScene = (dsvDesc.ArraySize == 1 &&
                            dsvDesc.Width == g_lastAoDepthW &&
                            dsvDesc.Height == g_lastAoDepthH &&
                            dsvDesc.SampleDesc.Count == g_lastAoDepthSamples);
    } else {
        sizeMatchesScene = (dsvDesc.ArraySize == 1 &&
                            dsvDesc.Width >= 640 && dsvDesc.Height >= 360 &&
                            dsvDesc.Width != dsvDesc.Height);
    }
    if (!sizeMatchesScene) {
        releaseTargets();
        return;
    }

    // Capture opaque / alpha-tested draws that participate in SceneDepth (rejects alpha-blended decals)
    ID3D11DepthStencilState* origDSS = nullptr;
    UINT origStencilRef = 0;
    pCtx->OMGetDepthStencilState(&origDSS, &origStencilRef);

    ID3D11BlendState* origBS = nullptr;
    FLOAT origBF[4] = {};
    UINT origSM = 0xFFFFFFFF;
    pCtx->OMGetBlendState(&origBS, origBF, &origSM);
    bool blendEnabled = false;
    if (origBS) {
        D3D11_BLEND_DESC bdesc = {};
        origBS->GetDesc(&bdesc);
        blendEnabled = (bdesc.RenderTarget[0].BlendEnable != FALSE);
        origBS->Release();
    }

    bool writesDepth = !blendEnabled;
    bool revZ = false;
    if (origDSS) {
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        origDSS->GetDesc(&dsd);
        if (!dsd.DepthEnable || dsd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL) {
            writesDepth = false;
        }
        if (dsd.DepthFunc == D3D11_COMPARISON_GREATER || dsd.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL) {
            revZ = true;
        }
    }
    if (!writesDepth) {
        if (origDSS) origDSS->Release();
        releaseTargets();
        return;
    }

    ID3D11Device* dev = nullptr;
    pCtx->GetDevice(&dev);
    if (!dev || !EnsureFaceDepthBuffer(dev, dsvDesc)) {
        if (dev) dev->Release();
        if (origDSS) origDSS->Release();
        releaseTargets();
        return;
    }
    dev->Release();

    g_lastSceneDSV = origDSV;
    g_isReverseZ   = revZ;

    if (g_faceDepthNeedsClear) {
        CallOrig_ClearDepthStencilView(pCtx, g_gpu.faceDepthDSV, D3D11_CLEAR_DEPTH, revZ ? 0.0f : 1.0f, 0);
        g_faceDepthNeedsClear = false;
        g_faceDrawsSinceClear = 0;
    }

    // Replay the exact draw into g_gpu.faceDepthDSV (null color RTVs, stencil disabled)
    pCtx->OMSetRenderTargets(0, nullptr, g_gpu.faceDepthDSV);
    pCtx->OMSetDepthStencilState(revZ ? g_gpu.writeGreaterEqDSS : g_gpu.writeLessEqDSS, 0);

    g_inInternalDraw = true;
    doOrigDraw();
    g_inInternalDraw = false;
    g_faceDrawsSinceClear++;

    // Restore exact render targets and depth-stencil state
    pCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, origDSV);
    pCtx->OMSetDepthStencilState(origDSS, origStencilRef);

    if (origDSS) origDSS->Release();
    releaseTargets();
}

// ----------------------------------------------------------------------------
// D3D11 Device & Shadow ImmediateContext Hooks
// ----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(
    ID3D11Device* pDev,
    const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D** ppTexture2D)
{
    HRESULT hr = Orig_CreateTexture2D(pDev, pDesc, pInitialData, ppTexture2D);
    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && pDesc) {
        ID3D11Resource* resPtr = static_cast<ID3D11Resource*>(*ppTexture2D);
        int matchKind = 0; // 1 = skin ramp, 2 = eye sphere, 3 = dlight_conv, 4 = Ennea face diffuse

        if (pInitialData && pInitialData[0].pSysMem && pDesc->ArraySize == 1 && pDesc->MipLevels >= 1) {
            if (pDesc->Width == 512 && pDesc->Height == 16) {
                uint64_t h = Fnv1a64(pInitialData[0].pSysMem, 1024);
                if (h == HASH_DLIGHT_SKIN_CONV || h == HASH_DLIGHT_SKIN_BC1) {
                    matchKind = 1;
                }
            } else if (pDesc->Width == 128 && pDesc->Height == 128) {
                uint64_t h = Fnv1a64(pInitialData[0].pSysMem, 1024);
                if (h == HASH_EYES_SPHERE_CONV || h == HASH_EYES_SPHERE_BC1) {
                    matchKind = 2;
                }
            } else if (pDesc->Width == 256 && pDesc->Height == 8) {
                uint64_t h = Fnv1a64(pInitialData[0].pSysMem, 1024);
                if (h == HASH_DLIGHT_CONV) {
                    matchKind = 3;
                }
            } else if (pDesc->Width == 512 && pDesc->Height == 512) {
                uint64_t h = Fnv1a64(pInitialData[0].pSysMem, 1024);
                if (h == HASH_ENNEA_FACE_DIFF) {
                    matchKind = 4;
                }
            }
        }

        AcquireSRWLockExclusive(&g_assetLock);
        g_skinRampTex.erase(resPtr);
        g_eyeSphereTex.erase(resPtr);
        g_dlightConvTex.erase(resPtr);
        g_enneaFaceDiffTex.erase(resPtr);
        if (matchKind == 1) {
            g_skinRampTex.insert(resPtr);
        } else if (matchKind == 2) {
            g_eyeSphereTex.insert(resPtr);
        } else if (matchKind == 3) {
            g_dlightConvTex.insert(resPtr);
        } else if (matchKind == 4) {
            g_enneaFaceDiffTex.insert(resPtr);
        }
        ReleaseSRWLockExclusive(&g_assetLock);

        if (matchKind == 1) {
            LogMsg("Detected character skin toon ramp texture: %p (%ux%u mips=%u fmt=%d)",
                   resPtr, pDesc->Width, pDesc->Height, pDesc->MipLevels, (int)pDesc->Format);
        } else if (matchKind == 2) {
            LogMsg("Detected character eye sphere texture: %p (%ux%u mips=%u fmt=%d)",
                   resPtr, pDesc->Width, pDesc->Height, pDesc->MipLevels, (int)pDesc->Format);
        } else if (matchKind == 4) {
            LogMsg("Detected Ennea face diffuse texture: %p (%ux%u mips=%u fmt=%d)",
                   resPtr, pDesc->Width, pDesc->Height, pDesc->MipLevels, (int)pDesc->Format);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateShaderResourceView(
    ID3D11Device* pDev,
    ID3D11Resource* pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
    ID3D11ShaderResourceView** ppSRV)
{
    HRESULT hr = Orig_CreateShaderResourceView(pDev, pResource, pDesc, ppSRV);
    if (SUCCEEDED(hr) && ppSRV && *ppSRV) {
        ID3D11ShaderResourceView* srv = *ppSRV;
        ID3D11Resource* checkRes = nullptr;
        srv->GetResource(&checkRes);

        int taggedKind = 0;
        AcquireSRWLockExclusive(&g_assetLock);
        g_skinRampSRVs.erase(srv);
        g_eyeSphereSRVs.erase(srv);
        g_dlightConvSRVs.erase(srv);
        g_enneaFaceDiffSRVs.erase(srv);

        if ((pResource && g_skinRampTex.find(pResource) != g_skinRampTex.end()) ||
            (checkRes  && g_skinRampTex.find(checkRes)  != g_skinRampTex.end())) {
            g_skinRampSRVs.insert(srv);
            taggedKind = 1;
        } else if ((pResource && g_eyeSphereTex.find(pResource) != g_eyeSphereTex.end()) ||
                   (checkRes  && g_eyeSphereTex.find(checkRes)  != g_eyeSphereTex.end())) {
            g_eyeSphereSRVs.insert(srv);
            taggedKind = 2;
        } else if ((pResource && g_dlightConvTex.find(pResource) != g_dlightConvTex.end()) ||
                   (checkRes  && g_dlightConvTex.find(checkRes)  != g_dlightConvTex.end())) {
            g_dlightConvSRVs.insert(srv);
            taggedKind = 3;
        } else if ((pResource && g_enneaFaceDiffTex.find(pResource) != g_enneaFaceDiffTex.end()) ||
                   (checkRes  && g_enneaFaceDiffTex.find(checkRes)  != g_enneaFaceDiffTex.end())) {
            g_enneaFaceDiffSRVs.insert(srv);
            taggedKind = 4;
        }
        ReleaseSRWLockExclusive(&g_assetLock);

        if (checkRes) checkRes->Release();
        if (taggedKind == 1) {
            g_statSkinSrvCount.fetch_add(1, std::memory_order_relaxed);
            LogMsg("Tagged skin ramp SRV: %p (for tex %p)", srv, pResource);
        } else if (taggedKind == 2) {
            LogMsg("Tagged eye sphere SRV: %p (for tex %p)", srv, pResource);
        } else if (taggedKind == 4) {
            LogMsg("Tagged Ennea face diffuse SRV: %p (for tex %p)", srv, pResource);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePixelShader(
    ID3D11Device* pDev,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    ID3D11PixelShader** ppPixelShader)
{
    HRESULT hr = Orig_CreatePixelShader(pDev, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
    if (SUCCEEDED(hr) && ppPixelShader && *ppPixelShader) {
        PsBindInfo info = ParsePixelShaderRdef(pShaderBytecode, BytecodeLength);
        if (info.cartoonSlot >= 0) {
            g_statCartoonPsCount.fetch_add(1, std::memory_order_relaxed);
            if (!g_loggedFirstCartoonPS) {
                g_loggedFirstCartoonPS = true;
                LogMsg("Parsed first character toon PS (%p): diffuse=t%d, cartoon=t%d, normal=t%d, sphere=t%d",
                       *ppPixelShader, (int)info.diffuseSlot, (int)info.cartoonSlot, (int)info.normalSlot, (int)info.sphereSlot);
            }
        }
        AcquireSRWLockExclusive(&g_psLock);
        g_psMap[*ppPixelShader] = info;
        ReleaseSRWLockExclusive(&g_psLock);
        g_psEpoch.fetch_add(1, std::memory_order_release);
    }
    return hr;
}

static void STDMETHODCALLTYPE Hook_PSSetShaderResources(
    ID3D11DeviceContext* pCtx,
    UINT StartSlot,
    UINT NumViews,
    ID3D11ShaderResourceView* const* ppSRVs)
{
    if (!g_inInternalDraw) {
        if (pCtx != g_trackedCtx) {
            memset(g_boundPsSRVs, 0, sizeof(g_boundPsSRVs));
            g_boundPS = nullptr;
            g_trackedCtx = pCtx;
        }
        if (StartSlot < 16 && NumViews > 0) {
            UINT endSlot = StartSlot + NumViews;
            if (endSlot > 16) endSlot = 16;
            for (UINT s = StartSlot; s < endSlot; ++s) {
                g_boundPsSRVs[s] = ppSRVs ? ppSRVs[s - StartSlot] : nullptr;
            }
        }
    }
    CallOrig_PSSetShaderResources(pCtx, StartSlot, NumViews, ppSRVs);
}

static void STDMETHODCALLTYPE Hook_PSSetShader(
    ID3D11DeviceContext* pCtx,
    ID3D11PixelShader* pPixelShader,
    ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances)
{
    if (!g_inInternalDraw) {
        if (pCtx != g_trackedCtx) {
            memset(g_boundPsSRVs, 0, sizeof(g_boundPsSRVs));
            g_trackedCtx = pCtx;
        }
        g_boundPS = pPixelShader;
    }
    CallOrig_PSSetShader(pCtx, pPixelShader, ppClassInstances, NumClassInstances);
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(
    ID3D11DeviceContext* pCtx,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    ExecuteDrawWithFaceCapture(pCtx, [&]() {
        ((PFN_DrawIndexed)g_origCtxVtbl[12])(pCtx, IndexCount, StartIndexLocation, BaseVertexLocation);
    });
}

static void STDMETHODCALLTYPE Hook_Draw(
    ID3D11DeviceContext* pCtx,
    UINT VertexCount,
    UINT StartVertexLocation)
{
    ExecuteDrawWithFaceCapture(pCtx, [&]() {
        ((PFN_Draw)g_origCtxVtbl[13])(pCtx, VertexCount, StartVertexLocation);
    });
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(
    ID3D11DeviceContext* pCtx,
    UINT IndexCountPerInstance,
    UINT InstanceCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation,
    UINT StartInstanceLocation)
{
    ExecuteDrawWithFaceCapture(pCtx, [&]() {
        ((PFN_DrawIndexedInstanced)g_origCtxVtbl[20])(
            pCtx, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    });
}

static void STDMETHODCALLTYPE Hook_DrawInstanced(
    ID3D11DeviceContext* pCtx,
    UINT VertexCountPerInstance,
    UINT InstanceCount,
    UINT StartVertexLocation,
    UINT StartInstanceLocation)
{
    ExecuteDrawWithFaceCapture(pCtx, [&]() {
        ((PFN_DrawInstanced)g_origCtxVtbl[21])(
            pCtx, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    });
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(
    ID3D11DeviceContext* pCtx,
    ID3D11Buffer* pBufferForArgs,
    UINT AlignedByteOffsetForArgs)
{
    ExecuteDrawWithFaceCapture(pCtx, [&]() {
        ((PFN_DrawIndexedInstancedIndirect)g_origCtxVtbl[39])(pCtx, pBufferForArgs, AlignedByteOffsetForArgs);
    });
}

static void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(
    ID3D11DeviceContext* pCtx,
    ID3D11Buffer* pBufferForArgs,
    UINT AlignedByteOffsetForArgs)
{
    ExecuteDrawWithFaceCapture(pCtx, [&]() {
        ((PFN_DrawInstancedIndirect)g_origCtxVtbl[40])(pCtx, pBufferForArgs, AlignedByteOffsetForArgs);
    });
}

static void STDMETHODCALLTYPE Hook_ClearDepthStencilView(
    ID3D11DeviceContext* pCtx,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT ClearFlags,
    FLOAT Depth,
    UINT8 Stencil)
{
    CallOrig_ClearDepthStencilView(pCtx, pDepthStencilView, ClearFlags, Depth, Stencil);
    if (!g_inInternalDraw && (ClearFlags & D3D11_CLEAR_DEPTH) && pDepthStencilView) {
        bool isSceneClear = (pDepthStencilView == g_lastSceneDSV);
        if (!isSceneClear && g_lastSceneDepthRes) {
            ID3D11Resource* res = nullptr;
            pDepthStencilView->GetResource(&res);
            if (res) {
                isSceneClear = (res == g_lastSceneDepthRes);
                res->Release();
            }
        }
        if (isSceneClear) {
            g_faceDepthNeedsClear = true;
            g_faceDrawsSinceClear = 0;
        }
    }
}

static void STDMETHODCALLTYPE Hook_ClearState(ID3D11DeviceContext* pCtx) {
    if (pCtx == g_trackedCtx) {
        memset(g_boundPsSRVs, 0, sizeof(g_boundPsSRVs));
        g_boundPS = nullptr;
    }
    ((PFN_ClearState)g_origCtxVtbl[110])(pCtx);
}

static INIT_ONCE g_shadowVtblInitOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK InitShadowVtblCallback(PINIT_ONCE, PVOID, PVOID*) {
    g_shadowCtxVtbl[8]   = (void*)&Hook_PSSetShaderResources;
    g_shadowCtxVtbl[9]   = (void*)&Hook_PSSetShader;
    g_shadowCtxVtbl[12]  = (void*)&Hook_DrawIndexed;
    g_shadowCtxVtbl[13]  = (void*)&Hook_Draw;
    g_shadowCtxVtbl[20]  = (void*)&Hook_DrawIndexedInstanced;
    g_shadowCtxVtbl[21]  = (void*)&Hook_DrawInstanced;
    g_shadowCtxVtbl[39]  = (void*)&Hook_DrawIndexedInstancedIndirect;
    g_shadowCtxVtbl[40]  = (void*)&Hook_DrawInstancedIndirect;
    g_shadowCtxVtbl[53]  = (void*)&Hook_ClearDepthStencilView;
    g_shadowCtxVtbl[110] = (void*)&Hook_ClearState;
    return TRUE;
}

static void EnsureContextShadowHooked(ID3D11DeviceContext* pCtx) {
    if (!pCtx) return;
    InitOnceExecuteOnce(&g_shadowVtblInitOnce, InitShadowVtblCallback, nullptr, nullptr);
    void** curVtbl = *(void***)pCtx;
    if (curVtbl == g_shadowCtxVtbl) return;

    g_origCtxVtbl = curVtbl;
    DWORD oldProt = 0;
    if (VirtualProtect(pCtx, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        *(void***)pCtx = g_shadowCtxVtbl;
        VirtualProtect(pCtx, sizeof(void*), oldProt, &oldProt);
    }
    g_trackedCtx = pCtx;
    LogMsg("Installed persistent shadow vtable on ImmediateContext %p (origVtbl=%p -> shadow=%p)",
           pCtx, curVtbl, g_shadowCtxVtbl);
}

template <typename FnType>
static void PatchVtableSlot(void** vtbl, size_t idx, FnType hookFn, FnType* outOrigFn) {
    if (!vtbl || vtbl[idx] == (void*)hookFn) return;
    DWORD oldProt = 0;
    if (VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        if (!*outOrigFn) {
            *outOrigFn = (FnType)vtbl[idx];
        }
        vtbl[idx] = (void*)hookFn;
        VirtualProtect(&vtbl[idx], sizeof(void*), oldProt, &oldProt);
    }
}

static void HookD3D11DeviceAndContext(ID3D11Device* pDevice) {
    if (!pDevice) return;
    void** devVtbl = *(void***)pDevice;
    bool alreadyHooked = (devVtbl[5] == (void*)&Hook_CreateTexture2D);
    PatchVtableSlot(devVtbl, 5,  &Hook_CreateTexture2D,          &Orig_CreateTexture2D);
    PatchVtableSlot(devVtbl, 7,  &Hook_CreateShaderResourceView, &Orig_CreateShaderResourceView);
    PatchVtableSlot(devVtbl, 15, &Hook_CreatePixelShader,        &Orig_CreatePixelShader);

    ID3D11DeviceContext* immCtx = nullptr;
    pDevice->GetImmediateContext(&immCtx);
    if (immCtx) {
        EnsureContextShadowHooked(immCtx);
        immCtx->Release();
    }
    if (!alreadyHooked) {
        LogMsg("Hooked ID3D11Device (%p) vtable", pDevice);
    }
}

// ----------------------------------------------------------------------------
// NVIDIA HBAO+ (GFSDK_SSAO_D3D11.win64.dll v3.1) RenderAO Hook
// ----------------------------------------------------------------------------
using PFN_GFSDK_RenderAO = int(STDMETHODCALLTYPE*)(
    void* pAOContext,
    ID3D11DeviceContext* pDeviceContext,
    const void* pInput,
    const void* pParams,
    const void* pOutput,
    uint32_t renderMask);

static PFN_GFSDK_RenderAO Orig_RenderAO = nullptr;

static bool g_prevToggleDown = false;
static bool g_prevReloadDown = false;

static void PollHotkeys() {
    if (g_cfg.toggleKey != 0) {
        bool down = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
        if (down && !g_prevToggleDown) {
            g_cfg.enabled = !g_cfg.enabled;
            LogMsg("Hotkey toggled Face HBAO Fix -> %s", g_cfg.enabled ? "ON" : "OFF");
            PlayAsyncBeep(g_cfg.enabled ? 880 : 440);
        }
        g_prevToggleDown = down;
    }
    if (g_cfg.reloadIniKey != 0) {
        bool down = (GetAsyncKeyState(g_cfg.reloadIniKey) & 0x8000) != 0;
        if (down && !g_prevReloadDown) {
            LoadConfig();
            LogMsg("Reloaded hbao_face_fix.ini (Enabled=%d, MaskStrength=%.2f, Dilate=%d)",
                   (int)g_cfg.enabled, g_cfg.maskStrength, g_cfg.edgeDilatePixels);
            PlayAsyncBeep(660);
        }
        g_prevReloadDown = down;
    }
}

static bool GetResource2DDesc(ID3D11View* view, D3D11_TEXTURE2D_DESC* outDesc, ID3D11Resource** outRawRes = nullptr) {
    if (!view || !outDesc) return false;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (!res) return false;
    if (outRawRes) *outRawRes = res; // non-owning identity snapshot
    ID3D11Texture2D* tex = nullptr;
    bool ok = false;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        tex->GetDesc(outDesc);
        tex->Release();
        ok = true;
    }
    res->Release();
    return ok;
}

static int STDMETHODCALLTYPE Hook_RenderAO(
    void* pAOContext,
    ID3D11DeviceContext* pCtx,
    const void* pInput,
    const void* pParams,
    const void* pOutput,
    uint32_t renderMask)
{
    PollHotkeys();
    EnsureContextShadowHooked(pCtx);

    int status = Orig_RenderAO(pAOContext, pCtx, pInput, pParams, pOutput, renderMask);
    if (status != 0 || !pCtx || !pInput || !pOutput) {
        g_faceDepthNeedsClear = true;
        g_faceDrawsSinceClear = 0;
        return status;
    }

    auto sceneDepthSRV = *(ID3D11ShaderResourceView* const*)((const uint8_t*)pInput + 0x68);
    auto aoRTV         = *(ID3D11RenderTargetView* const*)((const uint8_t*)pOutput + 0x00);

    D3D11_TEXTURE2D_DESC depthDesc = {};
    D3D11_TEXTURE2D_DESC aoDesc    = {};
    ID3D11Resource* rawDepthRes    = nullptr;
    if (GetResource2DDesc(sceneDepthSRV, &depthDesc, &rawDepthRes) && GetResource2DDesc(aoRTV, &aoDesc)) {
        g_lastSceneDepthRes  = rawDepthRes;
        g_lastAoDepthW       = depthDesc.Width;
        g_lastAoDepthH       = depthDesc.Height;
        g_lastAoDepthSamples = depthDesc.SampleDesc.Count;

        if (g_renderAoCount < 2) {
            LogMsg("RenderAO #%llu: SceneDepth=%ux%u (%ux MSAA fmt=%d), SSAOBuffer=%ux%u (fmt=%d), totalDraws=%llu, cartoonPS=%u, skinSRVs=%u, faceDraws=%u",
                   (unsigned long long)g_renderAoCount,
                   depthDesc.Width, depthDesc.Height, depthDesc.SampleDesc.Count, (int)depthDesc.Format,
                   aoDesc.Width, aoDesc.Height, (int)aoDesc.Format,
                   (unsigned long long)g_totalDrawCalls,
                   g_statCartoonPsCount.load(std::memory_order_relaxed),
                   g_statSkinSrvCount.load(std::memory_order_relaxed),
                   g_faceDrawsSinceClear);
        }
        g_renderAoCount++;

        if (g_cfg.enabled && g_cfg.maskStrength > 0.001f &&
            g_faceDrawsSinceClear > 0 && g_gpu.faceDepthSRV &&
            g_gpu.faceW == depthDesc.Width && g_gpu.faceH == depthDesc.Height &&
            g_gpu.faceSamples == depthDesc.SampleDesc.Count &&
            g_gpu.fullscreenVS && g_gpu.cleanseCB && g_gpu.cleanseBS && g_gpu.cleanseRS && g_gpu.disabledDSS)
        {
            if (!g_loggedFirstActiveClean) {
                g_loggedFirstActiveClean = true;
                LogMsg("Active Face-Cleanse pass executing! faceDraws=%u (%ux%u %uxMSAA)",
                       g_faceDrawsSinceClear, depthDesc.Width, depthDesc.Height, depthDesc.SampleDesc.Count);
            }
            // Save all D3D11 state modified by our cleanse pass
            ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            ID3D11DepthStencilView* savedDSV = nullptr;
            pCtx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);

            ID3D11BlendState* savedBS = nullptr;
            FLOAT savedBlendFactor[4] = {};
            UINT  savedSampleMask = 0xFFFFFFFF;
            pCtx->OMGetBlendState(&savedBS, savedBlendFactor, &savedSampleMask);

            ID3D11DepthStencilState* savedDSS = nullptr;
            UINT savedStencilRef = 0;
            pCtx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

            ID3D11RasterizerState* savedRS = nullptr;
            pCtx->RSGetState(&savedRS);

            UINT numVPs = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_VIEWPORT savedVPs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
            pCtx->RSGetViewports(&numVPs, savedVPs);

            D3D11_PRIMITIVE_TOPOLOGY savedTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            pCtx->IAGetPrimitiveTopology(&savedTopo);

            ID3D11InputLayout* savedIL = nullptr;
            pCtx->IAGetInputLayout(&savedIL);

            ID3D11VertexShader*   savedVS = nullptr;
            ID3D11PixelShader*    savedPS = nullptr;
            ID3D11GeometryShader* savedGS = nullptr;
            ID3D11HullShader*     savedHS = nullptr;
            ID3D11DomainShader*   savedDS = nullptr;
            pCtx->VSGetShader(&savedVS, nullptr, nullptr);
            pCtx->PSGetShader(&savedPS, nullptr, nullptr);
            pCtx->GSGetShader(&savedGS, nullptr, nullptr);
            pCtx->HSGetShader(&savedHS, nullptr, nullptr);
            pCtx->DSGetShader(&savedDS, nullptr, nullptr);

            ID3D11ShaderResourceView* savedPSSRVs[2] = {};
            pCtx->PSGetShaderResources(0, 2, savedPSSRVs);

            ID3D11Buffer* savedPSCB0 = nullptr;
            pCtx->PSGetConstantBuffers(0, 1, &savedPSCB0);

            // Update constant buffer
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(pCtx->Map(g_gpu.cleanseCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                auto* cb = (CleanseCBData*)mapped.pData;
                cb->strength       = g_cfg.maskStrength;
                cb->depthEps       = g_cfg.depthEpsilon;
                cb->sampleCount    = depthDesc.SampleDesc.Count;
                cb->reverseZ       = g_isReverseZ ? 1u : 0u;
                cb->depthW         = (int32_t)depthDesc.Width;
                cb->depthH         = (int32_t)depthDesc.Height;
                cb->aoW            = (int32_t)aoDesc.Width;
                cb->aoH            = (int32_t)aoDesc.Height;
                cb->dilatePx       = g_cfg.edgeDilatePixels;
                cb->dilateDepthEps = g_cfg.dilateDepthEps;
                cb->visualize      = g_cfg.visualizeMask ? 1u : 0u;
                cb->pad            = 0.0f;
                pCtx->Unmap(g_gpu.cleanseCB, 0);
            }

            // Bind cleanse pass onto SSAOBuffer
            g_inInternalDraw = true;

            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (FLOAT)aoDesc.Width, (FLOAT)aoDesc.Height, 0.0f, 1.0f };
            pCtx->RSSetViewports(1, &vp);
            pCtx->RSSetState(g_gpu.cleanseRS);
            pCtx->OMSetRenderTargets(1, &aoRTV, nullptr);

            const FLOAT zeroBF[4] = { 0, 0, 0, 0 };
            pCtx->OMSetBlendState(g_gpu.cleanseBS, zeroBF, 0xFFFFFFFF);
            pCtx->OMSetDepthStencilState(g_gpu.disabledDSS, 0);

            pCtx->IASetInputLayout(nullptr);
            pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            pCtx->VSSetShader(g_gpu.fullscreenVS, nullptr, 0);
            pCtx->GSSetShader(nullptr, nullptr, 0);
            pCtx->HSSetShader(nullptr, nullptr, 0);
            pCtx->DSSetShader(nullptr, nullptr, 0);

            ID3D11PixelShader* cleansePS = (depthDesc.SampleDesc.Count > 1)
                                         ? g_gpu.cleansePS_MSAA
                                         : g_gpu.cleansePS_Single;
            CallOrig_PSSetShader(pCtx, cleansePS, nullptr, 0);
            pCtx->PSSetConstantBuffers(0, 1, &g_gpu.cleanseCB);

            ID3D11ShaderResourceView* cleanseSRVs[2] = { sceneDepthSRV, g_gpu.faceDepthSRV };
            CallOrig_PSSetShaderResources(pCtx, 0, 2, cleanseSRVs);

            CallOrig_Draw(pCtx, 3, 0);

            // Unbind depth SRVs BEFORE rebinding savedDSV to avoid D3D11 SRV/DSV hazard!
            ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
            CallOrig_PSSetShaderResources(pCtx, 0, 2, nullSRVs);

            // Restore pipeline state
            pCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
            pCtx->OMSetBlendState(savedBS, savedBlendFactor, savedSampleMask);
            pCtx->OMSetDepthStencilState(savedDSS, savedStencilRef);
            pCtx->RSSetState(savedRS);
            pCtx->RSSetViewports(numVPs, savedVPs);
            pCtx->IASetPrimitiveTopology(savedTopo);
            pCtx->IASetInputLayout(savedIL);
            pCtx->VSSetShader(savedVS, nullptr, 0);
            CallOrig_PSSetShader(pCtx, savedPS, nullptr, 0);
            pCtx->GSSetShader(savedGS, nullptr, 0);
            pCtx->HSSetShader(savedHS, nullptr, 0);
            pCtx->DSSetShader(savedDS, nullptr, 0);
            CallOrig_PSSetShaderResources(pCtx, 0, 2, savedPSSRVs);
            pCtx->PSSetConstantBuffers(0, 1, &savedPSCB0);

            g_inInternalDraw = false;

            // Release saved COM references
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                if (savedRTVs[i]) savedRTVs[i]->Release();
            }
            if (savedDSV)       savedDSV->Release();
            if (savedBS)        savedBS->Release();
            if (savedDSS)       savedDSS->Release();
            if (savedRS)        savedRS->Release();
            if (savedIL)        savedIL->Release();
            if (savedVS)        savedVS->Release();
            if (savedPS)        savedPS->Release();
            if (savedGS)        savedGS->Release();
            if (savedHS)        savedHS->Release();
            if (savedDS)        savedDS->Release();
            if (savedPSSRVs[0]) savedPSSRVs[0]->Release();
            if (savedPSSRVs[1]) savedPSSRVs[1]->Release();
            if (savedPSCB0)     savedPSCB0->Release();
        }
    }

    g_faceDepthNeedsClear = true;
    g_faceDrawsSinceClear = 0;
    return status;
}

// ----------------------------------------------------------------------------
// Executable IAT Hooks (d3d11.dll & GFSDK_SSAO_D3D11.win64.dll)
// ----------------------------------------------------------------------------
using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_GFSDK_CreateCtx4 = int(__cdecl*)(ID3D11Device*, void**, const void*, const void*);

static PFN_D3D11CreateDevice              Orig_D3D11CreateDevice              = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain  Orig_D3D11CreateDeviceAndSwapChain  = nullptr;
static PFN_GFSDK_CreateCtx4               Orig_GFSDK_SSAO_CreateContext_D3D11 = nullptr;

static HRESULT WINAPI Hook_D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    HRESULT hr = Orig_D3D11CreateDevice(pAdapter, DriverType, Software, Flags,
                                        pFeatureLevels, FeatureLevels, SDKVersion,
                                        ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookD3D11DeviceAndContext(*ppDevice);
    }
    if (SUCCEEDED(hr) && ppImmediateContext && *ppImmediateContext) {
        EnsureContextShadowHooked(*ppImmediateContext);
    }
    return hr;
}

static HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    HRESULT hr = Orig_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
                                                    pFeatureLevels, FeatureLevels, SDKVersion,
                                                    pSwapChainDesc, ppSwapChain, ppDevice,
                                                    pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookD3D11DeviceAndContext(*ppDevice);
    }
    if (SUCCEEDED(hr) && ppImmediateContext && *ppImmediateContext) {
        EnsureContextShadowHooked(*ppImmediateContext);
    }
    return hr;
}

static int __cdecl Hook_GFSDK_SSAO_CreateContext_D3D11(
    ID3D11Device* pD3DDevice, void** ppContext, const void* pCustomHeap, const void* pHeaderVersion)
{
    if (pD3DDevice) {
        HookD3D11DeviceAndContext(pD3DDevice);
    }
    int status = Orig_GFSDK_SSAO_CreateContext_D3D11(pD3DDevice, ppContext, pCustomHeap, pHeaderVersion);
    if (status == 0 && ppContext && *ppContext) {
        void** aoVtbl = *(void***)(*ppContext);
        PatchVtableSlot(aoVtbl, 1, &Hook_RenderAO, &Orig_RenderAO);
        LogMsg("Hooked GFSDK_SSAO_Context_D3D11 (%p) RenderAO vtable[1]", *ppContext);
    }
    return status;
}

template <typename FnType>
static bool PatchModuleIAT(HMODULE hMod, const char* dllName, const char* funcName, FnType hookFn, FnType* outOrigFn) {
    if (!hMod) return false;
    auto* base = (uint8_t*)hMod;
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return false;

    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* modName = (const char*)(base + desc->Name);
        if (_stricmp(modName, dllName) != 0) continue;

        auto* origThunk = desc->OriginalFirstThunk
                        ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk)
                        : nullptr;
        auto* iatThunk  = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        if (!origThunk) continue;

        uintptr_t imgStart = (uintptr_t)base;
        uintptr_t imgEnd   = imgStart + nt->OptionalHeader.SizeOfImage;

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, funcName) == 0) {
                uintptr_t curFn = (uintptr_t)iatThunk->u1.Function;
                if ((void*)curFn == (void*)hookFn) return true;
                if (curFn == 0 || (curFn >= imgStart && curFn < imgEnd)) return false;
                DWORD oldProt = 0;
                if (VirtualProtect(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    if (!*outOrigFn) {
                        *outOrigFn = (FnType)curFn;
                    }
                    iatThunk->u1.Function = (ULONG_PTR)hookFn;
                    VirtualProtect(&iatThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                    return true;
                }
            }
        }
    }
    return false;
}

static std::atomic<bool> g_iatHooked{false};

static void InstallExeIatHooks() {
    if (g_iatHooked.load(std::memory_order_acquire)) return;
    HMODULE exeMod = GetModuleHandleW(nullptr);
    bool h1 = PatchModuleIAT(exeMod, "d3d11.dll", "D3D11CreateDevice",
                             &Hook_D3D11CreateDevice, &Orig_D3D11CreateDevice);
    bool h2 = PatchModuleIAT(exeMod, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                             &Hook_D3D11CreateDeviceAndSwapChain, &Orig_D3D11CreateDeviceAndSwapChain);
    bool h3 = PatchModuleIAT(exeMod, "GFSDK_SSAO_D3D11.win64.dll", "GFSDK_SSAO_CreateContext_D3D11",
                             &Hook_GFSDK_SSAO_CreateContext_D3D11, &Orig_GFSDK_SSAO_CreateContext_D3D11);
    if ((h1 || h2) && h3) {
        g_iatHooked.store(true, std::memory_order_release);
    }
    LogMsg("IAT hooks installed on EXE (%p): D3D11CreateDevice=%d, D3D11CreateDeviceAndSwapChain=%d, GFSDK_SSAO_CreateContext_D3D11=%d",
           exeMod, (int)h1, (int)h2, (int)h3);
}

// ----------------------------------------------------------------------------
// DllMain Entry Point (Loader-Lock Safe: Zero LoadLibrary Calls!)
// ----------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        GetModuleFileNameW(hModule, g_dllDir, MAX_PATH);
        wchar_t* slash = wcsrchr(g_dllDir, L'\\');
        if (slash) *slash = 0;

        swprintf(g_iniPath, MAX_PATH, L"%s\\hbao_face_fix.ini", g_dllDir);
        swprintf(g_logPath, MAX_PATH, L"%s\\hbao_face_fix.log", g_dllDir);

        LoadConfig();
        if (g_cfg.logging) {
            FILE* f = _wfopen(g_logPath, L"w");
            if (f) {
                fprintf(f, "=== Trails of Cold Steel III - Face HBAO+ Fix v5 Initialized ===\n");
                fclose(f);
            }
        }
        LogMsg("Config loaded: Enabled=%d, MaskStrength=%.2f, DepthEps=%.7f, ToggleKey=0x%X",
               (int)g_cfg.enabled, g_cfg.maskStrength, g_cfg.depthEpsilon, g_cfg.toggleKey);

        InstallExeIatHooks();
    }
    return TRUE;
}
