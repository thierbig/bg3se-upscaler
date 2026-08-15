#pragma once

#include <GameDefinitions/Base/Base.h>
#include <Extender/Client/IMGUI/Backends.h>
#include <Extender/ScriptExtender.h>
#include <CoreLib/Wrappers.h>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <unordered_map>
#include <iterator>
#include <cstdlib>
#include <vector>
#include <psapi.h>
#include <Extender/Client/IMGUI/Streamline.h>

#ifndef NVSDK_CONV
#ifdef __GNUC__
#define NVSDK_CONV
#else
#define NVSDK_CONV __cdecl
#endif
#endif

extern "C" {
    typedef void (NVSDK_CONV *PFN_NVSDK_NGX_ProgressCallback_C)(float, bool*);
}

struct NVSDK_NGX_Handle { unsigned int Id; };

enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000
};

#ifndef NVSDK_NGX_Parameter_Output
#define NVSDK_NGX_Parameter_Output "Output"
#endif

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

BEGIN_SE()

#define VK_HOOK(name) enum class Vk##name##HookTag {}; \
    using Vk##name##HookType = WrappableFunction<Vk##name##HookTag, decltype(vk##name)>; \
    Vk##name##HookType* Vk##name##HookType::gHook;

VK_HOOK(CreateInstance)
VK_HOOK(CreateDevice)
VK_HOOK(DestroyDevice)
VK_HOOK(CreatePipelineCache)
VK_HOOK(CreateSwapchainKHR)
VK_HOOK(DestroySwapchainKHR)
VK_HOOK(QueuePresentKHR)

enum class NgxEvaluateFeatureCHookTag {};
using NgxEvaluateFeatureCHookType = WrappableFunction<NgxEvaluateFeatureCHookTag, NVSDK_NGX_Result(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C)>;
template<> NgxEvaluateFeatureCHookType* NgxEvaluateFeatureCHookType::gHook = nullptr;

END_SE()


#define VK_ACCESS_ALL_READ_BITS                                                        \
  (VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |                    \
   VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |                  \
   VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |                   \
   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | \
   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT)

#define VK_CHECK(expr) { auto _rval = (expr); if (_rval != VK_SUCCESS) { ERR(#expr " failed: %d", _rval); } }

BEGIN_NS(extui)

class VulkanBackend : public RenderingBackend
{
public:
    static constexpr unsigned TextureSoftCap = 2000;
    static constexpr unsigned TextureHardCap = 2048;

    VulkanBackend(IMGUIManager& ui) : ui_(ui) {}

    ~VulkanBackend() override
    {
        IMGUI_DEBUG("Destroying VK backend");
        releaseSwapChain(swapchain_);
        DestroyUI();
        DisableHooks();
    }

    void EnableHooks() override
    {
        if (CreateInstanceHook_.IsWrapped()) return;

        IMGUI_DEBUG("VulkanBackend::EnableHooks()");

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        auto createInstance = vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
        CreateInstanceHook_.Wrap(ResolveFunctionTrampoline(createInstance));
        DetourTransactionCommit();

        // Instance and device creation are wrapped (not post-hooked); Streamline is initialized
        // at first vkCreateInstance in manual-hooking mode; see the *Wrapped methods.
        CreateInstanceHook_.SetWrapper(&VulkanBackend::vkCreateInstanceWrapped, this);
        CreateDeviceHook_.SetWrapper(&VulkanBackend::vkCreateDeviceWrapped, this);
        DestroyDeviceHook_.SetPreHook(&VulkanBackend::vkDestroyDeviceHooked, this);
        CreatePipelineCacheHook_.SetPostHook(&VulkanBackend::vkCreatePipelineCacheHooked, this);
        CreateSwapchainKHRHook_.SetPostHook(&VulkanBackend::vkCreateSwapchainKHRHooked, this);
        DestroySwapchainKHRHook_.SetPreHook(&VulkanBackend::vkDestroySwapchainKHRHooked, this);
        QueuePresentKHRHook_.SetPreHook(&VulkanBackend::vkQueuePresentKHRHooked, this);

        // The extender drives Streamline itself; PureDark's upscaler.dll is no longer loaded
        // on this branch (it was only ever loaded from right here). Nothing SL-related happens
        // here: loading the interposer this early lets it observe the game's pre-instance
        // loader calls and slInit then refuses with 'APIs invoked before slInit' (24), and
        // process startup is too early for NVAPI besides (driver version reads 0). Both load
        // and init happen at the first vkCreateInstance, which is where the working stack did
        // them.
    }

