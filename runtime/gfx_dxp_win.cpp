// The DXGI presenter (gfx_dxp.h): a Direct3D 12 swapchain on the window, fed from images shared with Vulkan.
//
// Direct3D 12 opens the adapter of the Vulkan device (the same LUID), so the shared images and fences stay on one GPU;
// DXGI takes the picture to the display from there, whichever adapter drives it. The swapchain is flip-model: its back
// buffers cannot be shared, so each present copies the shared image into the current one (dxp_present).
//
// d3d12.dll and dxgi.dll are loaded when the presenter is first asked for: a host without them, or a run that never
// wants HDR through DXGI, does not load them.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

extern "C" {
#include "gfx_dxp.h"
#include "host.h"
#include "vk.h"
HWND gfx_window_handle(void);   // gfx_window_win.c
}

namespace {

struct Dxp {
    int tried, ok;
    IDXGIFactory6 *factory;
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc[DXP_IMAGES];   // one per shared image: reset when that image's last copy is done
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *f_rendered, *f_copied, *f_idle;
    VkSemaphore s_rendered, s_copied;            // the two shared fences as Vulkan timeline semaphores
    HANDLE ev;
    uint64_t idle_n;
    int tearing_ok;
    // the swapchain and what it shows
    IDXGISwapChain4 *sc;
    uint32_t w, h;
    DXGI_FORMAT fmt;
    int hdr;
    ID3D12Resource *shared[DXP_IMAGES];
    VkImage img[DXP_IMAGES];
    VkDeviceMemory mem[DXP_IMAGES];
    uint64_t last[DXP_IMAGES];                   // the copy (its value on f_copied) that last read the image
} D;

template <class T> void drop(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

// A shared fence as a Vulkan timeline semaphore; VK_NULL_HANDLE when the driver does not import it.
VkSemaphore import_fence(VkCtx *c, ID3D12Fence *f) {
    auto import = (PFN_vkImportSemaphoreWin32HandleKHR)vk_dpa(c->device, "vkImportSemaphoreWin32HandleKHR");
    HANDLE h = nullptr;
    if (!import || FAILED(D.dev->CreateSharedHandle(f, nullptr, GENERIC_ALL, nullptr, &h))) return VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
    VkSemaphore s = VK_NULL_HANDLE;
    if (vkCreateSemaphore(c->device, &si, nullptr, &s) == VK_SUCCESS) {
        VkImportSemaphoreWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR, nullptr, s, 0,
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, h, nullptr};
        VkResult r = import(c->device, &ii);
        if (r != VK_SUCCESS) {
            rt_log("dxp: the driver does not take a Direct3D 12 fence as a timeline semaphore (%d)", (int)r);
            vkDestroySemaphore(c->device, s, nullptr);
            s = VK_NULL_HANDLE;
        }
    }
    CloseHandle(h);   // the import keeps its own reference
    return s;
}

void release_device() {
    VkCtx *c = vk_ctx();
    if (c && D.s_rendered) vkDestroySemaphore(c->device, D.s_rendered, nullptr);
    if (c && D.s_copied) vkDestroySemaphore(c->device, D.s_copied, nullptr);
    D.s_rendered = D.s_copied = VK_NULL_HANDLE;
    drop(D.f_rendered), drop(D.f_copied), drop(D.f_idle), drop(D.list);
    for (auto &a : D.alloc) drop(a);
    drop(D.queue), drop(D.dev), drop(D.factory);
    if (D.ev) CloseHandle(D.ev);
    D.ev = nullptr;
}

// Direct3D 12 on the Vulkan device's adapter, its queue and the shared fences. Once per Vulkan device.
int init() {
    if (D.tried) return D.ok;
    D.tried = 1;
    VkCtx *c = vk_ctx();
    if (!c || !c->has_win32_sharing) return 0;
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    if (!id.deviceLUIDValid) return rt_log("dxp: the Vulkan device names no adapter"), 0;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll"), d3d12 = LoadLibraryW(L"d3d12.dll");
    typedef HRESULT(WINAPI * MakeFactory)(UINT, REFIID, void **);
    typedef HRESULT(WINAPI * MakeDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    auto make_factory = dxgi ? (MakeFactory)(void *)GetProcAddress(dxgi, "CreateDXGIFactory2") : nullptr;
    auto make_device = d3d12 ? (MakeDevice)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    if (!make_factory || !make_device) return rt_log("dxp: no Direct3D 12 on this host"), 0;
    LUID luid;
    memcpy(&luid, id.deviceLUID, sizeof luid);
    IDXGIAdapter1 *adapter = nullptr;
    HRESULT hr = make_factory(0, IID_PPV_ARGS(&D.factory));
    if (SUCCEEDED(hr)) hr = D.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr)) hr = make_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&D.dev));
    DXGI_ADAPTER_DESC1 ad = {};
    if (adapter) adapter->GetDesc1(&ad);
    drop(adapter);
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = D.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&D.queue));
    for (int i = 0; i < DXP_IMAGES && SUCCEEDED(hr); i++)
        hr = D.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&D.alloc[i]));
    if (SUCCEEDED(hr)) hr = D.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D.alloc[0], nullptr, IID_PPV_ARGS(&D.list));
    if (SUCCEEDED(hr)) hr = D.list->Close();
    if (SUCCEEDED(hr)) hr = D.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&D.f_rendered));
    if (SUCCEEDED(hr)) hr = D.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&D.f_copied));
    if (SUCCEEDED(hr)) hr = D.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&D.f_idle));
    if (FAILED(hr)) {
        rt_log("dxp: Direct3D 12 did not start on the Vulkan device's adapter (%08lx)", (unsigned long)hr);
        return release_device(), 0;
    }
    D.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D.s_rendered = import_fence(c, D.f_rendered);
    D.s_copied = D.s_rendered ? import_fence(c, D.f_copied) : VK_NULL_HANDLE;
    if (!D.ev || !D.s_copied) return release_device(), 0;
    BOOL tearing = FALSE;
    D.tearing_ok = SUCCEEDED(D.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) && tearing;
    rt_log("dxp: Direct3D 12 on %ls for the DXGI presenter", ad.Description);
    return D.ok = 1;
}

