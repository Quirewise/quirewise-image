cmake_minimum_required(VERSION 3.21)
project(QuirewiseImage VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(QuirewiseCompression CONFIG REQUIRED)
find_package(QuirewiseRuntime CONFIG REQUIRED)

set(QW_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/djvu"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aes")

set(QW_JP2_SOURCES
    src/formats/image/jp2_codec.c
    src/formats/image/jp2_mq.c
    src/formats/image/jp2_packet_header.c
    src/formats/image/jp2_packet_body.c
    src/formats/image/jp2_packet_progression.c
    src/formats/image/jp2_packed_headers.c
    src/formats/image/jp2_parser.c
    src/formats/image/jp2_dwt.c
    src/formats/image/jp2_tier1_decoder.c
    src/formats/image/jp2_tier1_model.c)
add_library(quirewise_image_jp2 ${QW_LIBRARY_TYPE} ${QW_JP2_SOURCES})
set_target_properties(quirewise_image_jp2 PROPERTIES EXPORT_NAME ImageJP2 POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_jp2 PUBLIC c_std_11)
target_include_directories(quirewise_image_jp2 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})

option(QW_JP2_BUILD_COVERAGE_FUZZER
    "Build the GCC coverage-guided JP2/J2K mutation fuzzer" OFF)