    VkResult vkCreateInstanceWrapped(
        VkCreateInstanceHookType::BaseFuncType* orig,
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance)
    {
        // Load + init deferred to here (NVAPI is dead earlier and an early-loaded interposer
        // makes slInit refuse); attempted exactly once - retrying a failed slInit crashes.
        if (!slInitAttempted_) {
            slInitAttempted_ = true;
            if (streamline_.Load() && streamline_.Init()) {
                sl_ = streamline_.Module();
            }
        }

        VkResult result;
        auto const& reqs = streamline_.Requirements();
        if (streamline_.Ready() && reqs.valid && !reqs.instanceExtensions.empty()) {
            std::vector<char const*> extensions(
                pCreateInfo->ppEnabledExtensionNames,
                pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
            for (auto const& wanted : reqs.instanceExtensions) {
                bool present = false;
                for (auto existing : extensions) {
                    if (wanted == existing) { present = true; break; }
                }
                if (!present) extensions.push_back(wanted.c_str());
            }
            VkInstanceCreateInfo extended = *pCreateInfo;
            extended.enabledExtensionCount = (uint32_t)extensions.size();
            extended.ppEnabledExtensionNames = extensions.data();
            INFO("SL: instance create extended with %u extension(s)",
                (unsigned)(extensions.size() - pCreateInfo->enabledExtensionCount));
            result = orig(&extended, pAllocator, pInstance);
            if (result != VK_SUCCESS) {
                INFO("SL: extended vkCreateInstance failed: %d - retrying vanilla", (int)result);
                streamline_.Disable("extended vkCreateInstance failed, retrying vanilla");
                result = orig(pCreateInfo, pAllocator, pInstance);
            }
        } else {
            result = orig(pCreateInfo, pAllocator, pInstance);
        }
        vkCreateInstanceHooked(pCreateInfo, pAllocator, pInstance, result);
        return result;
    }

    VkResult vkCreateDeviceWrapped(
        VkCreateDeviceHookType::BaseFuncType* orig,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice)
    {
        VkResult result;
        auto const& reqs = streamline_.Requirements();
        if (streamline_.Ready() && reqs.valid) {
            // --- extensions (additive, dedup) ---
            std::vector<char const*> extensions(
                pCreateInfo->ppEnabledExtensionNames,
                pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
            for (auto const& wanted : reqs.deviceExtensions) {
                bool present = false;
                for (auto existing : extensions) {
                    if (wanted == existing) { present = true; break; }
                }
                if (!present) extensions.push_back(wanted.c_str());
            }

            // --- features 1.2/1.3: mutate-and-restore or prepend ---
            std::vector<char const*> f12Names;
            for (auto const& f : reqs.features12) f12Names.push_back(f.c_str());
            std::vector<char const*> f13Names;
            for (auto const& f : reqs.features13) f13Names.push_back(f.c_str());
            auto slF12 = sl::getVkPhysicalDeviceVulkan12Features((uint32_t)reqs.features12.size(), f12Names.data());
            auto slF13 = sl::getVkPhysicalDeviceVulkan13Features((uint32_t)reqs.features13.size(), f13Names.data());

            VkPhysicalDeviceVulkan12Features* gameF12{ nullptr };
            VkPhysicalDeviceVulkan13Features* gameF13{ nullptr };
            for (auto node = (VkBaseOutStructure*)pCreateInfo->pNext; node; node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) gameF12 = (VkPhysicalDeviceVulkan12Features*)node;
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) gameF13 = (VkPhysicalDeviceVulkan13Features*)node;
            }

            // OR VkBool32 payloads (skip sType+pNext header) into dst; returns saved copy.
            auto orFeatures = [](void* dst, void const* src, size_t size) {
                std::vector<uint8_t> saved((uint8_t*)dst, (uint8_t*)dst + size);
                auto* d = (uint32_t*)((uint8_t*)dst + offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
                auto* s = (uint32_t const*)((uint8_t const*)src + offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
                auto count = (size - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(uint32_t);
                for (size_t i = 0; i < count; i++) d[i] |= s[i];
                return saved;
            };

            std::vector<uint8_t> savedF12, savedF13;
            if (gameF12 != nullptr) savedF12 = orFeatures(gameF12, &slF12, sizeof(slF12));
            if (gameF13 != nullptr) {
                std::vector<uint8_t> saved((uint8_t*)gameF13, (uint8_t*)gameF13 + sizeof(*gameF13));
                auto* d = (VkBool32*)&gameF13->robustImageAccess;
                auto* s = (VkBool32 const*)&slF13.robustImageAccess;
                auto count = (sizeof(*gameF13) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32);
                for (size_t i = 0; i < count; i++) d[i] |= s[i];
                savedF13 = std::move(saved);
            }

            // Restore game-owned structs to their original (un-OR'd) state. Must run before any
            // vanilla fallback retry - otherwise the "vanilla" create-info still carries SL's
            // feature bits, which can make the fallback fail identically to the extended attempt
            // it was supposed to rescue. Idempotent, so it is also safe to call again at the end.
            auto restoreGameFeatures = [&]() {
                if (gameF12 != nullptr) std::copy(savedF12.begin(), savedF12.end(), (uint8_t*)gameF12);
                if (gameF13 != nullptr) std::copy(savedF13.begin(), savedF13.end(), (uint8_t*)gameF13);
            };

            VkDeviceCreateInfo extended = *pCreateInfo;
            // Prepend structs the game does not chain (head insertion, no cloning).
            if (gameF12 == nullptr && !reqs.features12.empty()) { slF12.pNext = const_cast<void*>(extended.pNext); extended.pNext = &slF12; }
            if (gameF13 == nullptr && !reqs.features13.empty()) { slF13.pNext = const_cast<void*>(extended.pNext); extended.pNext = &slF13; }

            // --- queues: extend counts, record SL slots ---
            uint32_t familyCount{ 0 };
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

            auto findFamily = [&](VkQueueFlags required, VkQueueFlags preferAbsent) -> uint32_t {
                uint32_t fallback = ~0u;
                for (uint32_t i = 0; i < familyCount; i++) {
                    if ((families[i].queueFlags & required) != required) continue;
                    if ((families[i].queueFlags & preferAbsent) == 0) return i;
                    if (fallback == ~0u) fallback = i;
                }
                return fallback;
            };

            std::vector<VkDeviceQueueCreateInfo> queues(
                pCreateInfo->pQueueCreateInfos,
                pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
            std::vector<std::vector<float>> priorityStorage;
            bool queueFailure = false;

            auto addQueues = [&](uint32_t family, uint32_t extra, uint32_t& outIndex) {
                if (extra == 0 || family == ~0u) { if (extra > 0) queueFailure = true; return; }
                for (auto& q : queues) {
                    if (q.queueFamilyIndex != family) continue;
                    if (q.queueCount + extra > families[family].queueCount) { queueFailure = true; return; }
                    outIndex = q.queueCount;
                    priorityStorage.emplace_back(q.queueCount + extra, 1.0f);
                    if (q.pQueuePriorities != nullptr)
                        std::copy(q.pQueuePriorities, q.pQueuePriorities + q.queueCount, priorityStorage.back().begin());
                    q.queueCount += extra;
                    q.pQueuePriorities = priorityStorage.back().data();
                    return;
                }
                if (extra > families[family].queueCount) { queueFailure = true; return; }
                outIndex = 0;
                priorityStorage.emplace_back(extra, 1.0f);
                queues.push_back(VkDeviceQueueCreateInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    nullptr, 0, family, extra, priorityStorage.back().data() });
            };

            slQueueSlots_ = {};
            if (reqs.graphicsQueues > 0) {
                slQueueSlots_.graphicsFamily = findFamily(VK_QUEUE_GRAPHICS_BIT, 0);
                addQueues(slQueueSlots_.graphicsFamily, reqs.graphicsQueues, slQueueSlots_.graphicsIndex);
            }
            if (reqs.computeQueues > 0) {
                slQueueSlots_.computeFamily = findFamily(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
                addQueues(slQueueSlots_.computeFamily, reqs.computeQueues, slQueueSlots_.computeIndex);
            }
            if (reqs.opticalFlowQueues > 0) {
                slQueueSlots_.opticalFlowFamily = findFamily(VK_QUEUE_OPTICAL_FLOW_BIT_NV, 0);
                slQueueSlots_.opticalFlowNative = slQueueSlots_.opticalFlowFamily != ~0u;
                addQueues(slQueueSlots_.opticalFlowFamily, reqs.opticalFlowQueues, slQueueSlots_.opticalFlowIndex);
            }

            extended.enabledExtensionCount = (uint32_t)extensions.size();
            extended.ppEnabledExtensionNames = extensions.data();
            extended.queueCreateInfoCount = (uint32_t)queues.size();
            extended.pQueueCreateInfos = queues.data();

            if (queueFailure) {
                streamline_.Disable("required SL queue does not fit family limits");
                restoreGameFeatures();
                result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
            } else {
                INFO("SL: device create extended: +%u ext, queues g=%u@%u c=%u@%u ofa=%u@%u",
                    (unsigned)(extensions.size() - pCreateInfo->enabledExtensionCount),
                    slQueueSlots_.graphicsFamily, slQueueSlots_.graphicsIndex,
                    slQueueSlots_.computeFamily, slQueueSlots_.computeIndex,
                    slQueueSlots_.opticalFlowFamily, slQueueSlots_.opticalFlowIndex);
                result = orig(physicalDevice, &extended, pAllocator, pDevice);
                if (result != VK_SUCCESS) {
                    INFO("SL: extended vkCreateDevice failed: %d (added %u ext) - retrying vanilla",
                        (int)result, (unsigned)(extensions.size() - pCreateInfo->enabledExtensionCount));
                    streamline_.Disable("extended vkCreateDevice failed, retrying vanilla");
                    restoreGameFeatures();
                    result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
                }
            }

            // Restore game-owned structs regardless of outcome (idempotent; also covers the
            // extended-success path, which does not go through either retry above).
            restoreGameFeatures();
        } else {
            result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
        vkCreateDeviceHooked(physicalDevice, pCreateInfo, pAllocator, pDevice, result);
        if (result == VK_SUCCESS) {
            streamline_.FlushBootLog();
            if (streamline_.Ready()) {
                streamline_.HandOffDevice(instance_, physicalDevice, *pDevice, slQueueSlots_);
            }
            streamline_.LogFeatureSupport(physicalDevice);
        }
        return result;
    }

    void DisableHooks() override
    {
        if (!CreateInstanceHook_.IsWrapped()) return;

        IMGUI_DEBUG("VulkanBackend::DisableHooks()");

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        CreateInstanceHook_.Unwrap();
        CreateDeviceHook_.Unwrap();
        DestroyDeviceHook_.Unwrap();
        CreatePipelineCacheHook_.Unwrap();
        CreateSwapchainKHRHook_.Unwrap();
        DestroySwapchainKHRHook_.Unwrap();
        QueuePresentKHRHook_.Unwrap();
        if (ngxEvaluateFeatureHook_.IsWrapped()) {
            ngxEvaluateFeatureHook_.Unwrap();
        }
        DetourTransactionCommit();
    }

    void InitializeUI() override
    {
        if (initialized_) return;

        IMGUI_DEBUG("VulkanBackend::InitializeUI()");

        VkDescriptorPoolSize poolSize
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = TextureHardCap,
        };

        VkDescriptorPoolCreateInfo createInfo
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = TextureHardCap,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize
        };
        VK_CHECK(vkCreateDescriptorPool(device_, &createInfo, nullptr, &descriptorPool_));

        VkSamplerCreateInfo samplerInfo
        {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_));

        IMGUI_DEBUG("VK initialization: desc pool %p, sampler %p", descriptorPool_, sampler_);

        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = instance_;
        init_info.PhysicalDevice = physicalDevice_;
        init_info.Device = device_;
        init_info.QueueFamily = queueFamily_;
        init_info.Queue = renderQueue_;
        init_info.PipelineCache = pipelineCache_;
        init_info.DescriptorPool = descriptorPool_;
        init_info.Subpass = 0;
        init_info.MinImageCount = swapchain_.images_.size();
        // ImGui rotates its vertex/index buffers through a ring of ImageCount slots, one per
        // RenderDrawData call, assuming the GPU is at most ImageCount frames behind. Frame
        // generation queues presents deeper than the swapchain image count, so a slot can be
        // rewritten while a command buffer from several frames ago is still reading it -
        // corrupted geometry on screen, and a device loss when the stale index data runs off the
        // end of the vertex buffer. Deepen the ring so FG latency fits inside it; the cost is a
        // few extra small per-frame buffers.
        init_info.ImageCount = swapchain_.images_.size() * 4;
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.Allocator = nullptr;
        init_info.CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS) {
                ERR("[IMGUI] VK operation failed: %d", err);
                TryDebugBreak();
            }
        };
        // init_info.RenderPass = presentRenderPass_;
        init_info.RenderPass = swapchain_.renderPass_;
        ImGui_ImplVulkan_Init(&init_info);
        ImGui_ImplVulkan_CreateFontsTexture();

        initialized_ = true;

        IMGUI_DEBUG("VK POSTINIT - uiFrameworkStarted %d", uiFrameworkStarted_ ? 1 : 0);
        IMGUI_DEBUG("    Instance %p, PhysicalDevice %p, Device %p", instance_, physicalDevice_, device_);
        IMGUI_DEBUG("    QueueFamily %d, RenderQueue %p", queueFamily_, renderQueue_, swapChain_);
        IMGUI_DEBUG("    PipelineCache %p, DescriptorPool %p, Sampler %p", pipelineCache_, descriptorPool_, sampler_);
        IMGUI_DEBUG("VK SWAPCHAIN");
        IMGUI_DEBUG("    SwapChain %p, RenderPass %p, CommandPool %p", swapChain_, swapchain_.renderPass_, swapchain_.commandPool_);
        IMGUI_DEBUG("VK FRAMEBUFFERS");
        for (auto const& image : swapchain_.images_) {
            IMGUI_DEBUG("    Image %p, FB %p, View %p, Fence %p", image.image, image.framebuffer, image.view, image.fence);
        }
    }

