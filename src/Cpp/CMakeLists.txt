if(BUILD_DEBUG_DEMO)
    add_executable(debug_demo main.cpp)

    if(MSVC)
        target_compile_options(debug_demo PRIVATE "/WX-")
    else()
        target_compile_options(debug_demo PRIVATE "-Wno-error")
    endif()

    target_link_libraries(debug_demo MaaCore)

    add_dependencies(debug_demo MaaCore)

    # 为 debug_demo 创建资源目录链接
    create_resource_link(debug_demo "$<TARGET_FILE_DIR:debug_demo>")

    # install(TARGETS debug_demo ${MaaCore_install_flatten_args})
endif(BUILD_DEBUG_DEMO)

if(BUILD_SMOKE_TEST)
    add_executable(smoke_test main.cpp)

    if(MSVC)
        target_compile_options(smoke_test PRIVATE "/WX-")
    else()
        target_compile_options(smoke_test PRIVATE "-Wno-error")
    endif()

    target_compile_definitions(smoke_test PRIVATE SMOKE_TESTING)
    target_link_libraries(smoke_test MaaCore)

    add_dependencies(smoke_test MaaCore)
    install(TARGETS smoke_test ${MaaCore_install_flatten_args})
endif(BUILD_SMOKE_TEST)

if(BUILD_OCR_BENCHMARK)
    if(NOT LINUX)
        message(FATAL_ERROR "BUILD_OCR_BENCHMARK is supported only on Linux")
    endif()
    if(NOT MAA_LINUX_OCR_BACKEND STREQUAL "ncnn")
        message(FATAL_ERROR "BUILD_OCR_BENCHMARK requires MAA_LINUX_OCR_BACKEND=ncnn")
    endif()

    add_executable(ocr_ncnn_benchmark ${PROJECT_SOURCE_DIR}/tools/OcrNcnnBenchmark/main.cpp)
    target_compile_features(ocr_ncnn_benchmark PRIVATE cxx_std_20)
    target_include_directories(ocr_ncnn_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src/MaaCore)
    target_link_libraries(ocr_ncnn_benchmark PRIVATE MaaCore)
endif()
