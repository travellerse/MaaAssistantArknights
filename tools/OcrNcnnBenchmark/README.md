# NCNN OCR benchmark

This optional Linux target measures the full detector and recognizer pipeline.
Results are informational: GPU performance is a soft gate because public CI
runners do not expose a Vulkan device.

Configure and build it in an existing Linux build tree:

```bash
cmake -S . -B build/linux-ncnn-x64 -DBUILD_OCR_BENCHMARK=ON -DMAA_LINUX_OCR_BACKEND=ncnn
cmake --build build/linux-ncnn-x64 --config RelWithDebInfo \
  --target ocr_ncnn_benchmark --parallel
```

Run CPU and Vulkan device 0 with 10 warmups and 200 measured iterations:

```bash
build/linux-ncnn-x64/bin/RelWithDebInfo/ocr_ncnn_benchmark \
  install/resource/PaddleOCR screenshot.png cpu 10 200
build/linux-ncnn-x64/bin/RelWithDebInfo/ocr_ncnn_benchmark \
  install/resource/PaddleOCR screenshot.png 0 10 200
```

The single output line reports result count, mean, p50, p95, minimum, and
maximum latency in milliseconds. Use the same build, image, model directory,
warmup count, and repeat count when comparing backends.