    void DestroyUI() override
    {
        if (!initialized_) return;

        IMGUI_DEBUG("VK shutdown");

        // Must run before ImGui_ImplVulkan_Shutdown(), which takes the backend data our pipeline
        // helper needs.
        resetNgxResources();

        ImGui_ImplVulkan_Shutdown();
        drawViewport_ = -1;
        initialized_ = false;
    }

    void NewFrame() override
    {
        // Speculative check to avoid unnecessary locking
        if (!initialized_) return;

        //OPTICK_EVENT(Optick::Category::Rendering);
        std::lock_guard _(globalResourceLock_);

        // Locked check (at this point we're certain noone is manipulating the initialized flag)
        if (!initialized_) return;

        curViewport_ = (curViewport_ + 1) % viewports_.size();
        GImGui->Viewports[0] = &viewports_[curViewport_].Viewport;

        if (requestReloadFonts_) {
            IMGUI_DEBUG("Rebuilding font atlas");
            ImGui_ImplVulkan_DestroyFontsTexture();
            requestReloadFonts_ = false;
        }

        streamline_.FlushBootLog();
        tryInstallNgxEvaluateFeatureHook();
        ImGui_ImplVulkan_NewFrame();
        //IMGUI_FRAME_DEBUG("VK: NewFrame");
    }

    void FinishFrame() override
    {
        // Speculative check to avoid unnecessary locking
        if (!initialized_) return;

        //OPTICK_EVENT(Optick::Category::Rendering);
        std::lock_guard _(globalResourceLock_);

        // Locked check (at this point we're certain noone is manipulating the initialized flag)
        if (!initialized_) return;

        auto& vp = viewports_[curViewport_].Viewport;
        auto& drawLists = viewports_[curViewport_].ClonedDrawLists;

        for (auto list : drawLists) {
            list->~ImDrawList();
            IM_FREE(list);
        }

        drawLists.clear();

        for (int i = 0; i < vp.DrawDataP.CmdLists.Size; i++) {
            auto list = vp.DrawDataP.CmdLists[i];
            auto drawList = list->CloneOutput();
            vp.DrawDataP.CmdLists[i] = drawList;
            drawLists.push_back(drawList);
        }

        drawViewport_ = curViewport_;
        menuVisible_ = (vp.DrawDataP.CmdLists.Size > 0);
        //IMGUI_FRAME_DEBUG("VK: FinishFrame");
    }

    void ClearFrame() override
    {
        if (!initialized_) return;

        drawViewport_ = -1;
    }

    std::optional<TextureLoadResult> RegisterTexture(TextureDescriptor* descriptor) override
    {
        if (!initialized_) return {};

        if (descriptor->Vulkan.Views.size() != 1 || !descriptor->Vulkan.Views[0]->View) {
            ERR("VK texture has no render view?");
            return {};
        }

        auto view = descriptor->Vulkan.Views[0]->View;
        if (!view) return {};

        if (textures_ > TextureSoftCap) {
            if (!textureLimitWarningShown_) {
                ERR("UI texture limit reached. Newly loaded textures may not load or render correctly");
                textureLimitWarningShown_ = true;
            }
            return {};
        }

        textures_++;
        return TextureLoadResult{ 
            TextureOpaqueHandle(view),
            descriptor->Vulkan.ImageData.Width, 
            descriptor->Vulkan.ImageData.Height
        };
    }

