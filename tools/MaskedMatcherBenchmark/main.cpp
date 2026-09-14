#include "MaaUtils/NoWarningCV.hpp"
#include "Vision/MaskedCcoeffMatcher.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
constexpr int ImageWidth = 480;
constexpr int ImageHeight = 320;
constexpr int TemplateSize = 64;
constexpr int SparseBatchTemplateSize = 36;
constexpr int SparseBatchTemplateCount = 32;

cv::Mat make_random_mat(cv::Size size, int type, uint64_t seed)
{
    cv::Mat result(size, type);
    cv::RNG rng(seed);
    rng.fill(result, cv::RNG::UNIFORM, 0, 256);
    return result;
}

cv::Mat make_mask(cv::Size size, uint64_t seed, int threshold)
{
    const cv::Mat values = make_random_mat(size, CV_8UC1, seed);
    cv::Mat mask;
    cv::threshold(values, mask, threshold, 255, cv::THRESH_BINARY);
    return mask;
}

double max_error(const cv::Mat& actual, const cv::Mat& expected)
{
    return actual.empty() ? std::numeric_limits<double>::infinity() : cv::norm(actual, expected, cv::NORM_INF);
}

template <typename Function>
double mean_milliseconds(int repeats, Function&& function)
{
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < repeats; ++i) {
        function();
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / repeats;
}
}

int main(int argc, char** argv)
{
    const int repeats = argc >= 2 ? std::atoi(argv[1]) : 20;
    const int sparse_repeats = argc >= 3 ? std::atoi(argv[2]) : repeats;
    if (repeats <= 0 || sparse_repeats <= 0) {
        std::cerr << "REPEATS and SPARSE_REPEATS must be greater than zero\n";
        return 2;
    }

    const cv::Mat image = make_random_mat({ ImageWidth, ImageHeight }, CV_8UC3, 0x4d4141);
    const cv::Mat templ = make_random_mat({ TemplateSize, TemplateSize }, CV_8UC3, 0x4e434e4e);
    const cv::Mat mask = make_mask(templ.size(), 0x56495349, 63);
    const cv::Mat sparse_image = make_random_mat({ 40, 40 }, CV_8UC3, 0x53504152);

    cv::Mat expected;
    cv::matchTemplate(image, templ, expected, cv::TM_CCOEFF_NORMED, mask);

    auto& matcher = asst::MaskedCcoeffMatcher::get_instance();

    cv::Mat unsupported_image(32, 32, CV_8UC4, cv::Scalar(0, 0, 0, 255));
    if (matcher.prepare_image(unsupported_image)) {
        std::cerr << "four-channel input was accepted\n";
        return 2;
    }
#ifdef VERIFY_SPECTRUM_CACHE_STATS
    matcher.reset_stats();
#endif
    const auto cold_begin = std::chrono::steady_clock::now();
    const cv::Mat cold = matcher.match(image, templ, mask);
    const double cold_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cold_begin).count();
    const cv::Mat warm = matcher.match(image, templ, mask);
#ifdef VERIFY_SPECTRUM_CACHE_STATS
    const auto stats = matcher.stats();
#endif

    if (cold.empty() || warm.empty()) {
        std::cerr << "custom matcher unexpectedly selected fallback\n";
        return 1;
    }

    const double cold_error = max_error(cold, expected);
    const double warm_error = max_error(warm, expected);
    if (!std::isfinite(cold_error) || cold_error > 1e-4 || warm_error > 1e-4) {
        std::cerr << "result mismatch: cold=" << cold_error << " warm=" << warm_error << '\n';
        return 1;
    }
