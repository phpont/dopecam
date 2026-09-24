cmake_minimum_required(VERSION 3.24)
project(DopeCam VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DopeCam Windows client only builds on Windows.")
endif()

configure_file(
    resources/DopeCam.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/DopeCam.rc
    @ONLY
)

add_executable(DopeCam WIN32
    ${CMAKE_CURRENT_BINARY_DIR}/DopeCam.rc
    src/main.cpp
    src/NetworkClient.cpp
    src/RtpReceiver.cpp
    src/MediaPipeline.cpp
    src/VirtualCameraPublisher.cpp
)

target_compile_features(DopeCam PRIVATE cxx_std_20)
target_compile_definitions(DopeCam PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DopeCam PRIVATE src)

target_link_libraries(DopeCam PRIVATE
    ws2_32
    iphlpapi
    comctl32
    ole32
    mfplat
    mf
    mfuuid
    wmcodecdspuuid
    d3d11
    dxgi
)

if(MSVC)
    target_compile_options(DopeCam PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_executable(DopeCamDirectShowCheck
    tools/DirectShowCheck.cpp
)

target_compile_features(DopeCamDirectShowCheck PRIVATE cxx_std_20)
target_compile_definitions(DopeCamDirectShowCheck PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DopeCamDirectShowCheck PRIVATE ole32 oleaut32 strmiids)

if(MSVC)
    target_compile_options(DopeCamDirectShowCheck PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
