#include <stdafx.h>
#include <Extender/Client/ScriptExtenderClient.h>
#include <Extender/ScriptExtender.h>
#include <Extender/Version.h>
#include <Extender/Client/IMGUI/Streamline.h>
#include <GameDefinitions/Components/Camera.h>
#include <shlwapi.h>

#define STATIC_HOOK(name) decltype(bg3se::ecl::ScriptExtender::name) * decltype(bg3se::ecl::ScriptExtender::name)::gHook;
STATIC_HOOK(gameStateWorkerStart_)
STATIC_HOOK(gameStateMachineUpdate_)

#include <Extender/Shared/ThreadedExtenderState.inl>

BEGIN_SE()

void InitCrashReporting();

extern char const* BuildDate;

END_SE()

BEGIN_NS(ecl)

char const * GameStateNames[] =
{
    "Unknown",
    "Init",
    "InitMenu",
    "InitNetwork",
    "InitConnection",
    "Idle",
    "LoadMenu",
    "Menu",
    "Exit",
    "SwapLevel",
    "LoadLevel",
    "LoadModule",
    "LoadSession",
    "UnloadLevel",
    "UnloadModule",
    "UnloadSession",
    "Paused",
    "PrepareRunning",
    "Running",
    "Disconnect",
    "Join",
    "Save",
    "StartLoading",
    "StopLoading",
    "StartServer",
    "Movie",
    "Installation",
    "ModReceiving",
    "Lobby",
    "BuildStory",
    "GeneratePsoCache",
    "LoadPsoCache",
    "AnalyticsSessionEnd"
};

ScriptExtender::ScriptExtender(ExtenderConfig& config)
    : ThreadedExtenderState(ContextType::Client),
    config_(config)
{
}

void ScriptExtender::Initialize()
{
    // Wrap state change functions even if extension startup failed, otherwise
    // we won't be able to show any startup errors

    auto& lib = GetStaticSymbols();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (lib.ecl__GameStateThreaded__GameStateWorker__DoWork != nullptr) {
        gameStateWorkerStart_.Wrap(lib.ecl__GameStateThreaded__GameStateWorker__DoWork);
    }

    if (lib.ecl__GameStateMachine__Update != nullptr) {
        gameStateMachineUpdate_.Wrap(lib.ecl__GameStateMachine__Update);
    }

    DetourTransactionCommit();

    gameStateWorkerStart_.SetWrapper(&ScriptExtender::GameStateWorkerWrapper, this);
    gameStateMachineUpdate_.SetPrePostHook(&ScriptExtender::OnPreUpdate, &ScriptExtender::OnUpdate, this);

    sdl_.EnableHooks();
}

void ScriptExtender::Shutdown()
{
    DEBUG("ecl::ScriptExtender::Shutdown: Exiting");
    sdl_.DisableHooks();
    ContextGuardAnyThread _(ContextType::Client);
    ResetExtensionState();
}

void ScriptExtender::PostStartup()
{
    if (postStartupDone_) return;

    OPTICK_EVENT();
    entityHelpers_.Setup();
    visualHelpers_.Setup();
    gExtender->GetPropertyMapManager().RegisterComponents(entityHelpers_);
    postStartupDone_ = true;
}

bool IsLoadingState(GameState state)
{
    return state == GameState::Init
        || state == GameState::InitMenu
        || state == GameState::InitNetwork
        || state == GameState::InitConnection
        || state == GameState::LoadMenu
        || state == GameState::SwapLevel
        || state == GameState::LoadLevel
        || state == GameState::LoadModule
        || state == GameState::LoadSession
        || state == GameState::UnloadLevel
        || state == GameState::UnloadModule
        || state == GameState::UnloadSession
        || state == GameState::PrepareRunning
        || state == GameState::Installation
        || state == GameState::ModReceiving;
}

// Undo "ModCrashSanityCheck" mechanism that disables mods on an unclean exit
void CleanupSanityCheck()
{
    auto path = GetStaticSymbols().ToPath("ModCrashSanityCheck", PathRootType::UserProfile);
    auto pathw = FromUTF8(path);
    if (PathIsDirectoryW(pathw.c_str())) {
        RemoveDirectoryW(pathw.c_str());
        INFO("Removed ModCrashSanityCheck");
    }
}