// Waits until the Direct3D queue has run everything queued so far (5 s at most: a copy that waits for a frame Vulkan
// never finishes would hold it for good).
void queue_idle() {
    if (!D.queue) return;
    if (FAILED(D.queue->Signal(D.f_idle, ++D.idle_n))) return;
    if (D.f_idle->GetCompletedValue() >= D.idle_n) return;
    if (SUCCEEDED(D.f_idle->SetEventOnCompletion(D.idle_n, D.ev)) && WaitForSingleObject(D.ev, 5000) != WAIT_OBJECT_0)
        rt_log("dxp: the Direct3D queue did not drain in 5 s");
}

void free_images() {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < DXP_IMAGES; i++) {
        if (c && D.img[i]) vkDestroyImage(c->device, D.img[i], nullptr);
        if (c && D.mem[i]) vkFreeMemory(c->device, D.mem[i], nullptr);
        D.img[i] = VK_NULL_HANDLE, D.mem[i] = VK_NULL_HANDLE, D.last[i] = 0;
        drop(D.shared[i]);
    }
}

// One image of both APIs: a shared Direct3D 12 texture, and a Vulkan image on its memory.
int make_image(VkCtx *c, int i, VkFormat vf) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = D.w, rd.Height = D.h, rd.DepthOrArraySize = 1, rd.MipLevels = 1, rd.Format = D.fmt, rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    HRESULT hr = D.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&D.shared[i]));
    HANDLE h = nullptr;
    if (SUCCEEDED(hr)) hr = D.dev->CreateSharedHandle(D.shared[i], nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr)) return rt_log("dxp: no shared texture %ux%u (%08lx)", D.w, D.h, (unsigned long)hr), 0;
    const VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkExternalMemoryImageCreateInfo ei = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, (VkExternalMemoryHandleTypeFlags)type};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ei};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = vf, ci.extent = {D.w, D.h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // what the present does with a swapchain image: blits and clears into it, the menu draws into it, shots read it
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE, ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(c->device, &ci, nullptr, &D.img[i]);
    if (r == VK_SUCCESS) {
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(c->device, D.img[i], &mr);
        uint32_t types = mr.memoryTypeBits;
        auto props = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vk_dpa(c->device, "vkGetMemoryWin32HandlePropertiesKHR");
        VkMemoryWin32HandlePropertiesKHR hp2 = {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (props && props(c->device, type, h, &hp2) == VK_SUCCESS && (types & hp2.memoryTypeBits)) types &= hp2.memoryTypeBits;
        uint32_t ti = 0;
        while (ti < 31 && !(types >> ti & 1)) ti++;
        VkMemoryDedicatedAllocateInfo di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, D.img[i], VK_NULL_HANDLE};
        VkImportMemoryWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &di, type, h, nullptr};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ii, mr.size, ti};
        r = vkAllocateMemory(c->device, &ai, nullptr, &D.mem[i]);
        if (r == VK_SUCCESS) r = vkBindImageMemory(c->device, D.img[i], D.mem[i], 0);
    }
    CloseHandle(h);   // the import keeps its own reference
    if (r != VK_SUCCESS) return rt_log("dxp: Vulkan did not take the shared texture (%d)", (int)r), 0;
    return 1;
}