if(QW_JP2_BUILD_COVERAGE_FUZZER)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "QW_JP2_BUILD_COVERAGE_FUZZER requires GCC trace-pc instrumentation")
    endif()
    add_library(quirewise_image_jp2_fuzz_instrumented STATIC ${QW_JP2_SOURCES})
    set_target_properties(quirewise_image_jp2_fuzz_instrumented PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_features(quirewise_image_jp2_fuzz_instrumented PUBLIC c_std_11)
    target_include_directories(quirewise_image_jp2_fuzz_instrumented PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE ${QW_INCLUDE_DIRS})
    target_compile_options(quirewise_image_jp2_fuzz_instrumented PRIVATE
        -fsanitize-coverage=trace-pc)
    add_executable(quirewise_image_jp2_coverage_fuzzer
        tests/jp2_coverage_fuzzer.c)
    target_compile_features(quirewise_image_jp2_coverage_fuzzer PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_coverage_fuzzer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_coverage_fuzzer PRIVATE
        quirewise_image_jp2_fuzz_instrumented)
endif()

set(QW_IMAGE_SOURCES
    src/formats/image/optional_codec_stubs.c
    src/formats/image/stb_image_impl.c
    src/formats/image/apng_probe.c
    src/formats/image/bmp_decoder.c
    src/formats/image/dcm_codec.c
    src/formats/image/dds_codec.c
    src/formats/image/exr_codec.c
    src/formats/image/gif_codec.c
    src/formats/image/hdr_codec.c
    src/formats/image/heif_parser.c
    src/formats/image/icns_codec.c
    src/formats/image/icon_codec.c
    src/formats/image/image_api.c
    src/formats/image/image_decode.c
    src/formats/image/jpeg_parser.c
    src/formats/image/miff_codec.c
    src/formats/image/mng_codec.c
    src/formats/image/pcx_codec.c
    src/formats/image/pfm_codec.c
    src/formats/image/pic_codec.c
    src/formats/image/png_decoder.c
    src/formats/image/pnm_codec.c
    src/formats/image/psb_codec.c
    src/formats/image/qoi_codec.c
    src/formats/image/ras_codec.c
    src/formats/image/raw_parser.c
    src/formats/image/sgi_codec.c
    src/formats/image/tga_codec.c
    src/formats/image/tiff_decode.c
    src/formats/image/wbmp_codec.c
    src/formats/image/webp_lossless.c
    src/formats/image/webp_parser.c
    src/formats/image/webp_vp8.c
    src/formats/image/webp_vp8_reconstruct.c
    src/formats/image/xbm_codec.c
    src/formats/image/xcf_codec.c
    src/formats/image/xpm_codec.c)
if(QW_JP2_BUILD_COVERAGE_FUZZER)
    add_library(quirewise_image_decode_fuzz_instrumented STATIC
        ${QW_IMAGE_SOURCES})
    set_target_properties(quirewise_image_decode_fuzz_instrumented PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_features(quirewise_image_decode_fuzz_instrumented PUBLIC
        c_std_11)
    target_include_directories(quirewise_image_decode_fuzz_instrumented PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE ${QW_INCLUDE_DIRS})
    target_compile_definitions(quirewise_image_decode_fuzz_instrumented PRIVATE
        HAVE_STRDUP)
    target_compile_options(quirewise_image_decode_fuzz_instrumented PRIVATE
        -fsanitize-coverage=trace-pc)
    target_link_libraries(quirewise_image_decode_fuzz_instrumented PRIVATE
        quirewise_image_jp2_fuzz_instrumented
        Quirewise::Compression
        "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")

    add_executable(quirewise_image_jp2_public_dispatch_fuzzer
        tests/jp2_coverage_fuzzer.c)
    target_compile_features(quirewise_image_jp2_public_dispatch_fuzzer PRIVATE
        c_std_11)
    target_include_directories(quirewise_image_jp2_public_dispatch_fuzzer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_compile_definitions(quirewise_image_jp2_public_dispatch_fuzzer PRIVATE
        QW_JP2_FUZZ_PUBLIC_DISPATCH=1)
    target_link_libraries(quirewise_image_jp2_public_dispatch_fuzzer PRIVATE
        quirewise_image_decode_fuzz_instrumented)
endif()
add_library(quirewise_image_decode ${QW_LIBRARY_TYPE} ${QW_IMAGE_SOURCES})
set_target_properties(quirewise_image_decode PROPERTIES EXPORT_NAME ImageDecode POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_decode PUBLIC c_std_11)
target_include_directories(quirewise_image_decode PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})
target_compile_definitions(quirewise_image_decode PRIVATE HAVE_STRDUP)
target_link_libraries(quirewise_image_decode PRIVATE
    quirewise_image_jp2 Quirewise::Compression
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")

add_library(quirewise_image_folder ${QW_LIBRARY_TYPE}
    src/formats/image/image_folder_parser.c
    src/formats/image/image_name.c
    src/core/platform_unicode.c
    src/core/natural_sort.c)
set_target_properties(quirewise_image_folder PROPERTIES EXPORT_NAME ImageFolder POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_folder PUBLIC c_std_11)
target_include_directories(quirewise_image_folder PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})
target_link_libraries(quirewise_image_folder PRIVATE
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")

add_library(quirewise_image_processing ${QW_LIBRARY_TYPE}
    src/formats/image/image_processing_api.c
    src/formats/image/scan_advanced_geometry.c
    src/formats/image/scan_geometry.c
    src/formats/image/scan_tone.c)
set_target_properties(quirewise_image_processing PROPERTIES EXPORT_NAME ImageProcessing POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_processing PUBLIC c_std_11)
target_include_directories(quirewise_image_processing PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})
target_link_libraries(quirewise_image_processing PRIVATE
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")

add_library(quirewise_image_bundle INTERFACE)
add_library(Quirewise::Image ALIAS quirewise_image_bundle)
set_target_properties(quirewise_image_bundle PROPERTIES EXPORT_NAME Image)
target_link_libraries(quirewise_image_bundle INTERFACE
    quirewise_image_decode quirewise_image_folder quirewise_image_processing)

install(TARGETS quirewise_image_decode quirewise_image_jp2
                quirewise_image_folder quirewise_image_processing quirewise_image_bundle
    EXPORT QuirewiseImageTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseImageTargets
    FILE QuirewiseImageTargets.cmake
    NAMESPACE Quirewise::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseImage")
configure_package_config_file(cmake/QuirewiseImageConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseImageConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseImage")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseImageConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseImageConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseImageConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseImage")

if(BUILD_TESTING)
    add_executable(quirewise_image_jp2_packet_header_test
        tests/jp2_packet_header_test.c)
    target_compile_features(quirewise_image_jp2_packet_header_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_packet_header_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_packet_header_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_packet_header
        COMMAND quirewise_image_jp2_packet_header_test)

    add_executable(quirewise_image_jp2_packed_headers_test
        tests/jp2_packed_headers_test.c)
    target_compile_features(quirewise_image_jp2_packed_headers_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_packed_headers_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_packed_headers_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_packed_headers
        COMMAND quirewise_image_jp2_packed_headers_test)

    add_executable(quirewise_image_jp2_packet_body_test
        tests/jp2_packet_body_test.c)
    target_compile_features(quirewise_image_jp2_packet_body_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_packet_body_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_packet_body_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_packet_body
        COMMAND quirewise_image_jp2_packet_body_test)

    add_executable(quirewise_image_jp2_packet_progression_test
        tests/jp2_packet_progression_test.c)
    target_compile_features(quirewise_image_jp2_packet_progression_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_packet_progression_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_packet_progression_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_packet_progression
        COMMAND quirewise_image_jp2_packet_progression_test)

    add_executable(quirewise_image_jp2_tier1_decoder_test
        tests/jp2_tier1_decoder_test.c)
    target_compile_features(quirewise_image_jp2_tier1_decoder_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_tier1_decoder_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_tier1_decoder_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_tier1_decoder
        COMMAND quirewise_image_jp2_tier1_decoder_test)

    add_executable(quirewise_image_jp2_tier1_model_test
        tests/jp2_tier1_model_test.c)
    target_compile_features(quirewise_image_jp2_tier1_model_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_tier1_model_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_tier1_model_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_tier1_model
        COMMAND quirewise_image_jp2_tier1_model_test)

    add_executable(quirewise_image_jp2_profile_matrix_test
        tests/jp2_profile_matrix_test.c)
    target_compile_features(quirewise_image_jp2_profile_matrix_test PRIVATE c_std_11)
    target_compile_definitions(quirewise_image_jp2_profile_matrix_test PRIVATE
        QUIREWISE_JP2_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/jp2")
    target_link_libraries(quirewise_image_jp2_profile_matrix_test PRIVATE
        quirewise_image_bundle)
    add_test(NAME quirewise_image_jp2_profile_matrix
        COMMAND quirewise_image_jp2_profile_matrix_test)

    add_executable(quirewise_image_jp2_dwt_test
        tests/jp2_dwt_test.c)
    target_compile_features(quirewise_image_jp2_dwt_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_dwt_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_dwt_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_dwt
        COMMAND quirewise_image_jp2_dwt_test)

    add_executable(quirewise_image_jp2_mq_test
        tests/jp2_mq_test.c)
    target_compile_features(quirewise_image_jp2_mq_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_jp2_mq_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_link_libraries(quirewise_image_jp2_mq_test PRIVATE
        quirewise_image_jp2)
    add_test(NAME quirewise_image_jp2_mq
        COMMAND quirewise_image_jp2_mq_test)


    add_executable(quirewise_image_compression_migration_test
        tests/image_compression_migration_test.c)
    target_compile_features(quirewise_image_compression_migration_test PRIVATE c_std_11)
    target_compile_definitions(quirewise_image_compression_migration_test PRIVATE
        QUIREWISE_TIFF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/tiff")
    target_link_libraries(quirewise_image_compression_migration_test PRIVATE
        quirewise_image_decode Quirewise::Compression)
    add_test(NAME quirewise_image_compression_migration
        COMMAND quirewise_image_compression_migration_test)

    add_executable(quirewise_image_fuzz_smoke tests/image_fuzz_smoke.c)
    target_compile_features(quirewise_image_fuzz_smoke PRIVATE c_std_11)
    target_include_directories(quirewise_image_fuzz_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(quirewise_image_fuzz_smoke PRIVATE quirewise_image_decode quirewise_image_jp2)
    add_test(NAME quirewise_image_fuzz_smoke
        COMMAND quirewise_image_fuzz_smoke)

    add_executable(quirewise_image_webp_animation_test
        tests/webp_animation_test.c)
    target_compile_features(quirewise_image_webp_animation_test PRIVATE c_std_11)
    target_include_directories(quirewise_image_webp_animation_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image")
    target_compile_definitions(quirewise_image_webp_animation_test PRIVATE
        QUIREWISE_WEBP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/webp")
    target_link_libraries(quirewise_image_webp_animation_test PRIVATE
        quirewise_image_decode)
    add_test(NAME quirewise_image_webp_animation
        COMMAND quirewise_image_webp_animation_test)
endif()

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_image_jp2
        quirewise_image_decode
        quirewise_image_folder
        quirewise_image_processing
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
target_link_libraries(quirewise_image_jp2 PUBLIC Quirewise::Runtime)
target_link_libraries(quirewise_image_decode PUBLIC Quirewise::Runtime)
target_link_libraries(quirewise_image_folder PUBLIC Quirewise::Runtime)
target_link_libraries(quirewise_image_processing PUBLIC Quirewise::Runtime)
