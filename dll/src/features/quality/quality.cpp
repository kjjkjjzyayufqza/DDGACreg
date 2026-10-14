#include "features/module_api.h"
#include "features/hook_chain.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

// Image base 0x140000000. RVAs below are subtracted from that.
//
// FConsoleManager singleton. FindConsoleObject is sub_140461EF0.
constexpr std::uintptr_t kConsoleManagerRva = 0x2DD7F50;
constexpr std::uintptr_t kFindConsoleObjectRva = 0x461EF0;

// IConsoleVariable::Set. Int and float value cvars and int refs each have
// their own override; all three take (this, string, set-by flags).
constexpr std::uintptr_t kIntSetRva = 0x472200;
constexpr std::uintptr_t kFloatSetRva = 0x472280;
constexpr std::uintptr_t kIntRefSetRva = 0x4723F0;

// Scalability callback registration. Runs once. Applying groups before this
// returns stores the index but does not load the @Cine ini section.
constexpr std::uintptr_t kScalabilityInitRva = 0x14D8450;

// APlayerCameraManager::UpdateCamera. Re-applies if something wrote the
// value fields without going through Set.
constexpr std::uintptr_t kUpdateCameraRva = 0x14BD5B0;

// ECVF_SetByConsole. Highest priority in this build (scalability is 0x01000000).
constexpr unsigned kSetByConsole = 0x0A000000u;

// FConsoleVariable<T> game-thread value. Int refs store int* at this offset.
constexpr unsigned kValueOffset = 104;

// IConsoleVariable::Set is vtable slot 11 on the int, float, and int-ref types.
constexpr unsigned kSetSlot = 11;

// sg.* NumLevels is 5, so index 4 is the last level and loads section @Cine.
// sg.ResolutionQuality is a percentage, not that index. 4 would clamp to 10%.

enum class ValueKind {
    Int,
    Float,
    IntRef,
};

struct Cvar {
    const wchar_t* name;
    const wchar_t* text;
    const char* label;
    ValueKind kind;
    bool group;
    void* object;
    // Consecutive reads that still do not match after Set. Stops a bad
    // offset from calling Set on every camera tick.
    unsigned misses;
};

