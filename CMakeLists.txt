cmake_minimum_required(VERSION 3.10)
project(LSFL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)

find_package(X11 REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(${PROJECT_NAME} src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${X11_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/libs/FidelityFX-SDK-Linux/ffx-api/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${X11_LIBRARIES}
    ${X11_Xcomposite_LIB}
    Xtst
    # Xshape
    Xfixes
    Vulkan::Vulkan
    # ${CMAKE_SOURCE_DIR}/libs/amd_fidelityfx_vk
)
