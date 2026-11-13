// Exercise the adaptive controller with synthetic frame work and a deterministic clock.
#include "vk_dlssg.c"
static uint64_t now_ns = 5000000000ull;
static int open_menu;
const char *cfg_value(const char *key) { (void)key; return "adaptive"; }
uint64_t kern_now_ns(void) { return now_ns; }
int menu_is_open(void) { return open_menu; }
void rt_log(const char *fmt, ...) { (void)fmt; }
const char *cfg_get(const char *key) { (void)key; return NULL; }
int vk_dlaa_fg_inputs(VkFfxImage *d, VkFfxImage *m, float *x, float *y, int *r) { (void)d;(void)m;(void)x;(void)y;(void)r;return 0; }
int vk_dlaa_fg_views(VkImageView *d, VkImageView *m) { (void)d;(void)m;return 0; }
int vk_dlaa_fg_camera(VkDlaaCamera *c) { (void)c;return 0; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *a,const VkAllocationCallbacks *b,VkInstance *c) { (void)a;(void)b;(void)c;return VK_ERROR_INITIALIZATION_FAILED; }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance a,uint32_t *b,VkPhysicalDevice *c) { (void)a;(void)b;(void)c;return VK_ERROR_INITIALIZATION_FAILED; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice a,const VkDeviceCreateInfo *b,const VkAllocationCallbacks *c,VkDevice *d) { (void)a;(void)b;(void)c;(void)d;return VK_ERROR_INITIALIZATION_FAILED; }
static uint32_t frames_max(void) { return 1; }
int main(void) {
    S.active = 1; S.frames_max = frames_max; S.shown_mult = 2;
    for (int i = 0; i < 120; i++) vk_dlssg_adaptive_sample(0.010, 60);
    if (A.mult != 1) { puts("fast frames did not reduce generation"); return 1; }
    S.shown_mult = 1; now_ns += 5000000000ull;
    for (int i = 0; i < 60; i++) vk_dlssg_adaptive_sample(0.025, 60);
    if (A.mult != 2) { puts("slow frames did not increase generation"); return 1; }
    S.shown_mult = 2; open_menu = 1; now_ns += 5000000000ull;
    for (int i = 0; i < 120; i++) vk_dlssg_adaptive_sample(0.010, 60);
    if (A.mult != 2 || A.n != 0) { puts("menu samples were accepted"); return 1; }
    puts("adaptive_framegen_check: ok"); return 0;
}
