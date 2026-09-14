#pragma once

#include "MaaUtils/NoWarningCVMat.hpp"

#include <array>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace asst
{
class MaskedCcoeffMatcher
{
public:
    struct Stats
    {
        uint64_t opencv_fallbacks = 0;
        uint64_t sparse_calls = 0;
        uint64_t fft_calls = 0;
        uint64_t spectrum_cache_hits = 0;
        uint64_t spectrum_cache_misses = 0;
        uint64_t workspace_rebuilds = 0;
        uint64_t spectrum_cache_bytes = 0;
        uint64_t image_preparations = 0;
    };

    struct PreparedImage
    {
        cv::Size size;
        std::array<cv::Mat, 3> channels;
    };

    static MaskedCcoeffMatcher& get_instance();

    void sync_cache_revision(uint64_t revision);
    Stats stats() const;
    void reset_stats();
    std::optional<PreparedImage> prepare_image(const cv::Mat& image_rgb);

    cv::Mat match(const cv::Mat& image_rgb, const cv::Mat& templ_rgb, const cv::Mat& mask_u8);
    cv::Mat match(const PreparedImage& image, const cv::Mat& templ_rgb, const cv::Mat& mask_u8);

    enum class MatchStrategy
    {
        OpenCV,
        Sparse,
        Fft,
    };

    // 路径规划只依赖 mask 像素数与结果矩阵规模；Matcher 用它做 prepare_image 之前的预判，
    // match() 内部会用同一函数再算一次，两者输入一致时结果必然一致。
    static MatchStrategy choose_strategy(int mask_pixels, int result_positions);

private:
    struct TemplatePlan;
    struct TemplateSpectrum;

    struct TemplateKey
    {
        uint64_t revision = 0;
        uint64_t template_hash = 0;
        uint64_t mask_hash = 0;
        int template_rows = 0;
        int template_cols = 0;
        int template_type = 0;
        int mask_rows = 0;
        int mask_cols = 0;
        int mask_type = 0;

        bool operator==(const TemplateKey&) const = default;
    };

    struct SpectrumKey
    {
        TemplateKey template_key;
        int dft_rows = 0;
        int dft_cols = 0;

        bool operator==(const SpectrumKey&) const = default;
    };

    struct TemplateKeyHash
    {
        size_t operator()(const TemplateKey& key) const noexcept;
    };

    struct SpectrumKeyHash
    {
        size_t operator()(const SpectrumKey& key) const noexcept;
    };

    struct CacheEntry
    {
        std::shared_ptr<const TemplatePlan> plan;
        std::list<TemplateKey>::iterator lru_it;
        size_t bytes;
    };

    struct SpectrumCacheEntry
    {
        std::shared_ptr<const TemplateSpectrum> spectrum;
        std::list<SpectrumKey>::iterator lru_it;
        size_t bytes;
    };

    static void fnv1a_update(uint64_t& h, const void* data, size_t size);
    static uint64_t hash_mat(const cv::Mat& mat);
    static bool is_binary_mask(const cv::Mat& mask_u8);
    static TemplateKey make_template_key(const cv::Mat& templ_rgb, const cv::Mat& mask_u8, uint64_t revision);
    static size_t calc_plan_bytes(const TemplatePlan& plan);

    std::shared_ptr<const TemplatePlan>
        get_or_build_template_plan(const TemplateKey& template_key, const cv::Mat& templ_rgb, const cv::Mat& mask_u8);
    std::shared_ptr<const TemplateSpectrum> get_or_build_template_spectrum(
        const TemplateKey& template_key,
        const TemplatePlan& plan,
        int dft_rows,
        int dft_cols);

    static constexpr size_t k_max_cache_bytes = 64ULL * 1024 * 1024; // 64 MB
#ifdef __ANDROID__
    static constexpr size_t k_max_spectrum_cache_bytes = 16ULL * 1024 * 1024;
#else
    static constexpr size_t k_max_spectrum_cache_bytes = 64ULL * 1024 * 1024;
#endif

    std::mutex m_cache_mtx;
    std::list<TemplateKey> m_lru_list;
    std::unordered_map<TemplateKey, CacheEntry, TemplateKeyHash> m_template_plan_cache;
    size_t m_cache_total_bytes { 0 };
    std::list<SpectrumKey> m_spectrum_lru_list;
    std::unordered_map<SpectrumKey, SpectrumCacheEntry, SpectrumKeyHash> m_spectrum_cache;
    size_t m_spectrum_cache_total_bytes { 0 };
    std::atomic<uint64_t> m_cache_revision { 0 };
    std::atomic<uint64_t> m_opencv_fallbacks { 0 };
    std::atomic<uint64_t> m_sparse_calls { 0 };
    std::atomic<uint64_t> m_fft_calls { 0 };
    std::atomic<uint64_t> m_spectrum_cache_hits { 0 };
    std::atomic<uint64_t> m_spectrum_cache_misses { 0 };
    std::atomic<uint64_t> m_workspace_rebuilds { 0 };
    std::atomic<uint64_t> m_spectrum_cache_bytes { 0 };
    std::atomic<uint64_t> m_image_preparations { 0 };
};
}