// Groups first so @Cine is applied before the LOD overrides below it.
// grass.DensityScale is not the registered name (the exe has grass.densityScale).
// @3 and @Cine both set that density to 1, so it is not an arcade cut.
Cvar g_cvars[] = {
    {L"sg.ViewDistanceQuality", L"4", "sg.ViewDistanceQuality", ValueKind::Int, true, nullptr},
    {L"sg.AntiAliasingQuality", L"4", "sg.AntiAliasingQuality", ValueKind::Int, true, nullptr},
    {L"sg.ShadowQuality", L"4", "sg.ShadowQuality", ValueKind::Int, true, nullptr},
    {L"sg.PostProcessQuality", L"4", "sg.PostProcessQuality", ValueKind::Int, true, nullptr},
    {L"sg.TextureQuality", L"4", "sg.TextureQuality", ValueKind::Int, true, nullptr},
    {L"sg.EffectsQuality", L"4", "sg.EffectsQuality", ValueKind::Int, true, nullptr},
    {L"sg.FoliageQuality", L"4", "sg.FoliageQuality", ValueKind::Int, true, nullptr},
    {L"sg.ResolutionQuality", L"100", "sg.ResolutionQuality", ValueKind::Float, true, nullptr},

    // Cache floors the scale at 0 and the distance cull multiplies by it with
    // no upper clamp. @Cine is already 10, so 10 does not extend past cinematic.
    {L"r.ViewDistanceScale", L"100", "r.ViewDistanceScale", ValueKind::Float, false, nullptr},
    // 0 never shows an HLOD cluster. DistanceScale pushes any leftover transition out.
    {L"r.HLOD.MaximumLevel", L"0", "r.HLOD.MaximumLevel", ValueKind::IntRef, false, nullptr},
    {L"r.HLOD.DistanceScale", L"10", "r.HLOD.DistanceScale", ValueKind::Float, false, nullptr},
    {L"r.SkeletalMeshLODBias", L"-10", "r.SkeletalMeshLODBias", ValueKind::IntRef, false, nullptr},
    {L"r.StaticMeshLODDistanceScale", L"0.01", "r.StaticMeshLODDistanceScale", ValueKind::Float, false, nullptr},
    {L"foliage.ForceLOD", L"0", "foliage.ForceLOD", ValueKind::Int, false, nullptr},
    // Multiplies the screen-size foliage distance. Higher keeps that distance larger.
    {L"foliage.LODDistanceScale", L"10", "foliage.LODDistanceScale", ValueKind::Float, false, nullptr},
    // <=0 skips the screen-size foliage cull (the cull distance stays at FLT_MAX).
    {L"foliage.MinimumScreenSize", L"0", "foliage.MinimumScreenSize", ValueKind::Float, false, nullptr},
    // Added to the desired particle LOD. A negative sum clamps to LOD 0.
    {L"r.ParticleLODBias", L"-10", "r.ParticleLODBias", ValueKind::IntRef, false, nullptr},
    {L"r.LandscapeLODBias", L"0", "r.LandscapeLODBias", ValueKind::IntRef, false, nullptr},

    {L"r.LightFunctionQuality", L"1", "r.LightFunctionQuality", ValueKind::Int, false, nullptr},
    {L"r.ShadowQuality", L"5", "r.ShadowQuality", ValueKind::Int, false, nullptr},
    {L"r.Shadow.CSM.MaxCascades", L"10", "r.Shadow.CSM.MaxCascades", ValueKind::Int, false, nullptr},
    {L"r.Shadow.MaxResolution", L"4096", "r.Shadow.MaxResolution", ValueKind::Int, false, nullptr},
    {L"r.Shadow.RadiusThreshold", L"0.03", "r.Shadow.RadiusThreshold", ValueKind::Float, false, nullptr},
    // Reader clamps this to [0, 2]. 2 is the largest distance the exe will use.
    {L"r.Shadow.DistanceScale", L"2", "r.Shadow.DistanceScale", ValueKind::Float, false, nullptr},
    {L"r.Shadow.CSM.TransitionScale", L"1", "r.Shadow.CSM.TransitionScale", ValueKind::Float, false, nullptr},
    {L"r.DistanceFieldShadowing", L"1", "r.DistanceFieldShadowing", ValueKind::Int, false, nullptr},
    {L"r.DistanceFieldAO", L"1", "r.DistanceFieldAO", ValueKind::Int, false, nullptr},

    {L"r.PostProcessAAQuality", L"6", "r.PostProcessAAQuality", ValueKind::Int, false, nullptr},
    // Quality must be >0 for the blur pass. 0 is off. DefaultFeature and Amount
    // stop a post-process volume from turning the amount back on.
    {L"r.MotionBlurQuality", L"0", "r.MotionBlurQuality", ValueKind::Int, false, nullptr},
    {L"r.DefaultFeature.MotionBlur", L"0", "r.DefaultFeature.MotionBlur", ValueKind::Int, false, nullptr},
    {L"r.MotionBlur.Amount", L"0", "r.MotionBlur.Amount", ValueKind::Float, false, nullptr},
    {L"r.AmbientOcclusionMipLevelFactor", L"0.4", "r.AmbientOcclusionMipLevelFactor", ValueKind::Float, false, nullptr},
    {L"r.AmbientOcclusionMaxQuality", L"100", "r.AmbientOcclusionMaxQuality", ValueKind::Int, false, nullptr},
    {L"r.AmbientOcclusionLevels", L"-1", "r.AmbientOcclusionLevels", ValueKind::Int, false, nullptr},
    {L"r.AmbientOcclusionRadiusScale", L"1", "r.AmbientOcclusionRadiusScale", ValueKind::Float, false, nullptr},
    {L"r.DepthOfFieldQuality", L"4", "r.DepthOfFieldQuality", ValueKind::Int, false, nullptr},
    {L"r.RenderTargetPoolMin", L"1000", "r.RenderTargetPoolMin", ValueKind::Int, false, nullptr},
    {L"r.LensFlareQuality", L"3", "r.LensFlareQuality", ValueKind::Int, false, nullptr},
    {L"r.SceneColorFringeQuality", L"1", "r.SceneColorFringeQuality", ValueKind::Int, false, nullptr},
    {L"r.EyeAdaptationQuality", L"2", "r.EyeAdaptationQuality", ValueKind::Int, false, nullptr},
    {L"r.BloomQuality", L"5", "r.BloomQuality", ValueKind::Int, false, nullptr},
    {L"r.FastBlurThreshold", L"100", "r.FastBlurThreshold", ValueKind::Float, false, nullptr},
    {L"r.Upscale.Quality", L"3", "r.Upscale.Quality", ValueKind::Int, false, nullptr},
    {L"r.Tonemapper.GrainQuantization", L"1", "r.Tonemapper.GrainQuantization", ValueKind::Int, false, nullptr},
    {L"r.LightShaftQuality", L"1", "r.LightShaftQuality", ValueKind::Int, false, nullptr},
    {L"r.Filter.SizeScale", L"1", "r.Filter.SizeScale", ValueKind::Float, false, nullptr},
    {L"r.Tonemapper.Quality", L"5", "r.Tonemapper.Quality", ValueKind::Int, false, nullptr},

    {L"r.Streaming.MipBias", L"0", "r.Streaming.MipBias", ValueKind::Float, false, nullptr},
    {L"r.MipMapLODBias", L"0", "r.MipMapLODBias", ValueKind::Float, false, nullptr},
    {L"r.MaxAnisotropy", L"8", "r.MaxAnisotropy", ValueKind::Int, false, nullptr},
    {L"r.Streaming.LimitPoolSizeToVRAM", L"0", "r.Streaming.LimitPoolSizeToVRAM", ValueKind::Int, false, nullptr},

    {L"r.TranslucencyLightingVolumeDim", L"64", "r.TranslucencyLightingVolumeDim", ValueKind::Int, false, nullptr},
    {L"r.RefractionQuality", L"2", "r.RefractionQuality", ValueKind::Int, false, nullptr},
    {L"r.SSR.Quality", L"4", "r.SSR.Quality", ValueKind::Int, false, nullptr},
    {L"r.SceneColorFormat", L"4", "r.SceneColorFormat", ValueKind::Int, false, nullptr},
    {L"r.DetailMode", L"2", "r.DetailMode", ValueKind::Int, false, nullptr},
    {L"r.TranslucencyVolumeBlur", L"1", "r.TranslucencyVolumeBlur", ValueKind::Int, false, nullptr},
    {L"r.MaterialQualityLevel", L"1", "r.MaterialQualityLevel", ValueKind::Int, false, nullptr},
    {L"r.SSS.Scale", L"1", "r.SSS.Scale", ValueKind::Float, false, nullptr},
    {L"r.SSS.SampleSet", L"2", "r.SSS.SampleSet", ValueKind::Int, false, nullptr},
    {L"r.SSS.Quality", L"1", "r.SSS.Quality", ValueKind::Int, false, nullptr},
    {L"r.SSS.HalfRes", L"0", "r.SSS.HalfRes", ValueKind::Int, false, nullptr},
    {L"r.EmitterSpawnRateScale", L"1", "r.EmitterSpawnRateScale", ValueKind::Float, false, nullptr},
    {L"r.ParticleLightQuality", L"2", "r.ParticleLightQuality", ValueKind::Int, false, nullptr},
    {L"foliage.DensityScale", L"1", "foliage.DensityScale", ValueKind::Float, false, nullptr},
    // Smaller multiplies the per-decal fade threshold, so decals fade later.
    {L"r.Decal.FadeScreenSizeMult", L"0.01", "r.Decal.FadeScreenSizeMult", ValueKind::Float, false, nullptr},

    {L"r.ScreenPercentage", L"100", "r.ScreenPercentage", ValueKind::Float, false, nullptr},
    {L"r.ScreenPercentageLeft", L"100", "r.ScreenPercentageLeft", ValueKind::Float, false, nullptr},
    {L"r.ScreenPercentageCenter", L"100", "r.ScreenPercentageCenter", ValueKind::Float, false, nullptr},
    {L"r.ScreenPercentageRight", L"100", "r.ScreenPercentageRight", ValueKind::Float, false, nullptr},

    {L"dgo.AutoDestroyCachedMips", L"0", "dgo.AutoDestroyCachedMips", ValueKind::Int, false, nullptr},
    // AutoDestroy drives this. Pin it off in case that callback writes 1 directly.
    {L"dgo.DroppingMips", L"0", "dgo.DroppingMips", ValueKind::Int, false, nullptr},
    // >0 replaces r.Streaming.PoolSize. 0 keeps the pool cvar below.
    {L"dgo.Streaming.PoolSizeKHTr", L"0", "dgo.Streaming.PoolSizeKHTr", ValueKind::Int, false, nullptr},
    // @Cine is 3000 MB. The project ini sets 1500 and KHTr was overwriting that with 1200.
    {L"r.Streaming.PoolSize", L"3000", "r.Streaming.PoolSize", ValueKind::Int, false, nullptr},
};