    void UnregisterTexture(TextureOpaqueHandle opaqueHandle) override
    {
        if (!initialized_) return;

        auto imageView = static_cast<VkImageView>(opaqueHandle);
        auto desc = textureDescriptors_.get_or_default(imageView, 0);
        if (desc) {
            ImGui_ImplVulkan_RemoveTexture(desc);
            textureDescriptors_.remove(imageView);
        }

        textures_--;
    }

    std::optional<ImTextureID> BindTexture(TextureOpaqueHandle opaqueHandle) override
    {
        auto imageView = static_cast<VkImageView>(opaqueHandle);
        auto desc = textureDescriptors_.get_or_default(imageView, 0);
        if (!desc) {
            desc = ImGui_ImplVulkan_AddTexture(sampler_, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (desc) {
                textureDescriptors_.set(imageView, desc);
            }
        }
        
        return (ImTextureID)desc;
    }

    bool IsInitialized() override
    {
        return initialized_;
    }

    void ReloadFonts() override
    {
        requestReloadFonts_ = true;
    }

    glm::ivec2 GetViewportSize() override
    {
        return glm::ivec2(swapchain_.width_, swapchain_.height_);
    }

private:
    struct SwapchainImageInfo
    {
        VkImage image{ VK_NULL_HANDLE };
        VkFramebuffer framebuffer{ VK_NULL_HANDLE };
        VkImageView view{ VK_NULL_HANDLE };
        VkFence fence{ VK_NULL_HANDLE };
        VkSemaphore uiDoneSemaphore{ VK_NULL_HANDLE };
        VkCommandBuffer commandBuffer{ VK_NULL_HANDLE };
    };

    struct SwapchainInfo
    {
        VkRenderPass renderPass_{ VK_NULL_HANDLE };
        VkCommandPool commandPool_{ VK_NULL_HANDLE };
        Array<SwapchainImageInfo> images_;
        uint32_t width_{ 0 };
        uint32_t height_{ 0 };
    };

    struct ViewportInfo
    {
        ImGuiViewportP Viewport;
        Array<ImDrawList*> ClonedDrawLists;
    };

    void vkCreateInstanceHooked(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance,
        VkResult result)
    {
        IMGUI_DEBUG("vkCreateInstance -> %p, %d", *pInstance, result);
        if (pCreateInfo->pApplicationInfo != nullptr && pCreateInfo->pApplicationInfo->pApplicationName != nullptr) {
            IMGUI_DEBUG("App=%s", pCreateInfo->pApplicationInfo->pApplicationName);
        }

        for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; i++) {
            IMGUI_DEBUG("Layer=%s", pCreateInfo->ppEnabledLayerNames[i]);
        }

        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            IMGUI_DEBUG("Extension=%s", pCreateInfo->ppEnabledExtensionNames[i]);
        }

        if (result != VK_SUCCESS) return;

        instance_ = *pInstance;

