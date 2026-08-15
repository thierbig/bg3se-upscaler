# Phase 2b investigation — camera constants + NGX depth/mvec tags

Read-only investigation, branch `dlss-fg` (HEAD at investigation time, after "PHASE 2a SEALED"). No code was modified. All citations are `file:line` against the repo at `/mnt/c/Dev/bg3se-upscaler`.

---

## Q1 — Camera matrices: the struct and how to reach a live instance

### The struct that owns the four matrices

`BG3Extender/GameDefinitions/Components/Camera.h:113-124`, inside `BEGIN_NS(rf) ... END_NS()` (namespace `rf`):

```cpp
struct Camera
{
    float field_0;
    glm::vec3 field_4;
    glm::mat4 ViewMatrix;
    glm::mat4 InvViewMatrix;
    glm::mat4 ProjectionMatrix;
    glm::mat4 InvProjectionMatrix;
    //glm::mat4 GetViewProjectionMatrix;
    //glm::mat4 GetOffsetViewProjectionMatrix;
    std::array<glm::vec4, 10> CullingPlanes;
};
```

Full name: **`rf::Camera`**. It is a **plain render struct**, not an ECS component — there is no `DEFINE_COMPONENT(...)` anywhere in its body. It is a sub-object embedded by value inside `rf::CameraController` (see below), not something you fetch from the ECS directly.

It is, however, registered in the reflection/property-map system for Lua access: `BG3Extender/GameDefinitions/Generated/PropertyMaps.inl:6737-6745`:

```cpp
BEGIN_CLS(rf::Camera, 814)
P_NOTIFY(field_0, TemporaryName)
P_NOTIFY(field_4, TemporaryName)
P(ViewMatrix)
P(InvViewMatrix)
P(ProjectionMatrix)
P(InvProjectionMatrix)
P(CullingPlanes)
END_CLS()
```

### `rf::CameraController` — the object that embeds `rf::Camera`

`Camera.h:126-173`:

```cpp
struct CameraController : public ProtectedGameObject<CameraController>
{
    virtual ~CameraController() = 0;
    virtual bool Enter() = 0;
    virtual bool Exit() = 0;
    virtual float GetFov() = 0;
    virtual float GetAspectRatio() = 0;
    virtual void Update(GameTime const&) = 0;
    virtual bool DoLookAt(glm::vec3 const&, glm::vec3 const&, bool) = 0;
    virtual bool EditorLookAt(glm::vec3 const&) = 0;
    virtual void EditorZoomExtents(glm::vec3 const&, float) = 0;
    virtual glm::vec3 const& GetWorldTranslate() const = 0;
    virtual void SetWorldTranslate(glm::vec3 const&) = 0;
    virtual glm::quat const& GetWorldRotate() const = 0;
    virtual void SetWorldRotate(glm::quat const&) = 0;
    virtual Transform const& GetWorldTransform() const = 0;
    virtual void SetWorldTransform(Transform const&) = 0;
    virtual void WarpMouse(bool) = 0;
    virtual void ClearInputState() = 0;
    virtual uint16_t* OnInputEvent(uint16_t* eventRetVal, void* inputEvent) = 0;
    virtual void StopMotion() = 0;
    virtual bool SetOrientation(float, float) = 0;
    virtual void SetMoveSpeed(float) = 0;
    virtual void SetLookAtShake(float, float) = 0;
    virtual void SetWobble(float, float) = 0;
    virtual glm::vec3 GetDistance_M() = 0;

    [[bg3::hidden]] __int64 field_8;
    bool ViewDirty;
    bool ProjectionDirty;
    bool IsOrthographic;
    bool OffCenter;
    float FOV;
    float NearPlane;
    float FarPlane;
    float AspectRatio;
    Transform WorldTransform;
    glm::vec3 LookAt;
    glm::vec3 field_58;
    float OrthoLeft;
    float OrthoRight;
    float OrthoBottom;
    float OrthoTop;
    FixedString ID;
    [[bg3::hidden]] __int64 field_78;
    Camera Camera;                              // <-- rf::Camera, embedded by value (line 171)
    std::array<glm::vec3, 8> FrustumPoints;
};
```

Note `Camera Camera;` at `Camera.h:171` — the live `rf::Camera` (and its four matrices) lives **inside** the `CameraController` object, not behind a separate pointer. `CameraController` also directly exposes `GetWorldTranslate()`/`GetWorldRotate()`/`GetWorldTransform()` (position/rotation independent of the matrices) and `FOV`/`NearPlane`/`FarPlane`/`AspectRatio` — all of which are exactly the inputs `sl::Constants` needs for `cameraPos`/`cameraUp`/`cameraRight`/`cameraFwd`/`cameraNear`/`cameraFar`/`cameraFOV`/`cameraAspectRatio` (see Q4).

