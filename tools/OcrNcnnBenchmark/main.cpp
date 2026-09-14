#include "Config/Miscellaneous/OcrPack.h"
#include "MaaUtils/NoWarningCV.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
class BenchmarkOcr final : public asst::OcrPack
{
public:
    BenchmarkOcr() = default;
};

int parse_nonnegative(std::string_view value, std::string_view name)
{
    size_t parsed = 0;
    const int result = std::stoi(std::string(value), &parsed);
    if (parsed != value.size() || result < 0) {
        throw std::invalid_argument(std::string(name) + " must be a non-negative integer");
    }
    return result;
}

double percentile(const std::vector<double>& sorted, double fraction)
{
    const size_t index = static_cast<size_t>(std::ceil(fraction * sorted.size())) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}
}

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 6) {
        std::cerr << "usage: ocr_ncnn_benchmark RESOURCE IMAGE cpu|DEVICE [WARMUP] [REPEATS]\n";
        return 2;
    }

    try {
        const std::filesystem::path resource = argv[1];
        const cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cerr << "image load failed: " << argv[2] << '\n';
            return 3;
        }

        const std::string_view backend = argv[3];
        const int warmup = argc >= 5 ? parse_nonnegative(argv[4], "WARMUP") : 5;
        const int repeats = argc >= 6 ? parse_nonnegative(argv[5], "REPEATS") : 50;
        if (repeats == 0) {
            throw std::invalid_argument("REPEATS must be greater than zero");
        }

        BenchmarkOcr ocr;
        if (backend == "cpu") {
            ocr.use_cpu();
        }
        else {
            const auto selector = asst::GpuDeviceSelector::parse(backend);
            if (!selector) {
                throw std::invalid_argument("DEVICE must be a non-negative integer");
            }
            ocr.use_gpu(*selector);
        }
        if (!ocr.load(resource)) {
            std::cerr << "model load failed: " << resource << '\n';
            return 4;
        }

        size_t result_count = 0;
        for (int i = 0; i < warmup; ++i) {
            result_count = ocr.recognize(image, false).size();
        }
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(repeats));
        for (int i = 0; i < repeats; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            result_count = ocr.recognize(image, false).size();
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin);
            samples.emplace_back(elapsed.count());
        }

        const bool requested_gpu = backend != "cpu";
        const bool actual_gpu = ocr.is_using_gpu();
        if (actual_gpu != requested_gpu) {
            std::cerr << "requested backend " << backend << " is unavailable; actual backend is "
                      << (actual_gpu ? "Vulkan" : "CPU") << '\n';
            return 5;
        }

        std::ranges::sort(samples);
        const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
        std::cout << std::fixed << std::setprecision(3) << "backend=" << backend << " warmup=" << warmup
                  << " repeats=" << repeats << " results=" << result_count << " mean_ms=" << total / samples.size()
                  << " p50_ms=" << percentile(samples, 0.50) << " p95_ms=" << percentile(samples, 0.95)
                  << " min_ms=" << samples.front() << " max_ms=" << samples.back() << '\n';
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    return 0;
}