// The swapchain at D.w x D.h in D.fmt: a new one, or the old one with new buffers.
int make_swapchain() {
    const UINT flags = D.tearing_ok ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (D.sc && FAILED(D.sc->ResizeBuffers(DXP_IMAGES, D.w, D.h, D.fmt, flags))) drop(D.sc);
    if (D.sc) return 1;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = D.w, sd.Height = D.h, sd.Format = D.fmt, sd.SampleDesc.Count = 1, sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = DXP_IMAGES, sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD, sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE, sd.Flags = flags;
    IDXGISwapChain1 *sc1 = nullptr;
    HWND hwnd = gfx_window_handle();
    HRESULT hr = D.factory->CreateSwapChainForHwnd(D.queue, hwnd, &sd, nullptr, nullptr, &sc1);
    if (SUCCEEDED(hr)) hr = sc1->QueryInterface(IID_PPV_ARGS(&D.sc));
    drop(sc1);
    if (FAILED(hr)) return rt_log("dxp: no DXGI swapchain %ux%u (%08lx)", D.w, D.h, (unsigned long)hr), 0;
    D.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);   // the window's keys stay its own
    return 1;
}

void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r, b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from, b.Transition.StateAfter = to;
    D.list->ResourceBarrier(1, &b);
}

}   // namespace

extern "C" int dxp_hdr_display(void) {
    if (!init()) return 0;
    // the factory's list of outputs is from when it was made: a new one after a display or its HDR switch changed
    if (!D.factory->IsCurrent()) {
        IDXGIFactory6 *f = nullptr;
        auto make_factory = (HRESULT(WINAPI *)(UINT, REFIID, void **))(void *)GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory2");
        if (!D.sc && make_factory && SUCCEEDED(make_factory(0, IID_PPV_ARGS(&f)))) drop(D.factory), D.factory = f;
    }
    HMONITOR mon = MonitorFromWindow(gfx_window_handle(), MONITOR_DEFAULTTONEAREST);
    int hdr = 0;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; D.factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput *o = nullptr;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            IDXGIOutput6 *o6 = nullptr;
            DXGI_OUTPUT_DESC1 od;
            if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6))) && SUCCEEDED(o6->GetDesc1(&od)) && od.Monitor == mon)
                hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            drop(o6), drop(o);
        }
        drop(a);
    }
    return hdr;
}

