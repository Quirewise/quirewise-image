cmake_minimum_required(VERSION 3.21)
project(QuirewiseImage VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(PkgConfig REQUIRED)
set(PKG_CONFIG_ARGN "--static")

pkg_check_modules(QW_IMAGE_BACKENDS REQUIRED IMPORTED_TARGET GLOBAL
    libjpeg zlib libwebp libwebpdemux)
pkg_check_modules(QW_JP2_BACKENDS REQUIRED IMPORTED_TARGET GLOBAL libopenjp2)

set(QW_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/djvu"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aes")

add_library(quirewise_image_stb STATIC src/formats/image/stb_image_impl.c)
set_target_properties(quirewise_image_stb PROPERTIES EXPORT_NAME ImageSTB POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_stb PUBLIC c_std_11)
target_include_directories(quirewise_image_stb PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})

add_library(quirewise_image_jp2 STATIC src/formats/image/jp2_codec.c)
set_target_properties(quirewise_image_jp2 PROPERTIES EXPORT_NAME ImageJP2 POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_jp2 PUBLIC c_std_11)
target_compile_definitions(quirewise_image_jp2 PRIVATE OPJ_STATIC)
target_include_directories(quirewise_image_jp2 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})
target_link_libraries(quirewise_image_jp2 PRIVATE PkgConfig::QW_JP2_BACKENDS)

set(QW_IMAGE_SOURCES
    src/formats/image/optional_codec_stubs.c
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
    src/formats/image/image_name.c
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
    src/formats/image/webp_parser.c
    src/formats/image/xbm_codec.c
    src/formats/image/xcf_codec.c
    src/formats/image/xpm_codec.c)
add_library(quirewise_image_decode STATIC ${QW_IMAGE_SOURCES})
set_target_properties(quirewise_image_decode PROPERTIES EXPORT_NAME ImageDecode POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_image_decode PUBLIC c_std_11)
target_include_directories(quirewise_image_decode PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_INCLUDE_DIRS})
target_compile_definitions(quirewise_image_decode PRIVATE USE_ZLIB HAVE_STRDUP OPJ_STATIC)
target_link_libraries(quirewise_image_decode PRIVATE
    quirewise_image_jp2 quirewise_image_stb PkgConfig::QW_IMAGE_BACKENDS
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")

add_library(quirewise_image_folder STATIC
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

add_library(quirewise_image_processing STATIC
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

install(TARGETS quirewise_image_decode quirewise_image_jp2 quirewise_image_stb
                quirewise_image_folder quirewise_image_processing quirewise_image_bundle
    EXPORT QuirewiseImageTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES third_party/stb/stb_image.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
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