using SetFn = std::int64_t(__fastcall*)(void* self, const wchar_t* value, unsigned setBy);
using FindFn = void*(__fastcall*)(void* manager, const wchar_t* name);
using ScalabilityInitFn = double(__fastcall*)();
using UpdateCameraFn = std::int64_t(__fastcall*)(void* camera, float deltaSeconds);

SetFn g_intSet = nullptr;
SetFn g_floatSet = nullptr;
SetFn g_intRefSet = nullptr;
FindFn g_find = nullptr;
ScalabilityInitFn g_scalabilityInit = nullptr;
UpdateCameraFn g_updateCamera = nullptr;

unsigned char* g_base = nullptr;
bool g_bound = false;
bool g_forcedOnce = false;
bool g_cineApplied = false;
bool g_applying = false;
bool g_loggedMissing = false;

const wchar_t* ForcedText(void* object)
{
    if (object == nullptr)
        return nullptr;
    for (const Cvar& cvar : g_cvars) {
        if (cvar.object == object)
            return cvar.text;
    }
    return nullptr;
}

std::int64_t Dispatch(SetFn original, void* self, const wchar_t* value, unsigned setBy,
    const wchar_t* (*rewrite)(void*, const wchar_t*))
{
    // While ForceAll is inside Set, still rewrite the string. The @Cine
    // callback writes the stock cinematic numbers; LOD overrides replace those.
    // Stream budgets share these Set entry points, so a cvar this list does
    // not own is offered to that rewrite.
    if (const wchar_t* forced = ForcedText(self))
        value = forced;
    else if (rewrite != nullptr) {
        if (const wchar_t* shared = rewrite(self, value))
            value = shared;
    }
    return original(self, value, setBy);
}

