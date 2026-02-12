cmake_minimum_required(VERSION 3.16)

if (NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()

project(sprintboard
    VERSION 0.1.0
    LANGUAGES CXX
)

option(SUNG_TEST "Build test exe projects" On)

set(submodules_dir "${CMAKE_CURRENT_SOURCE_DIR}/submodules")


find_package(absl CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(imageinfo CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)

add_subdirectory("${submodules_dir}/ImageRefinery")

add_subdirectory(cpp/src)

if (SUNG_TEST)
    add_subdirectory(cpp/tests)
endif()
