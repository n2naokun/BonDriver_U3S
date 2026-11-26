cmake_minimum_required(VERSION 3.21)
project(U3S_BonDriver LANGUAGES CXX)

if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

add_library(u3s_core STATIC src/u3s.cpp src/ts.cpp)
target_compile_features(u3s_core PUBLIC cxx_std_20)
target_compile_definitions(u3s_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(u3s_core PUBLIC src)
target_link_libraries(u3s_core PUBLIC bcrypt)

add_library(u3s_winusb STATIC src/winusb_device.cpp)
target_link_libraries(u3s_winusb PUBLIC u3s_core winusb setupapi advapi32 ole32)

if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "BonDriver_U3S requires MSVC targeting x64.")
endif()

add_library(u3s_host_ipc STATIC src/host_ipc.cpp)
target_link_libraries(u3s_host_ipc PUBLIC u3s_winusb wtsapi32)

add_executable(U3SHost WIN32 src/host_main.cpp)
target_link_libraries(U3SHost PRIVATE u3s_host_ipc)
foreach(file U3SHost.ini InstallU3SHost.cmd UninstallU3SHost.cmd Create-ProxyExD-HostSid.cmd)
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:U3SHost>/${file}" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
endforeach()

add_library(BonDriver_U3S SHARED src/bondriver.cpp src/channel_config.cpp)
target_link_libraries(BonDriver_U3S PRIVATE u3s_host_ipc)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:BonDriver_U3S>/BonDriver_U3S.ini"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/BonDriver_U3S.ini")

add_library(u3s-winscard SHARED src/winscard.cpp src/winscard.def)
target_link_libraries(u3s-winscard PRIVATE u3s_host_ipc)
set_target_properties(u3s-winscard PROPERTIES
    OUTPUT_NAME winscard
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/card-reader"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/card-reader")