std::int64_t __fastcall IntSetDetour(void* self, const wchar_t* value, unsigned setBy)
{
    return Dispatch(g_intSet, self, value, setBy, &hookchain::RewriteOwnedInt);
}

std::int64_t __fastcall FloatSetDetour(void* self, const wchar_t* value, unsigned setBy)
{
    return Dispatch(g_floatSet, self, value, setBy, nullptr);
}

std::int64_t __fastcall IntRefSetDetour(void* self, const wchar_t* value, unsigned setBy)
{
    return Dispatch(g_intRefSet, self, value, setBy, &hookchain::RewriteIntRef);
}

void* Manager()
{
    if (g_base == nullptr)
        return nullptr;
    return *reinterpret_cast<void**>(g_base + kConsoleManagerRva);
}

bool Bind()
{
    void* manager = Manager();
    if (manager == nullptr || g_find == nullptr)
        return false;

    bool complete = true;
    for (Cvar& cvar : g_cvars) {
        if (cvar.object == nullptr)
            cvar.object = g_find(manager, cvar.name);
        if (cvar.object == nullptr)
            complete = false;
    }
    g_bound = complete;
    return complete;
}

int ReadInt(const Cvar& cvar)
{
    auto* bytes = static_cast<unsigned char*>(cvar.object);
    if (cvar.kind == ValueKind::IntRef) {
        int* slot = *reinterpret_cast<int**>(bytes + kValueOffset);
        return slot != nullptr ? *slot : 0;
    }
    return *reinterpret_cast<int*>(bytes + kValueOffset);
}