Also registered for reflection, `PropertyMaps.inl:6747-6766` (note `P(Camera)` at line 6764, confirming the embedded-by-value relationship at the reflection layer too):

```cpp
BEGIN_CLS(rf::CameraController, 815)
P(ViewDirty)
P(ProjectionDirty)
P(IsOrthographic)
P(OffCenter)
P(FOV)
P(NearPlane)
P(FarPlane)
P(AspectRatio)
P(WorldTransform)
P(LookAt)
P_NOTIFY(field_58, TemporaryName)
P(OrthoLeft)
P(OrthoRight)
P(OrthoBottom)
P(OrthoTop)
P(ID)
P(Camera)
P(FrustumPoints)
END_CLS()
```

### `CameraComponent` (`Camera.h:287`) — field dump and what it holds

`Camera.h:287-299`:

```cpp
struct CameraComponent : public BaseComponent
{
    DEFINE_COMPONENT(Camera, "ls::CameraComponent")

    uint32_t MasterBehaviorType;
    rf::CameraController* Controller;
    int ExposureSettingIndex;
    bool Active;
    bool AcceptsInput;
    bool UseCameraPPSettings;
    bool UseSplitScreenFov;
    PostProcessCameraSetting PostProcess;
};
```

`CameraComponent` **is** an ECS component (`DEFINE_COMPONENT(Camera, "ls::CameraComponent")`, `ExtComponentType::Camera`). It does **not** contain the matrices directly and does not embed `rf::Camera`/`rf::CameraController` by value — it holds a raw pointer, `rf::CameraController* Controller` (line 292).

**Full path from a `CameraComponent` to `ViewMatrix`/`ProjectionMatrix`:**

```
CameraComponent            (ECS component, "ls::CameraComponent")
  .Controller               -> rf::CameraController*          (Camera.h:292)
      ->Camera               -> rf::Camera                    (embedded value, Camera.h:171)
          .ViewMatrix / .InvViewMatrix / .ProjectionMatrix / .InvProjectionMatrix   (Camera.h:117-120)
```