        if (!CreateDeviceHook_.IsWrapped()) {
            IMGUI_DEBUG("Hooking CreateDevice()");

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            auto createDevice = vkGetInstanceProcAddr(instance_, "vkCreateDevice");
            auto destroyDevice = vkGetInstanceProcAddr(instance_, "vkDestroyDevice");
            CreateDeviceHook_.Wrap(ResolveFunctionTrampoline(createDevice));
            DestroyDeviceHook_.Wrap(ResolveFunctionTrampoline(destroyDevice));
            DetourTransactionCommit();
        }
    }

    void vkCreateDeviceHooked(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice,
        VkResult result)
    {
        IMGUI_DEBUG("vkCreateDevice flags=%x -> %p, %d", pCreateInfo->flags, *pDevice, result);

        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
            auto const& queue = pCreateInfo->pQueueCreateInfos[i];
            IMGUI_DEBUG("Queue=%x, %d, %d", queue.flags, queue.queueFamilyIndex, queue.queueCount);
        }

        for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; i++) {
            IMGUI_DEBUG("Layer=%s", pCreateInfo->ppEnabledLayerNames[i]);
        }

        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            IMGUI_DEBUG("Extension=%s", pCreateInfo->ppEnabledExtensionNames[i]);
        }

        if (result != VK_SUCCESS) return;

        std::lock_guard _(globalResourceLock_);
        physicalDevice_ = physicalDevice;
        device_ = *pDevice;
        
        uint32_t numFamilies;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &numFamilies, nullptr);
        Array<VkQueueFamilyProperties> families;
        families.resize(numFamilies);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &numFamilies, families.data());

        uint32_t queueFamily{ 0 };
        VkQueue queue{ nullptr };
        for (auto const& family : families) {
            if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                vkGetDeviceQueue(device_, queueFamily, 0, &queue);
                queueFamily_ = queueFamily;
                renderQueue_ = queue;
                break;
            }

            queueFamily++;
        }

        IMGUI_DEBUG("Detected graphics queue family %d, %p", queueFamily, queue);

        PFN_vkCreatePipelineCache createPipelineCache = reinterpret_cast<PFN_vkCreatePipelineCache>(
            vkGetDeviceProcAddr(*pDevice, "vkCreatePipelineCache"));
        PFN_vkCreateSwapchainKHR gameCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            vkGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR"));
        PFN_vkDestroySwapchainKHR destroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            vkGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR"));
        PFN_vkQueuePresentKHR gameQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
            vkGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR"));

        PFN_vkQueuePresentKHR nextPresent = dlssgPresentFunction_ ? dlssgPresentFunction_ : gameQueuePresentKHR;
        PFN_vkCreateSwapchainKHR nextCreateSwapchain = dlssgCreateSwapchainKHR_ ? dlssgCreateSwapchainKHR_ : gameCreateSwapchainKHR;

        if (!CreatePipelineCacheHook_.IsWrapped()) {
            IMGUI_DEBUG("Hooking CreatePipelineCache()");

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            CreatePipelineCacheHook_.Wrap(ResolveFunctionTrampoline(createPipelineCache));
            CreateSwapchainKHRHook_.Wrap(ResolveFunctionTrampoline(nextCreateSwapchain));
            DestroySwapchainKHRHook_.Wrap(ResolveFunctionTrampoline(destroySwapchainKHR));
            QueuePresentKHRHook_.Wrap(ResolveFunctionTrampoline(nextPresent));
            DetourTransactionCommit();
        }
    }

    void vkDestroyDeviceHooked(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (device != device_) return;

        std::lock_guard _(globalResourceLock_);
        IMGUI_DEBUG("VK device destroyed");

        DestroyUI();
        releaseSwapChain(swapchain_);

        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        queueFamily_ = 0;
        renderQueue_ = VK_NULL_HANDLE;

        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
            textureDescriptors_.clear();
        }

        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
    }

    void vkCreatePipelineCacheHooked(
        VkDevice device,
        const VkPipelineCacheCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkPipelineCache* pPipelineCache,
        VkResult result)
    {
        if (result != VK_SUCCESS) return;

        IMGUI_DEBUG("VK pipeline cache created: %p", *pPipelineCache);
        pipelineCache_ = *pPipelineCache;
    }

    void vkCreateSwapchainKHRHooked(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain,
        VkResult result)
    {
        if (result != VK_SUCCESS) return;

        std::lock_guard _(globalResourceLock_);
        IMGUI_DEBUG("VK swap chain created: %p", *pSwapchain);
        swapChain_ = *pSwapchain;
        collectSwapChainInfo(pCreateInfo);
    }

    void vkDestroySwapchainKHRHooked(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* pAllocator)
    {
        if (device != device_ || swapchain != swapChain_) return;

        std::lock_guard _(globalResourceLock_);
        IMGUI_DEBUG("VK swap chain destroyed");
        DestroyUI();
        releaseSwapChain(swapchain_);

        swapChain_ = VK_NULL_HANDLE;
        pipelineCache_ = VK_NULL_HANDLE;
    }

    void releaseSwapChain(SwapchainInfo& swapchain)
    {
        for (auto& image : swapchain.images_) {
            vkFreeCommandBuffers(device_, swapchain.commandPool_, 1, &image.commandBuffer);
            vkDestroySemaphore(device_, image.uiDoneSemaphore, nullptr);
            vkDestroyFence(device_, image.fence, nullptr);
            vkDestroyFramebuffer(device_, image.framebuffer, nullptr);
            vkDestroyImageView(device_, image.view, nullptr);
        }

        swapchain.images_.clear();

        if (swapchain.renderPass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, swapchain.renderPass_, nullptr);
            swapchain.renderPass_ = VK_NULL_HANDLE;
        }

        if (swapchain.commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, swapchain.commandPool_, nullptr);
            swapchain.commandPool_ = VK_NULL_HANDLE;
        }
    }

    void collectSwapChainInfo(const VkSwapchainCreateInfoKHR* pCreateInfo)
    {
        SwapchainInfo& swapInfo = swapchain_;

        swapInfo.width_ = pCreateInfo->imageExtent.width;
        swapInfo.height_ = pCreateInfo->imageExtent.height;
        IMGUI_DEBUG("Swap chain size: %d x %d", swapInfo.width_, swapInfo.height_);

        {
            VkAttachmentDescription attDesc = {
                0,
                pCreateInfo->imageFormat,
                VK_SAMPLE_COUNT_1_BIT,
                VK_ATTACHMENT_LOAD_OP_LOAD,
                VK_ATTACHMENT_STORE_OP_STORE,
                VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                VK_ATTACHMENT_STORE_OP_DONT_CARE,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            };

            VkAttachmentReference attRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

            VkSubpassDescription sub = {
                0,    VK_PIPELINE_BIND_POINT_GRAPHICS,
                0,    NULL,       // inputs
                1,    &attRef,    // color
                NULL,             // resolve
                NULL,             // depth-stencil
                0,    NULL,       // preserve
            };

            VkRenderPassCreateInfo rpinfo = {
                VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                NULL,
                0,
                1,
                &attDesc,
                1,
                &sub,
                0,
                NULL,    // dependencies
            };

            VK_CHECK(vkCreateRenderPass(device_, &rpinfo, NULL, &swapInfo.renderPass_));
        }

        {
            VkCommandPoolCreateInfo createInfo = {
                VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                NULL,
                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                queueFamily_
            };
            VK_CHECK(vkCreateCommandPool(device_, &createInfo, nullptr, &swapchain_.commandPool_));
        }

        // serialise out the swap chain images
        {
            uint32_t numSwapImages{ 0 };
            VK_CHECK(vkGetSwapchainImagesKHR(device_, swapChain_, &numSwapImages, NULL));

            swapInfo.images_.resize(numSwapImages);

            Array<VkImage> images;
            images.resize(numSwapImages);

            // go through our own function so we assign these images IDs
            VK_CHECK(vkGetSwapchainImagesKHR(device_, swapChain_, &numSwapImages, images.data()));

            for (uint32_t i = 0; i < numSwapImages; i++)
            {
                auto& imInfo = swapInfo.images_[i];

                // memory doesn't exist for genuine WSI created images
                imInfo.image = images[i];

                VkImageSubresourceRange range;
                range.baseMipLevel = range.baseArrayLayer = 0;
                range.levelCount = 1;
                range.layerCount = pCreateInfo->imageArrayLayers;
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

                {
                    VkCommandBufferAllocateInfo allocInfo = {
                        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                        NULL,
                        swapchain_.commandPool_,
                        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                        1
                    };
                    VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, &imInfo.commandBuffer));
                }

                {
                    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL,
                                                    VK_FENCE_CREATE_SIGNALED_BIT };

                    VK_CHECK(vkCreateFence(device_, &fenceInfo, NULL, &imInfo.fence));
                }

                {
                    VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

                    VK_CHECK(vkCreateSemaphore(device_, &semInfo, NULL, &imInfo.uiDoneSemaphore));
                }

                {
                    VkImageViewCreateInfo info = {
                        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                        NULL,
                        0,
                        images[i],
                        VK_IMAGE_VIEW_TYPE_2D,
                        pCreateInfo->imageFormat,
                        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                    };

                    VK_CHECK(vkCreateImageView(device_, &info, NULL, &imInfo.view));

                    VkFramebufferCreateInfo fbinfo = {
                        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                        NULL,
                        0,
                        swapchain_.renderPass_,
                        1,
                        &imInfo.view,
                        (uint32_t)pCreateInfo->imageExtent.width,
                        (uint32_t)pCreateInfo->imageExtent.height,
                        1,
                    };

                    VK_CHECK(vkCreateFramebuffer(device_, &fbinfo, NULL, &imInfo.framebuffer));
                }
            }
        }
    }

    void presentPreHook(VkPresentInfoKHR* pPresentInfo)
    {
        auto& vp = viewports_[drawViewport_].Viewport;
        if (!vp.DrawDataP.Valid) return;

        auto& image = swapchain_.images_[pPresentInfo->pImageIndices[0]];
        //IMGUI_FRAME_DEBUG("vkQueuePresentKHR: Swap chain image #%d (%p)", pPresentInfo->pImageIndices[0], image.image);

        // wait for this command buffer to be free
        // If this ring has never been used the fence is signalled on creation.
        // this should generally be a no-op because we only get here when we've acquired the image
        VK_CHECK(vkWaitForFences(device_, 1, &image.fence, VK_TRUE, 50000000));

        VK_CHECK(vkResetFences(device_, 1, &image.fence));

        VK_CHECK(vkResetCommandBuffer(image.commandBuffer, 0));

        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                              VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

        VK_CHECK(vkBeginCommandBuffer(image.commandBuffer, &beginInfo));

        VkImageMemoryBarrier bbBarrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            NULL,
            0,
            0,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            queueFamily_,
            queueFamily_,
            image.image,
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };

        bbBarrier.srcAccessMask = VK_ACCESS_ALL_READ_BITS;
        bbBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(image.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
            NULL,
            0, NULL,
            1, &bbBarrier);

        uint32_t ringIdx = 0;

        VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

        VkPipelineStageFlags waitStages[3] = { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };

        // wait on the present's semaphores
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;

        // and signal overlaydone
        submitInfo.pSignalSemaphores = &image.uiDoneSemaphore;
        submitInfo.signalSemaphoreCount = 1;

        {
            VkClearValue clearval = {};
            VkRenderPassBeginInfo rpbegin = {
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                NULL,
                swapchain_.renderPass_,
                image.framebuffer,
                {{
                     0,
                     0,
                 },
                 {swapchain_.width_, swapchain_.height_}},
                1,
                &clearval,
            };
            vkCmdBeginRenderPass(image.commandBuffer, &rpbegin, VK_SUBPASS_CONTENTS_INLINE);
        }

        ImGui_ImplVulkan_RenderDrawData(&vp.DrawDataP, image.commandBuffer);

        vkCmdEndRenderPass(image.commandBuffer);

        std::swap(bbBarrier.srcQueueFamilyIndex, bbBarrier.dstQueueFamilyIndex);
        std::swap(bbBarrier.oldLayout, bbBarrier.newLayout);
        bbBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bbBarrier.dstAccessMask = VK_ACCESS_ALL_READ_BITS;

        vkCmdPipelineBarrier(image.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
            NULL,
            0, NULL,
            1, &bbBarrier);

        VK_CHECK(vkEndCommandBuffer(image.commandBuffer));

        {
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &image.commandBuffer;

            VK_CHECK(vkQueueSubmit(renderQueue_, 1, &submitInfo, image.fence));
        }

        // the next thing waits on our new semaphore - whether a subsequent overlay render or the
        // present
        const_cast<VkSemaphore*>(pPresentInfo->pWaitSemaphores)[0] = submitInfo.pSignalSemaphores[0];
        pPresentInfo->waitSemaphoreCount = 1;
    }

    void vkQueuePresentKHRHooked(
        VkQueue queue,
        const VkPresentInfoKHR* pPresentInfo)
    {
        if (pPresentInfo->swapchainCount != 1
            || pPresentInfo->pSwapchains[0] != swapChain_) {
            //IMGUI_FRAME_DEBUG("vkQueuePresentKHR: Bad swapchain (%d: %p vs. %p)",
                //pPresentInfo->swapchainCount, 
                //pPresentInfo->swapchainCount ? pPresentInfo->pSwapchains[0] : nullptr,
                //swapChain_);
            frameNo_++;
            return;
        }

        std::lock_guard _(globalResourceLock_);
        if (!initialized_) {
            IMGUI_DEBUG("vkQueuePresentKHR: Render backend initialized yet");
            frameNo_ = 0;

            if (!uiFrameworkStarted_) {
                ui_.OnRenderBackendInitialized();
                uiFrameworkStarted_ = true;
                IMGUI_DEBUG("vkQueuePresentKHR: uiFrameworkStarted_ = true");

                for (auto i = 0; i < viewports_.size(); i++) {
                    viewports_[i].Viewport = *GImGui->Viewports[0];
                }
            
                GImGui->Viewports[0] = &viewports_[0].Viewport;
            } else {
                ui_.OnRenderBackendInitialized();
                InitializeUI();
            }
        }

        if (ngxCompositedThisFrame_) {
            // Already drawn into the NGX output this frame - drawing again here would double the
            // overlay and lap ImGui's buffer ring.
            ngxCompositedThisFrame_ = false;
        } else if (initialized_ && drawViewport_ != -1) {
            presentPreHook(const_cast<VkPresentInfoKHR*>(pPresentInfo));
        } else {
            //IMGUI_FRAME_DEBUG("vkQueuePresentKHR: Cannot append command buffer - initialized %d, drawViewport %d",
                //initialized_ ? 1 : 0, drawViewport_);
        }

        frameNo_++;
    }
    NVSDK_NGX_Result ngxEvaluateFeatureCHook(
        NgxEvaluateFeatureCHookType::BaseFuncType* orig,
        VkCommandBuffer InCmdList,
        const NVSDK_NGX_Handle* InFeatureHandle,
        const NVSDK_NGX_Parameter* InParameters,
        PFN_NVSDK_NGX_ProgressCallback_C InCallback)
    {
        // Call original first so NGX completes its work and final image state
        NVSDK_NGX_Result evalRes = orig(InCmdList, InFeatureHandle, InParameters, InCallback);

        if (!ngxHookEnteredLogged_) {
            ngxHookEnteredLogged_ = true;
            INFO("IMGUI: NGX EvaluateFeature hook is live");
        }

        if (!initialized_ || !menuVisible_ || evalRes != NVSDK_NGX_Result_Success || !InCmdList || !InParameters)
            return evalRes;

        // Everything below touches state shared with NewFrame()/FinishFrame()/the present hook -
        // the render pass, framebuffer and pipeline caches, and viewports_[drawViewport_]. This
        // callback runs on the game's render thread with no synchronisation of its own, so take
        // the same lock those paths use. Without it, concurrent inserts corrupt the caches; the
        // observable symptom was the overlay pipeline being built twice for one format, followed
        // by crashes and hangs whose timing wandered from frame to frame.
        //
        // The lock is taken after orig() so NGX's own work is never serialised against us.
        std::lock_guard _(globalResourceLock_);

        // Re-check under the lock, since the backend may have been torn down while we waited.
        if (!initialized_ || drawViewport_ < 0)
            return evalRes;

        // DIAGNOSTIC (temporary): NgxOverlayStage bisects the composite without a rebuild.
        //   0 = do nothing              (control - is the composite the cause at all?)
        //   1 = layout barriers only    (tests the VK_IMAGE_LAYOUT_GENERAL assumption)
        //   2 = + render pass, no draw  (tests the framebuffer / render pass)
        //   3 = + ImGui draw            (full, default)
        // Set via NgxOverlayStage in ScriptExtenderSettings.json.
        if (ngxStage_ < 0) {
            ngxStage_ = (int)gExtender->GetConfig().NgxOverlayStage;
            if (ngxStage_ < 0 || ngxStage_ > 3) ngxStage_ = 3;
            INFO("IMGUI: NGX composite stage %d "
                "(0=off 1=barriers 2=+renderpass 3=full) - set NgxOverlayStage in "
                "ScriptExtenderSettings.json to change",
                ngxStage_);
        }

        if (ngxStage_ == 0)
            return evalRes;

        // Stay clear of swapchain transitions. Around an alt-tab the upscaler tears down and
        // recreates its NGX resources; a composite recorded near that window can reference an
        // output view whose image is destroyed before the command buffer completes. A barrier
        // survives that (it only records a dependency); a LOAD_OP_LOAD render pass reads memory
        // through the view and faults. frameNo_ restarts at 0 on every backend (re)init, so
        // holding off for a second of frames keeps the composite out of the transition window.
        // The present-path overlay covers the menu during warmup.
        if (frameNo_ < NgxCompositeWarmupFrames)
            return evalRes;

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

        if (ngxGetVoidPointer_ == nullptr) {
            if (!ngxOutputUnavailableLogged_) {
                ngxOutputUnavailableLogged_ = true;
                ERR("IMGUI: NVSDK_NGX_Parameter_GetVoidPointer not exported by any loaded module; "
                    "cannot read the NGX output image");
            }
            return evalRes;
        }

        // Extract NGX output resource as a Vulkan image view
        void* outPtr = nullptr;
        if (ngxGetVoidPointer_(const_cast<NVSDK_NGX_Parameter*>(InParameters), NVSDK_NGX_Parameter_Output, &outPtr) != NVSDK_NGX_Result_Success || !outPtr) {
            if (!ngxNoOutputResourceLogged_) {
                ngxNoOutputResourceLogged_ = true;
                ERR("IMGUI: NGX parameter block has no '%s' output resource", NVSDK_NGX_Parameter_Output);
            }
            return evalRes;
        }

        auto* outResVK = reinterpret_cast<NVSDK_NGX_Resource_VK*>(outPtr);
        const NVSDK_NGX_ImageViewInfo_VK& iv = outResVK->Resource.ImageViewInfo;
        VkImageView  targetView   = iv.ImageView;
        VkImage      targetImage  = iv.Image;
        VkFormat     targetFormat = iv.Format;
        uint32_t     targetW      = iv.Width;
        uint32_t     targetH      = iv.Height;
        VkImageSubresourceRange range = iv.SubresourceRange;
        if (range.aspectMask == 0)  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        if (range.levelCount == 0)  range.levelCount = 1;
        if (range.layerCount == 0)  range.layerCount = 1;

        if (targetView == VK_NULL_HANDLE || targetImage == VK_NULL_HANDLE || targetFormat == VK_FORMAT_UNDEFINED)
            return evalRes;

        // Get/create a render pass for this format
        auto getOrCreateRenderPass = [&](VkFormat fmt) -> VkRenderPass {
            auto it = formatToRenderPass_.find(fmt);
            if (it != formatToRenderPass_.end()) return it->second;

            VkAttachmentDescription attDesc{};
            attDesc.format         = fmt;
            attDesc.samples        = VK_SAMPLE_COUNT_1_BIT;
            attDesc.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve NGX output
            attDesc.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            attDesc.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attDesc.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attDesc.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorRef{};
            colorRef.attachment = 0;
            colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkSubpassDescription sub{};
            sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments    = &colorRef;

            VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            rpInfo.attachmentCount = 1;
            rpInfo.pAttachments    = &attDesc;
            rpInfo.subpassCount    = 1;
            rpInfo.pSubpasses      = &sub;

            VkRenderPass rp = VK_NULL_HANDLE;
            VK_CHECK(vkCreateRenderPass(device_, &rpInfo, nullptr, &rp));
            formatToRenderPass_[fmt] = rp;
            return rp;
        };

        VkRenderPass rp = VK_NULL_HANDLE;
        VkPipeline overlayPipeline = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;

        if (ngxStage_ >= 2) {
            rp = getOrCreateRenderPass(targetFormat);
            if (rp == VK_NULL_HANDLE)
                return evalRes;
        }

        // Bail before touching the command buffer if we have no compatible pipeline - drawing
        // with an incompatible one is a device-lost, not a missing overlay.
        if (ngxStage_ >= 3) {
            overlayPipeline = getOrCreateNgxPipeline(targetFormat, rp);
            if (overlayPipeline == VK_NULL_HANDLE)
                return evalRes;
        }

        if (ngxStage_ >= 2) {
            // NGX rotates through several output image views, and Vulkan is free to reuse a
            // handle value once the view behind it is destroyed. Caching a framebuffer against
            // a VkImageView therefore hands us, sooner or later, a framebuffer built from a view
            // that no longer exists - which faults the GPU. Build one per composite and retire it
            // a few frames later, once the GPU is certainly past it.
            retireStaleNgxFramebuffers();
            purgeNgxGraveyard();

            VkImageView attachments[1] = { targetView };
            VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fbInfo.renderPass = rp;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = attachments;
            fbInfo.width  = targetW;
            fbInfo.height = targetH;
            fbInfo.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device_, &fbInfo, nullptr, &fb));

            if (fb != VK_NULL_HANDLE) {
                ngxFramebuffers_.push_back({ fb, frameNo_ });
            }
        }

        if (ngxStage_ >= 2 && fb == VK_NULL_HANDLE)
            return evalRes;

        // Transition NGX output to COLOR_ATTACHMENT for overlay
        VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toColor.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        toColor.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image         = targetImage;
        toColor.subresourceRange = range;

        vkCmdPipelineBarrier(
            InCmdList,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &toColor);

        // Begin render pass and draw ImGui
        VkRenderPassBeginInfo rpBegin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpBegin.renderPass  = rp;
        rpBegin.framebuffer = fb;
        rpBegin.renderArea.offset = { 0, 0 };
        rpBegin.renderArea.extent = { targetW, targetH };

        bool drawn = false;
        if (ngxStage_ >= 2) {
            vkCmdBeginRenderPass(InCmdList, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            if (ngxStage_ >= 3) {
                drawn = injectImGuiIntoCommandBuffer(InCmdList, overlayPipeline);
            }
            vkCmdEndRenderPass(InCmdList);
        }

        if (drawn) {
            // Claim this frame so presentPreHook() does not render the same draw data a second
            // time. ImGui's Vulkan backend cycles its vertex/index buffers once per
            // RenderDrawData call over a ring sized to the swapchain image count, so drawing
            // twice per frame laps that ring and overwrites buffers still in flight.
            ngxCompositedThisFrame_ = true;

            if (!ngxOverlayDrawnLogged_) {
                ngxOverlayDrawnLogged_ = true;
                INFO("IMGUI: drawing overlay into NGX output (%dx%d, format %d); "
                    "present-time overlay disabled for composited frames",
                    (int)targetW, (int)targetH, (int)targetFormat);
            }
        }

        // Transition back to GENERAL so downstream consumers can read
        VkImageMemoryBarrier toGeneral = toColor;
        toGeneral.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        toGeneral.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toGeneral.newLayout     = VK_IMAGE_LAYOUT_GENERAL;

        vkCmdPipelineBarrier(
            InCmdList,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &toGeneral);

        return evalRes;
    }

    bool injectImGuiIntoCommandBuffer(VkCommandBuffer cmd, VkPipeline pipeline)
    {
        if (!initialized_ || drawViewport_ < 0 || !cmd)
            return false;
        auto& vp = viewports_[drawViewport_].Viewport;
        if (!vp.DrawDataP.Valid || vp.DrawDataP.CmdListsCount == 0)
            return false;
        ImGui_ImplVulkan_RenderDrawData(&vp.DrawDataP, cmd, pipeline);
        return true;
    }

    // Drop everything the composite cached. The ImGui backend is torn down and rebuilt whenever
    // the swapchain is recreated - alt-tabbing does it - and NGX recreates its own resources at
    // the same time, so nothing built against the old ones stays valid.
    //
    // Nothing is destroyed here: this runs inside the swapchain-destroy hook, where the game's
    // render threads may still be submitting, so neither vkDeviceWaitIdle nor destroying
    // possibly-in-flight objects is safe. Everything moves to a graveyard instead, purged from
    // the composite path once the rebuilt backend has been running long enough that the old
    // work is certainly complete.
    void resetNgxResources()
    {
        for (auto const& fb : ngxFramebuffers_) {
            ngxGraveyardFramebuffers_.push_back(fb.Framebuffer);
        }
        for (auto const& pipeline : formatToPipeline_) {
            if (pipeline.second != VK_NULL_HANDLE) ngxGraveyardPipelines_.push_back(pipeline.second);
        }
        for (auto const& renderPass : formatToRenderPass_) {
            if (renderPass.second != VK_NULL_HANDLE) ngxGraveyardRenderPasses_.push_back(renderPass.second);
        }

        ngxFramebuffers_.clear();
        formatToPipeline_.clear();
        formatToRenderPass_.clear();
    }

    // Destroy graveyard objects once the rebuilt backend has run for a while. frameNo_ restarts
    // at 0 on re-init, so a simple threshold works. Called under the backend lock with the
    // device alive. If the device is destroyed before we get here, the entries are abandoned -
    // device teardown reclaims them.
    void purgeNgxGraveyard()
    {
        if (frameNo_ < NgxGraveyardPurgeFrame) return;
        if (ngxGraveyardFramebuffers_.empty() && ngxGraveyardPipelines_.empty()
            && ngxGraveyardRenderPasses_.empty()) return;

        for (auto fb : ngxGraveyardFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
        for (auto pipeline : ngxGraveyardPipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
        for (auto renderPass : ngxGraveyardRenderPasses_) vkDestroyRenderPass(device_, renderPass, nullptr);

        ngxGraveyardFramebuffers_.clear();
        ngxGraveyardPipelines_.clear();
        ngxGraveyardRenderPasses_.clear();
    }

    // Destroy NGX framebuffers the GPU has certainly finished with. Called under the backend
    // lock from the composite path.
    void retireStaleNgxFramebuffers()
    {
        if (device_ == VK_NULL_HANDLE) {
            ngxFramebuffers_.clear();
            return;
        }

        auto it = ngxFramebuffers_.begin();
        while (it != ngxFramebuffers_.end()) {
            auto age = frameNo_ - it->FrameNo;
            if (age > NgxFramebufferLifetime || age < 0) {
                vkDestroyFramebuffer(device_, it->Framebuffer, nullptr);
                it = ngxFramebuffers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ImGui builds its pipeline against the swapchain render pass, but the NGX output is a
    // different attachment format (B10G11R11_UFLOAT vs the swapchain's B8G8R8A8_UNORM), and a
    // pipeline may only be used inside a render pass compatible with the one it was built for.
    // Using the swapchain pipeline here faults the GPU and loses the device, so build one per
    // output format.
    VkPipeline getOrCreateNgxPipeline(VkFormat fmt, VkRenderPass renderPass)
    {
        auto it = formatToPipeline_.find(fmt);
        if (it != formatToPipeline_.end()) return it->second;

        auto pipeline = ImGui_ImplVulkan_CreatePipelineForRenderPass(renderPass, VK_SAMPLE_COUNT_1_BIT, 0);
        formatToPipeline_[fmt] = pipeline;

        if (pipeline == VK_NULL_HANDLE) {
            ERR("IMGUI: could not build an overlay pipeline for NGX output format %d; "
                "skipping the overlay rather than drawing with an incompatible pipeline", (int)fmt);
        } else {
            INFO("IMGUI: built overlay pipeline for NGX output format %d", (int)fmt);
        }

        return pipeline;
    }

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

    IMGUIManager& ui_;
    VkInstance instance_{ VK_NULL_HANDLE };
    VkPhysicalDevice physicalDevice_{ VK_NULL_HANDLE };
    VkDevice device_{ VK_NULL_HANDLE };
    uint32_t queueFamily_{ 0 };
    VkQueue renderQueue_{ VK_NULL_HANDLE };
    VkSwapchainKHR swapChain_{ VK_NULL_HANDLE };
    VkPipelineCache pipelineCache_{ VK_NULL_HANDLE };
    VkDescriptorPool descriptorPool_{ VK_NULL_HANDLE };
    VkSampler sampler_{ VK_NULL_HANDLE };
    std::array<ViewportInfo, 3> viewports_;
    int32_t drawViewport_{ -1 };
    int32_t curViewport_{ 0 };
    int32_t frameNo_{ 0 };
    bool textureLimitWarningShown_{ false };

    bool initialized_{ false };
    bool uiFrameworkStarted_{ false };
    bool menuVisible_{ false };
    bool requestReloadFonts_{ false };
    std::mutex globalResourceLock_;

    HashMap<VkImageView, VkDescriptorSet> textureDescriptors_;
    std::unordered_map<VkFormat, VkRenderPass> formatToRenderPass_;
    std::unordered_map<VkFormat, VkPipeline> formatToPipeline_;
    // Framebuffers built for NGX output views, retired once the GPU is well past them. Keyed by
    // creation frame rather than by image view, which is not a stable identity.
    struct NgxFramebuffer
    {
        VkFramebuffer Framebuffer;
        int32_t FrameNo;
    };

    static constexpr int32_t NgxFramebufferLifetime{ 8 };
    static constexpr int32_t NgxGraveyardPurgeFrame{ 30 };
    static constexpr int32_t NgxCompositeWarmupFrames{ 60 };
    std::vector<NgxFramebuffer> ngxFramebuffers_;
    std::vector<VkFramebuffer> ngxGraveyardFramebuffers_;
    std::vector<VkPipeline> ngxGraveyardPipelines_;
    std::vector<VkRenderPass> ngxGraveyardRenderPasses_;

    VkCreateInstanceHookType CreateInstanceHook_;
    VkCreateDeviceHookType CreateDeviceHook_;
    VkDestroyDeviceHookType DestroyDeviceHook_;
    VkCreatePipelineCacheHookType CreatePipelineCacheHook_;
    VkCreateSwapchainKHRHookType CreateSwapchainKHRHook_;
    VkDestroySwapchainKHRHookType DestroySwapchainKHRHook_;
    VkQueuePresentKHRHookType QueuePresentKHRHook_;
    NgxEvaluateFeatureCHookType ngxEvaluateFeatureHook_;

    StreamlineManager streamline_;
    bool slInitAttempted_{ false };
    SLQueueSlots slQueueSlots_;
    HMODULE sl_{ nullptr };
    PFN_vkQueuePresentKHR dlssgPresentFunction_{ nullptr };
    PFN_vkCreateSwapchainKHR dlssgCreateSwapchainKHR_{ nullptr };

    // Frames between attempts to locate the NGX EvaluateFeature entry point.
    static constexpr unsigned NgxProbeInterval{ 120 };
    unsigned ngxProbeDelay_{ 0 };
    PFN_NVSDK_NGX_Parameter_GetVoidPointer ngxGetVoidPointer_{ nullptr };
    // One-shot latches so the per-frame paths below report once instead of every frame.
    bool ngxProbeFailureLogged_{ false };
    bool ngxHookEnteredLogged_{ false };
    bool ngxOutputUnavailableLogged_{ false };
    bool ngxNoOutputResourceLogged_{ false };
    bool ngxOverlayDrawnLogged_{ false };
    // Set when the NGX hook composites the overlay; consumed by the present hook so a frame is
    // never rendered twice.
    bool ngxCompositedThisFrame_{ false };
    // BG3SE_NGX_STAGE override; -1 until resolved from the environment.
    int ngxStage_{ -1 };

    SwapchainInfo swapchain_;
    uint32_t textures_{ 0 };
};

END_NS()