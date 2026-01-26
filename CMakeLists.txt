cmake_minimum_required(VERSION 3.16)

if (NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()

project(sprintboard
    VERSION 0.1.0
    LANGUAGES CXX
)


option(SUNG_TEST "Build test exe projects" On)


find_package(httplib CONFIG REQUIRED)
find_package(imageinfo CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PNG REQUIRED)

add_subdirectory(src_cpp)

if (SUNG_TEST)
    add_subdirectory(test_cpp)
endif()
