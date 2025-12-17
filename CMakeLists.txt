cmake_minimum_required(VERSION 3.10)
project(LSFL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")

find_package(X11 REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(${PROJECT_NAME} src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${X11_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIRS}
    # ${CMAKE_SOURCE_DIR}/libs/FidelityFX-SDK-Linux/ffx-api/include
)
# target_link_directories(${PROJECT_NAME} PRIVATE
#     ${CMAKE_SOURCE_DIR}/libs/
# )

add_library(amd_fidelityfx_vk SHARED IMPORTED GLOBAL)

set_target_properties(amd_fidelityfx_vk PROPERTIES
  IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/libs/libamd_fidelityfx_vk.so"
)
target_include_directories(amd_fidelityfx_vk INTERFACE "${CMAKE_SOURCE_DIR}/libs/FidelityFX-SDK-Linux/ffx-api/include")

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${X11_LIBRARIES}
    ${X11_Xcomposite_LIB}
    Xtst
    # Xshape
    Xfixes
    Vulkan::Vulkan
    amd_fidelityfx_vk
)

