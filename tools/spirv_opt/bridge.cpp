#include <spirv-tools/optimizer.hpp>
#include <spirv-tools/libspirv.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// The caller allocates the result so no allocator ownership crosses the DLL boundary.
#ifdef _WIN32
#define OPT_EXPORT __declspec(dllexport)
#else
#define OPT_EXPORT __attribute__((visibility("default")))
#endif
extern "C" OPT_EXPORT int nx2navi_spirv_opt_v1(
    const uint32_t *input, size_t words, void *(*allocate)(size_t), uint32_t **output, size_t *output_words) {
    if (!input || words < 5 || !allocate || !output || !output_words) return 0;
    *output = nullptr; *output_words = 0;
    try {
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
        optimizer.RegisterPerformancePasses(true);
        spvtools::OptimizerOptions options;
        options.set_preserve_bindings(true);
        options.set_preserve_spec_constants(true);
        std::vector<uint32_t> result;
        if (!optimizer.Run(input, words, &result, options)) return 0;
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
        if (!validator.Validate(result)) return 0;
        auto *copy = static_cast<uint32_t *>(allocate(result.size() * sizeof(uint32_t)));
        if (!copy) return 0;
        std::memcpy(copy, result.data(), result.size() * sizeof(uint32_t));
        *output = copy; *output_words = result.size();
        return 1;
    } catch (...) { return 0; }
}