extern "C" int dxp_create(uint32_t w, uint32_t h, int *hdr, VkFormat *fmt, VkImage imgs[DXP_IMAGES]) {
    VkCtx *c = vk_ctx();
    if (!init() || !c || !w || !h) return 0;
    queue_idle();
    free_images();
    D.w = w, D.h = h;
    for (int want = *hdr;; want = 0) {
        D.fmt = want ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!make_swapchain()) return 0;
        const DXGI_COLOR_SPACE_TYPE cs = want ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        UINT sup = 0;
        if (SUCCEEDED(D.sc->CheckColorSpaceSupport(cs, &sup)) && (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
            SUCCEEDED(D.sc->SetColorSpace1(cs))) {
            D.hdr = want;
            break;
        }
        if (!want) return rt_log("dxp: the swapchain takes no sRGB picture"), 0;
        rt_log("dxp: the swapchain takes no HDR10 picture; sRGB instead");
    }
    *fmt = D.hdr ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_UNORM;
    for (int i = 0; i < DXP_IMAGES; i++) {
        if (!make_image(c, i, *fmt)) return free_images(), 0;
        imgs[i] = D.img[i];
    }
    static uint32_t lw, lh;
    static int lhdr = -1;
    if (lw != w || lh != h || lhdr != D.hdr)
        rt_log("dxp: DXGI swapchain %ux%u, %s", w, h, D.hdr ? "HDR10 (10 bits, PQ in Rec. 2020)" : "sRGB (8 bits)");
    lw = w, lh = h, lhdr = D.hdr;
    *hdr = D.hdr;
    return 1;
}

extern "C" void dxp_destroy(void) {
    queue_idle();
    free_images();
    drop(D.sc);
    release_device();
    D.tried = D.ok = 0;
}

extern "C" VkSemaphore dxp_rendered(void) { return D.s_rendered; }
extern "C" VkSemaphore dxp_copied(uint32_t idx, uint64_t *value) {
    *value = idx < DXP_IMAGES ? D.last[idx] : 0;
    return D.s_copied;
}

extern "C" int dxp_present(uint32_t idx, uint64_t n, int sync, int tearing) {
    if (!D.sc || idx >= DXP_IMAGES || !D.shared[idx]) return 0;
    // the allocator of this image's last copy is reset: that copy has to be done (it nearly always is, three presents on)
    if (D.f_copied->GetCompletedValue() < D.last[idx] && SUCCEEDED(D.f_copied->SetEventOnCompletion(D.last[idx], D.ev)))
        WaitForSingleObject(D.ev, 2000);
    ID3D12Resource *bb = nullptr;
    if (FAILED(D.sc->GetBuffer(D.sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) return 0;
    D.alloc[idx]->Reset();
    D.list->Reset(D.alloc[idx], nullptr);
    transition(D.shared[idx], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D.list->CopyResource(bb, D.shared[idx]);
    transition(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(D.shared[idx], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    D.list->Close();
    bb->Release();   // the swapchain holds it
    D.queue->Wait(D.f_rendered, n);   // on the GPU's side: the copy runs once Vulkan has finished the frame
    ID3D12CommandList *lists[] = {D.list};
    D.queue->ExecuteCommandLists(1, lists);
    D.queue->Signal(D.f_copied, n);
    D.last[idx] = n;
    HRESULT hr = D.sc->Present(sync ? 1 : 0, !sync && tearing && D.tearing_ok ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        static int told;
        if (!told++) rt_log("dxp: Present failed (%08lx)", (unsigned long)hr);
        return 0;
    }
    return 1;
}

extern "C" void dxp_metadata(float peak_nits, float frame_nits) {
    if (!D.sc || !D.hdr) return;
    DXGI_HDR_METADATA_HDR10 m = {};   // Rec. 2020 primaries and D65, in 1/50000
    m.RedPrimary[0] = 35400, m.RedPrimary[1] = 14600, m.GreenPrimary[0] = 8500, m.GreenPrimary[1] = 39850;
    m.BluePrimary[0] = 6550, m.BluePrimary[1] = 2300, m.WhitePoint[0] = 15635, m.WhitePoint[1] = 16450;
    m.MaxMasteringLuminance = (UINT)peak_nits, m.MinMasteringLuminance = 0;
    m.MaxContentLightLevel = (UINT16)(peak_nits > 65535 ? 65535 : peak_nits), m.MaxFrameAverageLightLevel = (UINT16)frame_nits;
    D.sc->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof m, &m);
}