void ScriptExtender::OnGameStateChanged(GameState fromState, GameState toState)
{
    if (gExtender->GetConfig().SendCrashReports) {
        // We need to initialize the crash reporter after the game engine has started,
        // otherwise the game will overwrite the top level exception filter
        InitCrashReporting();
    }

    // Check to make sure that startup is done even if the extender was loaded when the game was already in GameState::Init
    if (toState != GameState::Unknown
        && toState != GameState::StartLoading
        && toState != GameState::InitMenu
        && !gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        // We need to initialize the function library here, as GlobalAllocator isn't available in Init().
        gExtender->PostStartup();
    }

    if (toState == GameState::Menu
        && gExtender->GetLibraryManager().InitializationFailed()) {
        gExtender->PostStartup();
    }

    if ((toState == GameState::InitMenu || toState == GameState::StartLoading)
        && !gExtender->GetLibraryManager().CriticalInitializationFailed()
        && gExtender->GetConfig().InsanityCheck) {
        CleanupSanityCheck();
    }

#if defined(DEBUG_SERVER_CLIENT)
    DEBUG("ecl::ScriptExtender::OnGameStateChanged(): %s -> %s", 
        GameStateNames[(unsigned)fromState], GameStateNames[(unsigned)toState]);
#endif

    if (fromState != GameState::Unknown) {
        BindToThreadPersistent();
    }

    if (gExtender->WasInitialized() && !gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        if (IsLoadingState(toState)) {
            UpdateClientProgress(EnumInfo<GameState>::Find(toState).GetString());
        } else {
            UpdateClientProgress("");
        }
    }

    switch (fromState) {
    case GameState::LoadModule:
        INFO("ecl::ScriptExtender::OnGameStateChanged(): Loaded module");
        gExtender->GetVirtualTextureHelpers().Load();
        ShowVersionNumber();
        LoadExtensionState(ExtensionStateContext::Game);
        break;

    // Initialize client state when exiting from a game and returning to menu
    case GameState::LoadMenu:
        LoadExtensionState(ExtensionStateContext::Game);
        break;

    case GameState::LoadSession:
        if (extensionState_) {
            extensionState_->OnGameSessionLoaded();
        }
        break;

    case GameState::InitNetwork:
        network_.ExtendNetworking();
        break;
    }

    switch (toState) {
    case GameState::Init:
        entityHelpers_.Bind();
        ResetExtensionState();
        break;

    case GameState::InitNetwork:
    case GameState::Disconnect:
        network_.Reset();
        break;

    case GameState::UnloadSession:
        INFO("ecl::ScriptExtender::OnGameStateChanged(): Unloading session");
        entityHelpers_.OnDestroy();
        ResetExtensionState();
        break;

    case GameState::Menu:
    {
        auto error = gExtender->GetUpdaterAPI().GetDisplayError();
        if (error) {
            gExtender->GetLibraryManager().ShowStartupError(STDString(*error), false, false);
        } else {
            #if defined(SE_IS_DEVELOPER_BUILD) && defined(NDEBUG)
            if (!gExtender->GetConfig().DeveloperMode) {
                gExtender->GetLibraryManager().ShowStartupError("This is an experimental version of the Script Extender meant for development use; things may frequently break here. It is recommended to switch back to the Release version unless you know what you are doing!", false, false);
            }
            #endif
        }
        break;
    }

    case GameState::LoadModule:
        gExtender->InitRuntimeLogging();
        break;

    case GameState::LoadSession:
        INFO("ecl::ScriptExtender::OnClientGameStateChanged(): Loading game session");
        LoadExtensionState(ExtensionStateContext::Game);
        network_.ExtendNetworking();
        entityHelpers_.OnInit();
        if (extensionState_) {
            extensionState_->OnGameSessionLoading();
        }
        break;

    case GameState::LoadLevel:
        if (extensionState_) {
            LuaClientPin lua(*extensionState_);
            if (lua) {
                lua->OnLevelLoading();
            }
        }
        break;
    }

    if (extensionState_) {
        LuaClientPin lua(*extensionState_);
        if (lua) {
            lua->OnGameStateChanged(fromState, toState);
        }
    }
}

void ScriptExtender::GameStateWorkerWrapper(void (*wrapped)(void*), void* self)
{
    ContextGuard _(ContextType::Client);
    wrapped(self);
}