float ReadFloat(const Cvar& cvar)
{
    return *reinterpret_cast<float*>(static_cast<unsigned char*>(cvar.object) + kValueOffset);
}

bool AlreadySet(const Cvar& cvar)
{
    if (cvar.object == nullptr)
        return false;
    if (cvar.kind == ValueKind::Float) {
        const float want = static_cast<float>(wcstof(cvar.text, nullptr));
        return std::fabs(ReadFloat(cvar) - want) <= 0.001f;
    }
    return ReadInt(cvar) == _wtoi(cvar.text);
}

void WriteCvar(Cvar& cvar, bool force)
{
    if (cvar.object == nullptr || cvar.misses >= 3)
        return;
    if (!force && AlreadySet(cvar)) {
        cvar.misses = 0;
        return;
    }

    void** vtable = *reinterpret_cast<void***>(cvar.object);
    auto set = reinterpret_cast<SetFn>(vtable[kSetSlot]);
    set(cvar.object, cvar.text, kSetByConsole);

    if (AlreadySet(cvar))
        cvar.misses = 0;
    else if (++cvar.misses >= 3)
        LOG("Quality: %s did not stick; stop rewriting", cvar.label);
}

Cvar* FindLabel(const char* label)
{
    for (Cvar& cvar : g_cvars) {
        if (cvar.label != nullptr && std::strcmp(cvar.label, label) == 0)
            return &cvar;
    }
    return nullptr;
}

void LogLiveValues()
{
    const Cvar* view = FindLabel("sg.ViewDistanceQuality");
    const Cvar* scale = FindLabel("r.ViewDistanceScale");
    const Cvar* shadow = FindLabel("r.Shadow.MaxResolution");
    const Cvar* aa = FindLabel("r.PostProcessAAQuality");
    const Cvar* skel = FindLabel("r.SkeletalMeshLODBias");
    const Cvar* foliage = FindLabel("foliage.ForceLOD");
    const Cvar* mesh = FindLabel("r.StaticMeshLODDistanceScale");
    const Cvar* screen = FindLabel("r.ScreenPercentage");
    const Cvar* mips = FindLabel("dgo.AutoDestroyCachedMips");
    LOG("Quality: live sg.ViewDistance=%d viewScale=%.2f shadowMax=%d aa=%d skelBias=%d foliageForce=%d meshLodScale=%.3f screen=%.0f dropMips=%d",
        view != nullptr && view->object != nullptr ? ReadInt(*view) : -999,
        scale != nullptr && scale->object != nullptr ? ReadFloat(*scale) : -1.0f,
        shadow != nullptr && shadow->object != nullptr ? ReadInt(*shadow) : -1,
        aa != nullptr && aa->object != nullptr ? ReadInt(*aa) : -1,
        skel != nullptr && skel->object != nullptr ? ReadInt(*skel) : -999,
        foliage != nullptr && foliage->object != nullptr ? ReadInt(*foliage) : -999,
        mesh != nullptr && mesh->object != nullptr ? ReadFloat(*mesh) : -1.0f,
        screen != nullptr && screen->object != nullptr ? ReadFloat(*screen) : -1.0f,
        mips != nullptr && mips->object != nullptr ? ReadInt(*mips) : -1);
}

void ForceAll()
{
    if (!Bind()) {
        if (!g_loggedMissing) {
            g_loggedMissing = true;
            for (const Cvar& cvar : g_cvars) {
                if (cvar.object == nullptr)
                    LOG("Quality: cvar not registered: %s", cvar.label);
            }
        }
        const Cvar* view = FindLabel("sg.ViewDistanceQuality");
        if (view == nullptr || view->object == nullptr)
            return;
    }

    if (!g_forcedOnce)
        LogLiveValues();

    // Groups go through Set once after the scalability callbacks exist, even
    // when the stored index is already 4. Later frames only write a drift.
    const bool pushGroups = !g_cineApplied;
    for (Cvar& cvar : g_cvars)
        WriteCvar(cvar, cvar.group && pushGroups);

    g_cineApplied = true;

    if (!g_forcedOnce) {
        g_forcedOnce = true;
        LOG("Quality: forcing sg=4, view distance 100, HLOD off, shadow distance 2, motion blur off, pool 3000");
    }
}

