// The entry points of AMD's FSR SDK 2.4 loader (amd_fidelityfx_loader_vk.dll, which vk_ffx4.cpp loads), for
// vk_fg4.cpp. C++ and Windows only, and never in one file with vk_ffx.h's API section: the two sets of headers name
// the same types with other layouts.
#pragma once
#include <stdint.h>

#include "third_party/ffx_sdk/api/include/ffx_api.h"
#include "third_party/ffx_sdk/api/include/vk/ffx_api_vk.h"

extern "C" {
// The versions the loader lists for one kind of context (a FFX_API_CREATE_CONTEXT_DESC_TYPE_*) on this device, in
// its order, newest first: their count, at most cap. The names are the library's memory. 0 without the loader, before
// the device exists, or when it lists none (an effect whose library is not beside the executable).
int vk_ffx4_api_versions(uint64_t create_type, uint64_t *ids, const char **names, int cap);
int vk_ffx4_api_file(const char *name);   // a file of that name is next to the executable (one of the SDK's libraries)
// The API's calls (0 = FFX_API_RETURN_OK), valid once vk_ffx4_api_versions returned a count.
ffxReturnCode_t vk_ffx4_api_create(ffxContext *ctx, ffxCreateContextDescHeader *desc);
ffxReturnCode_t vk_ffx4_api_destroy(ffxContext *ctx);
ffxReturnCode_t vk_ffx4_api_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc);
ffxReturnCode_t vk_ffx4_api_query(ffxContext *ctx, ffxQueryDescHeader *desc);
ffxReturnCode_t vk_ffx4_api_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc);
}