i.e. `cameraComponent->Controller->Camera.ViewMatrix`, after a null check on `Controller` (it's a raw pointer and the component can exist before/without an active controller).

`CameraComponent` is also registered for reflection, `PropertyMaps.inl:6863` onward (`BEGIN_CLS(CameraComponent, 824)`, `INHERIT(BaseComponent)`, `P(Controller)` at `PropertyMaps.inl:6866`).

### Is there exactly one active camera? Existing code that reads it?

`CameraComponent::Active` (`Camera.h:294`) implies the ECS can hold more than one `CameraComponent` entity, with (in the common case) one flagged active. The data model has explicit multi-camera/split-screen concepts:

- `CameraGlobalSwitches` (`Camera.h:8-107`) has `MultiCameraDistanceMax`/`MultiCameraDistanceMin`/`MultiCameraDistanceDefault` (`Camera.h:32-34`).
- `CameraComponent::UseSplitScreenFov` (`Camera.h:297`).
- `ecl::camera::PhotoModeCameraBehaviorComponent` (`Camera.h:491-497`) and `PhotoModeCameraTransformRequestsSingletonComponent` (`Camera.h:484-489`) show the engine swaps in a different controller (`FixedString CameraControllerId`) for photo mode rather than spawning a second live camera feed.

This repo targets the Vulkan/PC build, which does not ship local split-screen (that's console-only in BG3), so in practice there should be exactly one `CameraComponent` with `Active == true` feeding the renderer during normal Vulkan/PC play — but this is inferred from the data model and game knowledge, not proven from static code. **Recommend a one-time runtime check** before Phase 2b locks in the assumption: e.g. `Ext.Entity.GetAllEntitiesWithComponent("Camera")` from a debug console/IMGUI panel, and inspect `.Active` on each result.

**No existing extender C++ code reads camera position/rotation/matrices anywhere in this codebase.** A repo-wide search (`grep -rn "camera" -i` across `BG3Extender/`, plus targeted searches for `ActiveCamera`, `GetCamera`, `CameraManager`, `CameraSystem`) turns up nothing outside:
- the `Camera.h` component/struct definitions themselves,
- their auto-generated `PropertyMaps.inl` reflection entries (generic Lua property-map metadata — table-driven, not bespoke logic; this is what would back a Lua script doing `entity.Camera.Controller.Camera.ViewMatrix`, but no such script or C++ call exists in-tree), and
- an unrelated `void* CameraManager;` field in `BG3Extender/GameDefinitions/Components/Timeline.h:649` (hidden/opaque, cinematics-related, not used).

There is **zero** camera-reading code under `BG3Extender/Extender/Client/IMGUI/` (the render-hook layer) — confirmed by grepping that directory for `GetComponent`, `GetEntityWorld`, `EntityHandle`, `GetEntityHelpers` (no hits). Phase 2b will be the first code to actually resolve and read this path.

---

## Q2 — How extender code reads a live ECS component instance at runtime

### Canonical C++ call sequence

1. Get the per-side entity-system helpers object. On the client:
   `BG3Extender/Extender/Client/ScriptExtenderClient.h:40`:
   ```cpp
   inline ecs::ClientEntitySystemHelpers& GetEntityHelpers()
   ```
   reached via `gExtender->GetClient().GetEntityHelpers()` (this exact accessor is used at `BG3Extender/GameDefinitions/EntitySystem.cpp:1912-1914` and `BG3Extender/Extender/Shared/ExtensionState.cpp:36`). `ClientEntitySystemHelpers : public EntitySystemHelpersBase` (`EntitySystemHelpers.h:451`).

2. `EntitySystemHelpersBase::GetEntityWorld()` (`EntitySystemHelpers.h:294-298`):
   ```cpp
   inline EntityWorld* GetEntityWorld() const
   {
       assert(World != nullptr); // Should not be called before helpers are initialized
       return World;
   }
   ```
   (there's also `HasEntityWorld()`, `EntitySystemHelpers.h:289-292`, for a non-asserting check.)

3. Resolve an `EntityHandle` (from a GUID, a net ID, or an existing handle) — e.g. `EntitySystemHelpersBase::GetEntityHandle(Guid const&)` (`EntitySystemHelpers.h:338`), or by enumerating entities that have the component at all (see below).

4. Fetch the component with the templated accessor, `EntitySystemHelpers.h:250-254`:
   ```cpp
   template <class T>
   T* GetComponent(EntityHandle entityHandle)
   {
       return static_cast<T*>(GetRawComponent(entityHandle, T::ComponentType));
   }
   ```
   `T::ComponentType` comes from the `DEFINE_COMPONENT(componentType, cls)` macro (`BG3Extender/GameDefinitions/Base/Base.h:45-49`):
   ```cpp
   #define DEFINE_COMPONENT(componentType, cls) \
       static constexpr ExtComponentType ComponentType = ExtComponentType::componentType; \
       static constexpr auto ComponentName = #componentType; \
       static constexpr auto EngineClass = cls; \
       static constexpr auto OneFrame = false;
   ```
   so for `CameraComponent` this is `ExtComponentType::Camera` (from `DEFINE_COMPONENT(Camera, "ls::CameraComponent")`, `Camera.h:289`).

5. `GetRawComponent` resolution chain, `BG3Extender/GameDefinitions/EntitySystem.cpp:1246-1268`:
   ```cpp
   void* EntitySystemHelpersBase::GetRawComponent(EntityHandle entityHandle, PerComponentData const& meta)
   {
       if (!meta.ComponentIndex) return nullptr;
       auto world = GetEntityWorld();
       if (!world) return nullptr;
       if (meta.IsProxy) {
           return world->GetAndDereferenceRawComponent(entityHandle, *meta.ComponentIndex, meta.InlineSize);
       } else {
           return world->GetRawComponent(entityHandle, *meta.ComponentIndex, meta.InlineSize);
       }
   }

   void* EntitySystemHelpersBase::GetRawComponent(EntityHandle entityHandle, ExtComponentType type)
   {
       auto const& meta = GetComponentMeta(type);
       return GetRawComponent(entityHandle, meta);
   }
   ```
   and `EntityWorld::GetRawComponent`, `EntitySystem.cpp:652-663`:
   ```cpp
   void* EntityWorld::GetRawComponent(EntityHandle entityHandle, ComponentTypeIndex type, std::size_t componentSize)
   {
       auto component = GetCommittedComponent(entityHandle, type, componentSize);
       if (!component) {
           component = GetImmediateComponent(entityHandle, type);
           if (!component) {
               component = GetECBComponent(entityHandle, type);
           }
       }
       return component;
   }
   ```
   i.e. it checks committed storage, then the immediate write/read-change cache, then the entity command buffer (staged-but-not-committed changes), in that order.

### Concrete precedent in-tree (this is how Lua's `Ext.Entity` resolves components)

`BG3Extender/Lua/Libs/Entity.inl:4-6`:
```cpp
std::optional<Guid> HandleToUuid(lua_State* L, EntityHandle entity)
{
    auto uuid = State::FromLua(L)->GetEntitySystemHelpers()->GetComponent<UuidComponent>(entity);
    ...
```
and the enumeration form (`GetAllEntitiesWithComponent`, `Entity.inl:83-129`) which walks either a registered `SingleComponentQuery` or the full `world->Storage->Storages` list if the component has no dedicated query — this is the mechanism that would back `Ext.Entity.GetAllEntitiesWithComponent("Camera")` and is the natural way to answer the "how many `Active` camera entities exist right now" question at runtime (see Q1).

### Render-thread safety — **not safe to call directly from the Vulkan/NGX hook; snapshot on the game/update thread instead**

Evidence:

1. **The Vulkan/NGX hooks explicitly run on a distinct render thread with no synchronization of their own.** Current-branch comment, `BG3Extender/Extender/Client/IMGUI/Vulkan.inl:1179-1187`:
   > "Everything below touches state shared with NewFrame()/FinishFrame()/the present hook - the render pass, framebuffer and pipeline caches, and viewports_[drawViewport_]. This callback runs on the game's render thread with no synchronisation of its own, so take the same lock those paths use..."

   This is the extender's own author documenting that `ngxEvaluateFeatureCHook` (the NGX `EvaluateFeature` hook — same hook Phase 2b would extend to read/write tags) runs on the game's render thread, and that touching shared state there requires explicit locking (`globalResourceLock_`, a `std::mutex`).

2. **The game/simulation update runs on a separate hook point, installed separately from the Vulkan hooks**, `BG3Extender/Extender/Client/ScriptExtenderClient.cpp:76-87`:
   ```cpp
   if (lib.ecl__GameStateThreaded__GameStateWorker__DoWork != nullptr) {
       gameStateWorkerStart_.Wrap(lib.ecl__GameStateThreaded__GameStateWorker__DoWork);
   }
   if (lib.ecl__GameStateMachine__Update != nullptr) {
       gameStateMachineUpdate_.Wrap(lib.ecl__GameStateMachine__Update);
   }
   ...
   gameStateWorkerStart_.SetWrapper(&ScriptExtender::GameStateWorkerWrapper, this);
   gameStateMachineUpdate_.SetPrePostHook(&ScriptExtender::OnPreUpdate, &ScriptExtender::OnUpdate, this);
   ```
   The symbol names themselves (`GameStateThreaded::GameStateWorker::DoWork`, a worker-thread entry point) indicate game/simulation state advances on its own worker thread/tick, distinct from wherever `vkQueuePresentKHR`/NGX `EvaluateFeature` are invoked.

3. `ScriptExtender::OnUpdate`/`OnUpdateGuarded` (`ScriptExtenderClient.cpp:298-315`) is the extender's existing game-thread tick — it already calls `gExtender->IMGUI().Update()` and `extensionState_->OnUpdate(*time)` from there, i.e. the codebase already treats "OnUpdate" (game thread) and "the Vulkan/NGX hooks" (render thread) as two separate execution contexts that need to hand data across, not one.

4. **No precedent exists for touching ECS from the render-hook layer.** Confirmed by grep: nothing under `BG3Extender/Extender/Client/IMGUI/` references `GetComponent`, `GetEntityWorld`, `EntityHandle`, or `GetEntityHelpers`.

**Conclusion:** `GetComponent<CameraComponent>(entity)` itself won't crash if called from inside the NGX hook (it is a plain, already-allocated-memory read, no lock/assert tied to thread identity) — but doing so races the GameStateWorker thread's writes to `Controller->Camera.{View,Projection}Matrix` (four `glm::mat4`, i.e. 64 floats) with no engine-provided synchronization at that point. A concurrent read there risks observing a torn/half-updated matrix for one frame during fast camera motion. The pattern already used elsewhere in this file (`globalResourceLock_`, `std::lock_guard`) is the template to reuse: **snapshot the camera data (matrices + `WorldTransform`/`FOV`/near/far/aspect) into a small extender-owned POD, written once per frame from `ScriptExtender::OnUpdate` (game thread) under a lock or double-buffer, and have the render-thread NGX hook read only that snapshot** — never call `GetComponent`/walk `Controller->Camera` directly from inside `ngxEvaluateFeatureCHook` or `vkQueuePresentKHRHooked`.

---

## Q3 — NGX `EvaluateFeature` hook + parameter reads

### Important correction to the brief: the hook is not parked — it's already live on `dlss-fg`

`git show origin/ngx-experiments:BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (1296 lines) was diffed against the current `dlss-fg` `Vulkan.inl` (1677 lines). The entire NGX hook-install mechanism and the `NVSDK_NGX_Resource_VK`/`ngxGetVoidPointer_`/"Output" parameter read are **byte-for-byte identical** between the two (only shifted by +3 lines in the current file, from two extra `#include`s). The `dlss-fg` branch's `EnableHooks()`/`vkCreateInstance` handling has been substantially rewritten (manual Streamline hooking replaced the old `upscaler.dll`/`sl.interposer.dll` load-and-forward approach), but the NGX `EvaluateFeature` hook itself was carried forward unchanged and is **currently active** — it composites the ImGui overlay directly into the NGX output image (controlled by `NgxOverlayStage`), not parked/disabled. So "reviving" it is really "extending an already-running hook to also read/write the DLSS-SR *input* tags", not resurrecting dead code.

### The hook-install / module-scan mechanism (identical in both branches; current-branch line numbers, `Vulkan.inl:1517-1600`)

```cpp
bool installNgxHookFrom(HMODULE mod, wchar_t const* label)
{
    // The C++ and _C entry points take the same argument layout; they differ only in the
    // callback type, which we forward untouched. Prefer _C, but accept either.
    static char const* const symbols[] = {
        "NVSDK_NGX_VULKAN_EvaluateFeature_C",
        "NVSDK_NGX_VULKAN_EvaluateFeature"
    };

    for (auto symbol : symbols) {
        auto proc = GetProcAddress(mod, symbol);
        if (!proc) continue;

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        ngxEvaluateFeatureHook_.Wrap(ResolveFunctionTrampoline(
            reinterpret_cast<NgxEvaluateFeatureCHookType::BaseFuncType*>(proc)));
        DetourTransactionCommit();
        ngxEvaluateFeatureHook_.SetWrapper(&VulkanBackend::ngxEvaluateFeatureCHook, this);
        INFO("IMGUI: hooked %s in %S", symbol, label);
        return true;
    }

    return false;
}

// Which module provides the NGX entry points moves between Streamline and NGX versions - and
// between upscaler mods - so try the usual providers by name and then fall back to scanning
// every loaded module. Calls fn(module, label) until it returns true.
template <class Fn>
static bool forEachNgxCandidateModule(Fn&& fn)
{
    static wchar_t const* const knownModules[] = {
        L"sl.interposer.dll",
        L"sl.dlss.dll",
        L"sl.dlss_g.dll",
        L"nvngx_dlss.dll",
        L"nvngx.dll",
        L"_nvngx.dll"
    };

    for (auto name : knownModules) {
        auto mod = GetModuleHandleW(name);
        if (mod != nullptr && fn(mod, name)) return true;
    }

    DWORD needed{ 0 };
    if (!EnumProcessModules(GetCurrentProcess(), nullptr, 0, &needed)) return false;

    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!EnumProcessModules(GetCurrentProcess(), mods.data(),
        (DWORD)(mods.size() * sizeof(HMODULE)), &needed)) return false;

    for (auto mod : mods) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(mod, path, MAX_PATH) == 0) continue;
        if (fn(mod, static_cast<wchar_t const*>(path))) return true;
    }

    return false;
}

void tryInstallNgxEvaluateFeatureHook()
{
    if (ngxEvaluateFeatureHook_.IsWrapped()) return;

    // NGX modules load lazily, so this has to keep retrying, but enumerating the module
    // list every frame is wasteful - probe periodically instead.
    if (ngxProbeDelay_ > 0) {
        ngxProbeDelay_--;
        return;
    }
    ngxProbeDelay_ = NgxProbeInterval;

    if (forEachNgxCandidateModule([this](HMODULE mod, wchar_t const* label) {
        return installNgxHookFrom(mod, label);
    })) return;

    if (!ngxProbeFailureLogged_) {
        ngxProbeFailureLogged_ = true;
        ERR("IMGUI: NVSDK_NGX_VULKAN_EvaluateFeature is not exported by any loaded module; "
            "the overlay will stay hidden while upscaling is active");
    }
}
```
`tryInstallNgxEvaluateFeatureHook()` is called every frame from the swapchain-create path — current-branch call site `Vulkan.inl:516`.

The hook type itself (`Vulkan.inl:87-88`, both branches):
```cpp
enum class NgxEvaluateFeatureCHookTag {};
using NgxEvaluateFeatureCHookType = WrappableFunction<NgxEvaluateFeatureCHookTag, NVSDK_NGX_Result(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C)>;
template<> NgxEvaluateFeatureCHookType* NgxEvaluateFeatureCHookType::gHook = nullptr;
```

### `ngxEvaluateFeatureCHook` signature and how it reads the NGX parameter block (current-branch, `Vulkan.inl:1161-1266`; identical logic in `ngx-experiments` at `/tmp/ngx-exp.inl:937-1000`)

```cpp
NVSDK_NGX_Result ngxEvaluateFeatureCHook(
    NgxEvaluateFeatureCHookType::BaseFuncType* orig,
    VkCommandBuffer InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    const NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback_C InCallback)
{
    // Call original first so NGX completes its work and final image state
    NVSDK_NGX_Result evalRes = orig(InCmdList, InFeatureHandle, InParameters, InCallback);
    ...
    // Resolve lazily and keep retrying until found - the providing module may not be loaded
    // yet the first time we get here, and caching a null would disable the overlay for good.
    if (ngxGetVoidPointer_ == nullptr) {
        forEachNgxCandidateModule([this](HMODULE mod, wchar_t const* label) {
            auto proc = reinterpret_cast<PFN_NVSDK_NGX_Parameter_GetVoidPointer>(
                GetProcAddress(mod, "NVSDK_NGX_Parameter_GetVoidPointer"));
            if (proc == nullptr) return false;
            ngxGetVoidPointer_ = proc;
            INFO("IMGUI: resolved NVSDK_NGX_Parameter_GetVoidPointer in %S", label);
            return true;
        });
    }
    ...
    // Extract NGX output resource as a Vulkan image view
    void* outPtr = nullptr;
    if (ngxGetVoidPointer_(const_cast<NVSDK_NGX_Parameter*>(InParameters), NVSDK_NGX_Parameter_Output, &outPtr) != NVSDK_NGX_Result_Success || !outPtr) {
        ...
    }

    auto* outResVK = reinterpret_cast<NVSDK_NGX_Resource_VK*>(outPtr);
    const NVSDK_NGX_ImageViewInfo_VK& iv = outResVK->Resource.ImageViewInfo;
    VkImageView  targetView   = iv.ImageView;
    VkImage      targetImage  = iv.Image;
    VkFormat     targetFormat = iv.Format;
    uint32_t     targetW      = iv.Width;
    uint32_t     targetH      = iv.Height;
    VkImageSubresourceRange range = iv.SubresourceRange;
    ...
```

Notable: the hook calls the real `orig()` **first**, then inspects the (now-populated) parameter block afterward — it reads the *output* NGX already produced, it does not currently modify any input before NGX runs.

`PFN_NVSDK_NGX_Parameter_GetVoidPointer` typedef (`Vulkan.inl:42`, current; `:39` ngx-experiments):
```cpp
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_NVSDK_NGX_Parameter_GetVoidPointer)(NVSDK_NGX_Parameter*, const char*, void**);
```

`NVSDK_NGX_Parameter_Output` usage — it's a locally-defined string macro, not the real NGX SDK's constant (no vendored NGX SDK header exists anywhere in this repo — confirmed, `find . -iname "nvsdk_ngx*"` returns nothing):
```cpp
#ifndef NVSDK_NGX_Parameter_Output
#define NVSDK_NGX_Parameter_Output "Output"
#endif
```
(`Vulkan.inl:37-39` current; `:34-36` ngx-experiments)

`NVSDK_NGX_Resource_VK` struct definition used to interpret a resource (`Vulkan.inl:30-70` current; `:27-67` ngx-experiments — identical):
```cpp
struct NVSDK_NGX_Handle { unsigned int Id; };

enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000
};

struct NVSDK_NGX_Parameter;
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_NVSDK_NGX_Parameter_GetVoidPointer)(NVSDK_NGX_Parameter*, const char*, void**);

enum NVSDK_NGX_Resource_VK_Type {
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW,
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER
};

struct NVSDK_NGX_ImageViewInfo_VK {
    VkImageView ImageView;
    VkImage Image;
    VkImageSubresourceRange SubresourceRange;
    VkFormat Format;
    unsigned int Width;
    unsigned int Height;
};

struct NVSDK_NGX_BufferInfo_VK {
    VkBuffer Buffer;
    unsigned int SizeInBytes;
};

struct NVSDK_NGX_Resource_VK {
    union {
        NVSDK_NGX_ImageViewInfo_VK ImageViewInfo;
        NVSDK_NGX_BufferInfo_VK BufferInfo;
    } Resource;
    NVSDK_NGX_Resource_VK_Type Type;
    bool ReadWrite;
};
```

### NGX parameter NAME strings present — and what's missing

Only **one** NGX parameter name string appears anywhere in either branch's `Vulkan.inl`: `"Output"` (via `NVSDK_NGX_Parameter_Output`). Grepped explicitly for `"Depth"`, `"MotionVectors"`, `"Jitter"`, `"MV Scale"`, `"Reset"` in both the current and `ngx-experiments` files — **zero hits**.

**Confirmed gap:** the standard NGX DLSS-SR input parameter names (`"Depth"`, `"MotionVectors"`, `"Jitter Offset X"` / `"Jitter Offset Y"`, `"MV Scale X"` / `"MV Scale Y"`, `"Reset"`) are not defined or read anywhere in this codebase and will need to be added — as local `#define`s in the same style as `NVSDK_NGX_Parameter_Output`, since no vendored NGX SDK header exists here to pull real constants from — plus corresponding `NVSDK_NGX_Parameter_GetVoidPointer`/`GetI`/`GetF`/`GetUI` style reads (only `GetVoidPointer` is currently wired up; the existing hook never reads a float/int/uint parameter, only pointers).

Also worth noting for the plan: this hook currently only *reads after* `orig()` runs (post-hook style, inspecting NGX's own output). Depth/motion-vector/jitter/MV-scale/reset are *inputs* NGX consumes — practically, those are more naturally supplied via Streamline's own tagging/constants APIs (`slSetTagForFrame`, `slSetConstants`, see Q4) rather than by mutating the raw `NVSDK_NGX_Parameter*` block inside this hook, since Streamline sits in front of NGX and is what BG3SE is already driving (Phase 1/2a manual-hooking work). Whether `ngxEvaluateFeatureCHook` needs to *write* into `InParameters` at all for Phase 2b (vs. just continuing to read `Output` for the overlay, while depth/mvec/jitter/reset flow through `slSetTagForFrame`/`slSetConstants` upstream of NGX) is a design question for the plan, not settled by this investigation.

---

## Q4 — Streamline tag + constants APIs (confirmed signatures)

All from `External/streamline/include/`.

### Core API function signatures — `sl_core_api.h`

```cpp
// sl_core_api.h:152
//! @param frame Specifies the frame this tag applies to. Frame token can be obtained using slGetNewFrameToken API.
//! @param viewport Specifies viewport this tag applies to
//! @param tags Pointer to resources tags, set to null to remove the specified tag
//! @param numTags Number of resource tags in the provided list
//! @param cmdBuffer Command buffer to use (optional and can be null if ALL tags are null or have eValidUntilPresent life-cycle)
//! IMPORTANT: GPU payload that generates content for the provided tag(s) MUST be either already submitted to the
//! provided command buffer or some other command buffer which is guaranteed, by the host application, to be
//! executed BEFORE the provided command buffer.
//! This method is thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* resources, uint32_t numResources, sl::CommandBuffer* cmdBuffer);

// sl_core_api.h:173 — deprecated, non-frame-token variant, still present
SL_API [[deprecated(...)]] sl::Result slSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);

// sl_core_api.h:185
//! @param values Common constants required by SL plugins (SL will keep a copy)
//! @param frame Index of the current frame
//! @param viewport Unique id (can be viewport id | instance id etc.)
//! This method is thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport);

// sl_core_api.h:316
//! @param handle Frame token to return
//! @param frameIndex Frame index (optional, if not provided SL internal frame counting is used)
//! NOTE: Normally SL would not expect more that 3 frames in flight due to added latency.
//! This method is thread safe.
SL_API sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex = nullptr);
```

Use `slSetTagForFrame` (not the deprecated `slSetTag`) — it requires `sl::PreferenceFlags::eUseFrameBasedResourceTagging` to be set (noted right above its declaration, `sl_core_api.h:136`).

### `sl::Resource` — constructors (there is no separate "Vulkan image" constructor; these are the general ones, `sl_core_types.h:329-378`)

```cpp
SL_STRUCT_BEGIN(Resource, StructType(...), kStructVersion1)
    //! Resource type, native pointer are MANDATORY always
    //! Resource state is MANDATORY unless using D3D11
    //! Resource view, description etc. are MANDATORY only when using Vulkan
    Resource(ResourceType _type, void* _native, void* _mem, void* _view, uint32_t _state = UINT_MAX)
        : BaseStructure(Resource::s_structType, kStructVersion1), type(_type), native(_native), memory(_mem), view(_view), state(_state) {};
    Resource(ResourceType _type, void* _native, uint32_t _state = UINT_MAX)
        : BaseStructure(Resource::s_structType, kStructVersion1), type(_type), native(_native), state(_state) {};

    ResourceType type = ResourceType::eTex2d;
    void* native{};        // ID3D11Resource/ID3D12Resource/VkBuffer/VkImage
    void* memory{};        // vkDeviceMemory or nullptr
    void* view{};           // VkImageView/VkBufferView or nullptr
    uint32_t state = UINT_MAX;   // D3D12_RESOURCE_STATES or VkImageLayout (MANDATORY, must be correct)
    uint32_t width{};
    uint32_t height{};
    uint32_t nativeFormat{};
    uint32_t mipLevels{};
    uint32_t arrayLayers{};
    uint64_t gpuVirtualAddress{};
    uint32_t flags;          // VkImageCreateFlags
    uint32_t usage{};        // VkImageUsageFlags
    uint32_t reserved{};
SL_STRUCT_END()
```
For Vulkan a depth/motion-vector tag would use the 4-arg constructor: `sl::Resource(sl::ResourceType::eTex2d, (void*)vkImage, /*mem*/nullptr, (void*)vkImageView, (uint32_t)vkImageLayout)`, per the doc comment "Resource view, description etc. are MANDATORY only when using Vulkan".

### `sl::ResourceTag` (`sl_core_types.h:401-418`)

```cpp
SL_STRUCT_BEGIN(ResourceTag, StructType(...), kStructVersion1)
    ResourceTag(Resource* r, BufferType t, ResourceLifecycle l, const Extent* e = nullptr)
        : BaseStructure(ResourceTag::s_structType, kStructVersion1), resource(r), type(t), lifecycle(l)
    { if (e) extent = *e; };

    Resource* resource{};
    BufferType type{};
    ResourceLifecycle lifecycle{};
    Extent extent{};
SL_STRUCT_END()
```

### `sl::ResourceLifecycle` enum (`sl_core_types.h:386-394`)

```cpp
enum ResourceLifecycle
{
    eOnlyValidNow,        // Resource can change/be destroyed/reused after being provided to SL
    eValidUntilPresent,   // Resource does NOT change until the frame is presented
    eValidUntilEvaluate   // Resource does NOT change until after slEvaluateFeature returns
};
```
Guidance from the header itself (`sl_core_types.h:382-385`): use `eOnlyValidNow`/`eValidUntilEvaluate` only when really needed (can force SL to make VRAM copies); for features like DLSS-G, prefer `eValidUntilPresent` by default.

### `sl::Extent` (`sl_consts.h:143-169`)

```cpp
struct Extent
{
    uint32_t top{};
    uint32_t left{};
    uint32_t width{};
    uint32_t height{};
    inline operator bool() const { return width != 0 && height != 0; }
    ...
};
```
Pass `nullptr`/default `Extent{}` to tag the whole resource.

### `sl::BufferType` constants to tag (`sl_core_types.h:57-68`)

```cpp
using BufferType = uint32_t;
//! Depth buffer - IMPORTANT - Must be suitable to use with clipToPrevClip transformation (see Constants below)
constexpr BufferType kBufferTypeDepth = 0;
//! Object and optional camera motion vectors (see Constants below)
constexpr BufferType kBufferTypeMotionVectors = 1;
```
Confirmed: `kBufferTypeDepth = 0` and `kBufferTypeMotionVectors = 1` are the two buffer types Phase 2b needs to tag via `slSetTagForFrame`. (Full list runs `sl_core_types.h:66-214`; also present but not required for basic DLSS-SR: `kBufferTypeHUDLessColor=2`, `kBufferTypeScalingInputColor=3`, `kBufferTypeScalingOutputColor=4`, `kBufferTypeExposure=13`, etc.)

### `sl::Constants` (`sl_consts.h:182-258`) — for cross-reference with Q1

```cpp
SL_STRUCT_BEGIN(Constants, StructType(...), kStructVersion2)
    float4x4 cameraViewToClip;
    float4x4 clipToCameraView;
    float4x4 clipToLensClip;          // optional
    float4x4 clipToPrevClip;          // clipToView * viewToViewPrev * viewToClipPrev
    float4x4 prevClipToClip;          // = clipToPrevClip.inverse()

    float2 jitterOffset;
    float2 mvecScale;
    float2 cameraPinholeOffset;       // optional
    float3 cameraPos;
    float3 cameraUp;
    float3 cameraRight;
    float3 cameraFwd;

    float cameraNear = INVALID_FLOAT;
    float cameraFar = INVALID_FLOAT;
    float cameraFOV = INVALID_FLOAT;
    float cameraAspectRatio = INVALID_FLOAT;
    float motionVectorsInvalidValue = INVALID_FLOAT;

    Boolean depthInverted = Boolean::eInvalid;
    Boolean cameraMotionIncluded = Boolean::eInvalid;
    Boolean motionVectors3D = Boolean::eInvalid;
    Boolean reset = Boolean::eInvalid;
    Boolean orthographicProjection = Boolean::eFalse;
    Boolean motionVectorsDilated = Boolean::eFalse;
    Boolean motionVectorsJittered = Boolean::eFalse;

    float minRelativeLinearDepthObjectSeparation = 40.0f;   // v2, default 40.0
SL_STRUCT_END()
```
Direct tie back to Q1: `cameraViewToClip`/`clipToCameraView` map to `rf::Camera::ProjectionMatrix`/`InvProjectionMatrix` (`Camera.h:119-120`) reached via `CameraComponent->Controller->Camera`; `cameraPos`/`cameraUp`/`cameraRight`/`cameraFwd` need to come from `CameraController::GetWorldTranslate()`/`GetWorldRotate()`/`WorldTransform` (`Camera.h:137,139,141,162`), not from the `rf::Camera` matrices — the matrices give view/projection, not decomposed basis vectors. `sl_matrix_helpers.h` (same include dir) has `matrixMul` and other helpers useful for deriving `clipToPrevClip` from consecutive frames' matrices, referenced in the doc comment above `clipToPrevClip` ("Sample code can be found in sl_matrix_helpers.h").

Confirmed: no existing code in this repo calls `slSetTagForFrame`, `slSetConstants`, `slGetNewFrameToken`, or constructs `sl::Constants`/`sl::ResourceTag` anywhere under `BG3Extender/` — this is greenfield for Phase 2b (checked via grep across `BG3Extender/Extender/Client/IMGUI/`).
