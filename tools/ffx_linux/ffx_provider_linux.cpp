// The provider registry of AMD's ffx-api (ffx-api/src/ffx_provider.cpp) for Linux: the FSR 3.1 upscaler, frame
// generation and its Vulkan swapchain. AMD's own registry also looks for driver providers through amdxc64.dll (D3D12)
// and lists FSR 2, which this build does not have.
#include "ffx_provider.h"
#include "ffx_provider_fsr3upscale.h"
#include "ffx_provider_framegeneration.h"
#include "vk/ffx_provider_framegenerationswapchain_vk.h"

static const ffxProvider* const providers[] = {
    &ffxProvider_FSR3Upscale::Instance,
    &ffxProvider_FrameGeneration::Instance,
    &ffxProvider_FrameGenerationSwapChain_VK::Instance,
};
static constexpr size_t providerCount = sizeof providers / sizeof providers[0];

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void*)
{
    for (size_t i = 0; i < providerCount; ++i)
        if ((!overrideId || providers[i]->GetId() == overrideId) && providers[i]->CanProvide(descType))
            return providers[i];
    return nullptr;
}

const ffxProvider* GetAssociatedProvider(ffxContext* context)
{
    return ((const InternalContextHeader*)(*context))->provider;
}

uint64_t GetProviderVersions(ffxStructType_t descType, void*, uint64_t capacity, uint64_t* versionIds, const char** versionNames)
{
    uint64_t count = 0;
    for (size_t i = 0; i < providerCount && count < capacity; ++i)
        if (providers[i]->CanProvide(descType)) {
            if (versionIds) versionIds[count] = providers[i]->GetId();
            if (versionNames) versionNames[count] = providers[i]->GetVersionName();
            count++;
        }
    return count;
}

uint64_t GetProviderCount(ffxStructType_t descType, void* device)
{
    return GetProviderVersions(descType, device, UINT64_MAX, nullptr, nullptr);
}
