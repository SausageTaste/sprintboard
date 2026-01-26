set(target_name ${PROJECT_NAME})
set(private_header_dir ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(private_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/src)

file(GLOB_RECURSE private_header_files CONFIGURE_DEPENDS
    "${private_header_dir}/*.h"
    "${private_header_dir}/*.hpp"
)

file(GLOB_RECURSE private_source_files CONFIGURE_DEPENDS
    "${private_source_dir}/*.cpp"
)

add_executable(${target_name})
set_target_properties(${target_name} PROPERTIES FOLDER "${PROJECT_NAME}")
target_compile_features(${target_name} PRIVATE cxx_std_23)

target_sources(${target_name}
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS
        FILES
            ${public_header_files}
    PRIVATE
        ${private_header_files}
        ${private_source_files}
)

target_include_directories(${target_name} PRIVATE
    ${private_header_dir}
)

target_link_libraries(${target_name} PRIVATE
    httplib::httplib
    imageinfo::imageinfo
    nlohmann_json::nlohmann_json
)