namespace
{
    // Enumerate every entity carrying a CameraComponent. Direct, non-Lua port of
    // lua::entity::GetAllEntitiesWithComponent (BG3Extender/Lua/Libs/Entity.inl) restricted to
    // CameraComponent: that component is not one-frame (DEFINE_COMPONENT default, Base.h:45-49),
    // so the one-frame branches from the Lua original are omitted here.
    void CollectCameraEntities(ecs::EntitySystemHelpersBase& helpers, ecs::EntityWorld& world, std::vector<EntityHandle>& out)
    {
        auto componentType = helpers.GetComponentIndex(CameraComponent::ComponentType);
        if (!componentType) return;

        auto const& meta = helpers.GetComponentMeta(CameraComponent::ComponentType);
        if (meta.SingleComponentQuery != ecs::UndefinedQuery) {
            auto& query = world.Queries.Queries[(unsigned)meta.SingleComponentQuery];
            for (auto const& storage : query.EntityStorages.values()) {
                if (storage.Storage == nullptr) continue;
                for (auto const& handle : storage.Storage->InstanceToPageMap.keys()) {
                    out.push_back(handle);
                }
            }
        } else if (world.Storage != nullptr) {
            for (auto cls : world.Storage->Storages) {
                if (cls != nullptr && cls->ComponentTypeToIndex.try_get(*componentType)) {
                    for (auto const& handle : cls->InstanceToPageMap.keys()) {
                        out.push_back(handle);
                    }
                }
            }
        }
    }

    // Resolve the single active CameraComponent (CameraComponent::Active, Camera.h:294) and
    // copy its render-thread-unsafe matrices/controller state into a plain POD snapshot.
    // GAME THREAD ONLY - see Docs/superpowers/2026-08-15-phase2b-investigation.md Q2 for why
    // this must never run from the Vulkan/NGX render hook. Never throws: every ECS access here
    // is null-checked, and the caller additionally wraps this in try/catch as defense in depth.
    extui::CameraSnapshot ResolveCameraSnapshot()
    {
        extui::CameraSnapshot snap;

        auto& helpers = gExtender->GetClient().GetEntityHelpers();
        if (!helpers.HasEntityWorld()) return snap;

        auto world = helpers.GetEntityWorld();
        if (world == nullptr) return snap;

        std::vector<EntityHandle> candidates;
        CollectCameraEntities(helpers, *world, candidates);

        CameraComponent* active = nullptr;
        for (auto const& handle : candidates) {
            auto* cam = helpers.GetComponent<CameraComponent>(handle);
            if (cam != nullptr && cam->Active) {
                active = cam;
                break;
            }
        }

        if (active == nullptr || active->Controller == nullptr) {
            return snap;
        }

        auto* controller = active->Controller;
        auto const& cam = controller->Camera;

        snap.view = cam.ViewMatrix;
        snap.invView = cam.InvViewMatrix;
        snap.proj = cam.ProjectionMatrix;
        snap.invProj = cam.InvProjectionMatrix;
        snap.pos = controller->GetWorldTranslate();
        auto rot = controller->GetWorldRotate();
        snap.right = rot * glm::vec3(1.0f, 0.0f, 0.0f);
        snap.up = rot * glm::vec3(0.0f, 1.0f, 0.0f);
        snap.fwd = rot * glm::vec3(0.0f, 0.0f, -1.0f);
        snap.nearP = controller->NearPlane;
        snap.farP = controller->FarPlane;
        snap.fov = controller->FOV;
        snap.aspect = controller->AspectRatio;
        snap.valid = true;
        return snap;
    }

    // Called once per game-thread tick from OnUpdateGuarded. Only does any work when
    // StreamlineEnabled && StreamlineFGEnabled are both set; otherwise it's a no-op (no
    // snapshot is produced or published, per the phase 2b gating requirement).
    void UpdateCameraSnapshot()
    {
        auto& config = gExtender->GetConfig();
        if (!config.StreamlineEnabled || !config.StreamlineFGEnabled) return;

        auto* sl = extui::StreamlineManager::Get();
        if (sl == nullptr) return;

        extui::CameraSnapshot snap;
        try {
            snap = ResolveCameraSnapshot();
        } catch (...) {
            // Never let a resolution failure propagate into the game loop - publish an
            // explicitly-invalid snapshot instead (the render thread skips constants for it).
            snap = extui::CameraSnapshot{};
        }

        static bool firstValidLogged = false;
        static bool failureLogged = false;
        if (snap.valid) {
            if (!firstValidLogged) {
                firstValidLogged = true;
                sl->Note("camera snapshot: first valid frame - fov=%.2f near=%.3f far=%.1f aspect=%.3f proj[0][0]=%.4f proj[1][1]=%.4f",
                    snap.fov, snap.nearP, snap.farP, snap.aspect, snap.proj[0][0], snap.proj[1][1]);
            }
        } else if (!failureLogged) {
            failureLogged = true;
            sl->Note("camera snapshot: could not resolve active camera");
        }

        sl->SetCameraSnapshot(snap);
    }
}