#ifdef VERIFY_SPECTRUM_CACHE_STATS
    constexpr uint64_t MaxPackedSpectrumBytes = 8ULL * 1024 * 1024;
    if (stats.spectrum_cache_misses != 1 || stats.spectrum_cache_hits != 1 || stats.workspace_rebuilds != 1 ||
        stats.spectrum_cache_bytes == 0 || stats.spectrum_cache_bytes > MaxPackedSpectrumBytes) {
        std::cerr << "unexpected spectrum cache stats: hits=" << stats.spectrum_cache_hits
                  << " misses=" << stats.spectrum_cache_misses << " workspace_rebuilds=" << stats.workspace_rebuilds
                  << " spectrum_cache_bytes=" << stats.spectrum_cache_bytes << '\n';
        return 1;
    }

    matcher.sync_cache_revision(1);
    matcher.reset_stats();
    const cv::Mat after_revision = matcher.match(image, templ, mask);
    const auto revision_stats = matcher.stats();
    if (max_error(after_revision, expected) > 1e-4 || revision_stats.spectrum_cache_misses != 1 ||
        revision_stats.spectrum_cache_hits != 0) {
        std::cerr << "spectrum cache was not invalidated by resource revision\n";
        return 1;
    }

    const cv::Mat sparse_templ = make_random_mat({ 16, 16 }, CV_8UC3, 0x5345544d);
    const cv::Mat sparse_mask = make_mask(sparse_templ.size(), 0x534d4153, 127);
    cv::Mat sparse_expected;
    cv::matchTemplate(sparse_image, sparse_templ, sparse_expected, cv::TM_CCOEFF_NORMED, sparse_mask);
    matcher.reset_stats();
    const cv::Mat sparse_actual = matcher.match(sparse_image, sparse_templ, sparse_mask);
    if (max_error(sparse_actual, sparse_expected) > 1e-4 || matcher.stats().sparse_calls != 1) {
        std::cerr << "sparse path validation failed\n";
        return 1;
    }

    const cv::Mat fallback_image = make_random_mat({ 80, 80 }, CV_8UC3, 0x46414c4c);
    matcher.reset_stats();
    const cv::Mat fallback = matcher.match(fallback_image, sparse_templ, sparse_mask);
    if (!fallback.empty() || matcher.stats().opencv_fallbacks != 1) {
        std::cerr << "OpenCV fallback path validation failed\n";
        return 1;
    }

    matcher.reset_stats();
    const auto prepared_image = matcher.prepare_image(sparse_image);
    if (!prepared_image) {
        std::cerr << "three-channel input was rejected\n";
        return 2;
    }
    const cv::Mat prepared_actual = matcher.match(*prepared_image, sparse_templ, sparse_mask);
    const cv::Mat second_templ = make_random_mat({ 16, 16 }, CV_8UC3, 0x53455432);
    const cv::Mat second_mask = make_mask(second_templ.size(), 0x534d4132, 127);
    cv::Mat second_expected;
    cv::matchTemplate(sparse_image, second_templ, second_expected, cv::TM_CCOEFF_NORMED, second_mask);
    const cv::Mat second_actual = matcher.match(*prepared_image, second_templ, second_mask);
    if (max_error(prepared_actual, sparse_expected) > 1e-4 || max_error(second_actual, second_expected) > 1e-4 ||
        matcher.stats().image_preparations != 1) {
        std::cerr << "prepared image reuse validation failed\n";
        return 1;
    }

#endif

    std::vector<cv::Mat> batch_templates;
    std::vector<cv::Mat> batch_masks;
    batch_templates.reserve(SparseBatchTemplateCount);
    batch_masks.reserve(SparseBatchTemplateCount);
    for (int i = 0; i < SparseBatchTemplateCount; ++i) {
        batch_templates.emplace_back(
            make_random_mat({ SparseBatchTemplateSize, SparseBatchTemplateSize }, CV_8UC3, 0x42415400 + i));
        batch_masks.emplace_back(make_mask(batch_templates.back().size(), 0x4d415300 + i, 127));
    }

    cv::Mat batch_result;
    for (int i = 0; i < SparseBatchTemplateCount; ++i) {
        batch_result = matcher.match(sparse_image, batch_templates[i], batch_masks[i]);
        if (batch_result.empty()) {
            std::cerr << "sparse batch unexpectedly selected fallback\n";
            return 1;
        }
    }
    cv::Mat batch_expected;
    cv::matchTemplate(sparse_image, batch_templates.back(), batch_expected, cv::TM_CCOEFF_NORMED, batch_masks.back());
    if (max_error(batch_result, batch_expected) > 1e-4) {
        std::cerr << "sparse batch result mismatch\n";
        return 1;
    }

    const double sparse_batch_legacy_ms = mean_milliseconds(sparse_repeats, [&] {
        for (int i = 0; i < SparseBatchTemplateCount; ++i) {
            batch_result = matcher.match(sparse_image, batch_templates[i], batch_masks[i]);
        }
    });
#ifdef VERIFY_SPECTRUM_CACHE_STATS
    const double sparse_batch_prepared_ms = mean_milliseconds(sparse_repeats, [&] {
        const auto prepared = matcher.prepare_image(sparse_image);
        if (!prepared) {
            return;
        }
        for (int i = 0; i < SparseBatchTemplateCount; ++i) {
            batch_result = matcher.match(*prepared, batch_templates[i], batch_masks[i]);
        }
    });
#endif

    cv::Mat custom_result;
    const double custom_mean_ms =
        mean_milliseconds(repeats, [&] { custom_result = matcher.match(image, templ, mask); });
    cv::Mat opencv_result;
    const double opencv_mean_ms =
        mean_milliseconds(repeats, [&] { cv::matchTemplate(image, templ, opencv_result, cv::TM_CCOEFF_NORMED, mask); });

    std::cout << "max_error=" << std::max(cold_error, warm_error)
#ifdef VERIFY_SPECTRUM_CACHE_STATS
              << " spectrum_cache_hits=" << stats.spectrum_cache_hits
              << " spectrum_cache_misses=" << stats.spectrum_cache_misses
              << " workspace_rebuilds=" << stats.workspace_rebuilds
              << " spectrum_cache_bytes=" << stats.spectrum_cache_bytes
              << " sparse_batch_prepared_ms=" << sparse_batch_prepared_ms
#endif
              << " sparse_batch_legacy_ms=" << sparse_batch_legacy_ms << " cold_ms=" << cold_ms
              << " custom_mean_ms=" << custom_mean_ms << " opencv_mean_ms=" << opencv_mean_ms << '\n';
    return 0;
}