void PushDrifted()
{
    if (!g_bound)
        Bind();
    for (Cvar& cvar : g_cvars)
        WriteCvar(cvar, false);
}

double __fastcall ScalabilityInitDetour()
{
    const double result = g_scalabilityInit();
    if (!g_applying) {
        g_applying = true;
        // OnChanged is installed only by the call above. An earlier camera
        // tick can store index 4 without loading the @Cine section.
        g_cineApplied = false;
        ForceAll();
        g_applying = false;
    }
    return result;
}

void CameraTick()
{
    if (g_applying)
        return;
    g_applying = true;
    if (!g_cineApplied)
        ForceAll();
    else
        PushDrifted();
    g_applying = false;
}

std::int64_t __fastcall UpdateCameraDetour(void* camera, float deltaSeconds)
{
    CameraTick();
    return g_updateCamera != nullptr ? g_updateCamera(camera, deltaSeconds) : 0;
}

bool Hook(void* target, void* detour, void** original, const char* label)
{
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        LOG("Quality: hook failed for %s", label);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        LOG("Quality: enable failed for %s", label);
        return false;
    }
    return true;
}

bool QualityEnabled(const wchar_t* moduleDir)
{
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return false;

    wchar_t path[MAX_PATH];
    if (wcscpy_s(path, moduleDir) != 0)
        return false;
    const std::size_t length = wcslen(path);
    if (length == 0 || length + 12 >= MAX_PATH)
        return false;
    if (path[length - 1] != L'\\' && path[length - 1] != L'/') {
        if (wcscat_s(path, L"\\") != 0)
            return false;
    }
    if (wcscat_s(path, L"config.ini") != 0)
        return false;
    return GetPrivateProfileIntW(L"Quality", L"Enabled", 1, path) != 0;
}

} // namespace

namespace features {

void Quality_Install(const wchar_t* moduleDir)
{
    if (!QualityEnabled(moduleDir)) {
        LOG("Quality: disabled");
        return;
    }

    HMODULE game = GetModuleHandleW(L"TG4AC-Win64-Shipping.exe");
    if (game == nullptr)
        game = GetModuleHandleW(nullptr);
    if (game == nullptr) {
        LOG("Quality: game module not loaded");
        return;
    }

    g_base = reinterpret_cast<unsigned char*>(game);
    g_find = reinterpret_cast<FindFn>(g_base + kFindConsoleObjectRva);

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("Quality: MH_Initialize failed (%d)", static_cast<int>(initStatus));
        return;
    }

    const bool intSet = Hook(g_base + kIntSetRva, &IntSetDetour, reinterpret_cast<void**>(&g_intSet), "int Set");
    const bool floatSet = Hook(g_base + kFloatSetRva, &FloatSetDetour, reinterpret_cast<void**>(&g_floatSet), "float Set");
    const bool refSet = Hook(g_base + kIntRefSetRva, &IntRefSetDetour, reinterpret_cast<void**>(&g_intRefSet), "int ref Set");
    const bool init = Hook(g_base + kScalabilityInitRva, &ScalabilityInitDetour,
        reinterpret_cast<void**>(&g_scalabilityInit), "scalability init");

    // HUD already detours UpdateCamera when it is enabled. Register a tick
    // instead of taking a second hook that MinHook would reject.
    bool camera = false;
    const MH_STATUS cameraStatus = MH_CreateHook(
        g_base + kUpdateCameraRva, &UpdateCameraDetour, reinterpret_cast<void**>(&g_updateCamera));
    if (cameraStatus == MH_ERROR_ALREADY_CREATED) {
        hookchain::SetCameraTick(&CameraTick);
        camera = true;
        LOG("Quality: UpdateCamera shared with HUD");
    } else if (cameraStatus == MH_OK && MH_EnableHook(g_base + kUpdateCameraRva) == MH_OK) {
        camera = true;
    } else {
        LOG("Quality: hook failed for UpdateCamera");
    }

    if (intSet && floatSet && refSet && init && camera)
        LOG("Quality: hooks installed");
}

} // namespace features