#if USE_OPTICK
std::unique_ptr< ::Optick::Event> frameEvent;
#endif

void ScriptExtender::OnPreUpdate(void* self, GameTime* time)
{
#if USE_OPTICK
    static ::Optick::ThreadScope mainThreadScope("Client");
    frameEvent.reset();
    ::Optick::EndFrame();
    ::Optick::Update();

    auto frameNumber = ::Optick::BeginFrame();
    frameEvent = std::make_unique<::Optick::Event>(*::Optick::GetFrameDescription());
    OPTICK_TAG("Frame", frameNumber);
#endif
}

void ScriptExtender::OnUpdate(void* self, GameTime* time)
{
    BEGIN_GUARDED()
    // In case we're loaded too late to see LoadModule transition
    BindToThreadPersistent();
    OnUpdateGuarded(self, time);
    END_GUARDED()
}

void ScriptExtender::OnUpdateGuarded(void* self, GameTime* time)
{
    network_.Update();
    RunPendingTasks();
    gExtender->IMGUI().Update();
    UpdateCameraSnapshot();
    if (extensionState_) {
        extensionState_->OnUpdate(*time);
        if (gExtender->GetLuaDebugger()) {
            gExtender->GetLuaDebugger()->ClientTick();
        }
    }

}

void ScriptExtender::OnIncLocalProgress(void* self, int progress, char const* state)
{
    if (strcmp(state, "EffectManager") != 0) {
        UpdateClientProgress(state);
    }
    else {
        UpdateClientProgress("");
    }
}

void ScriptExtender::UpdateServerProgress(STDString const& status)
{
    serverStatus_ = status;
    ShowLoadingProgress();
}

void ScriptExtender::UpdateClientProgress(STDString const& status)
{
    clientStatus_ = status;
    ShowLoadingProgress();
}

void ScriptExtender::ShowLoadingProgress()
{
    // FIXME - not supported for now
}

void ScriptExtender::ShowVersionNumber()
{
    RuntimeStringHandle rsh(FixedString("h5b6e4138g2cf0g4d67gb825gee416cf8c54f"));
    auto versionText = GetStaticSymbols().GetTranslatedStringRepository()->GetTranslatedString(rsh);
    if (versionText && !STDString(*versionText).contains("Script Extender")) {
        auto expandedVersion = STDString(*versionText) +
            "\r\nScript Extender v" + STDString(std::to_string(CurrentVersion)) + " loaded, built on " + BuildDate + ".";
        GetStaticSymbols().GetTranslatedStringRepository()->UpdateTranslatedString(rsh, expandedVersion);
    }
}

void ScriptExtender::ResetLuaState()
{
    if (extensionState_ && extensionState_->GetLua()) {
        auto ext = extensionState_.get();

        ext->AddPostResetCallback([&ext]() {
            ext->OnModuleResume();
            auto state = GetStaticSymbols().GetClientState();
            if (state && (state == GameState::Paused || state == GameState::Running)) {
                ext->OnGameSessionLoading();
                ext->OnGameSessionLoaded();
            }
            ext->OnResetCompleted();
        });
        ext->RequestLuaReset(true);
    }
}

void ScriptExtender::ResetExtensionState()
{
    OPTICK_EVENT();
    network_.OnResetExtensionState();
    extensionState_.reset();
    extensionState_ = std::make_unique<ExtensionState>();
    extensionState_->Reset();
    gExtender->ClearPathOverrides();
    extensionLoaded_ = false;
}

void ScriptExtender::LoadExtensionState(ExtensionStateContext ctx)
{
    if (extensionLoaded_ && (!extensionState_ || ctx == extensionState_->Context())) {
        return;
    }

    OPTICK_EVENT(Optick::Category::IO);
    PostStartup();

    if (!extensionState_) {
        ResetExtensionState();
    }

    extensionState_->LoadConfigs();

    if (!gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        OsiMsg("Initializing client with target context " << ContextToString(ctx));
        gExtender->GetLibraryManager().ApplyCodePatches();
        //networkManager_.ExtendNetworkingClient();
        extensionState_->RequestLuaReset(ctx, true);
    }

    extensionLoaded_ = true;
}

END_NS()
