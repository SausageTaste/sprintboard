add_executable(${PROJECT_NAME}_test_filesys filesys.cpp)
add_test(NAME ${PROJECT_NAME}_test_filesys COMMAND ${PROJECT_NAME}_test_filesys)
set_target_properties(${PROJECT_NAME}_test_filesys PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_link_libraries(${PROJECT_NAME}_test_filesys sprintboard_aux)

add_executable(${PROJECT_NAME}_test_server_configs server_configs.cpp)
add_test(NAME ${PROJECT_NAME}_test_server_configs COMMAND ${PROJECT_NAME}_test_server_configs)
set_target_properties(${PROJECT_NAME}_test_server_configs PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_link_libraries(${PROJECT_NAME}_test_server_configs sprintboard_aux)

add_executable(
    ${PROJECT_NAME}_test_source_image
    source_image.cpp
    ../src/server/src/source_image.cpp
)
add_test(
    NAME ${PROJECT_NAME}_test_source_image
    COMMAND ${PROJECT_NAME}_test_source_image
)
set_target_properties(
    ${PROJECT_NAME}_test_source_image PROPERTIES FOLDER "${PROJECT_NAME}/test"
)
target_include_directories(
    ${PROJECT_NAME}_test_source_image PRIVATE ../src/server/src
)
target_link_libraries(${PROJECT_NAME}_test_source_image sprintboard_aux)

add_executable(${PROJECT_NAME}_test_png png.cpp)
add_test(NAME ${PROJECT_NAME}_test_png COMMAND ${PROJECT_NAME}_test_png WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(${PROJECT_NAME}_test_png PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_link_libraries(${PROJECT_NAME}_test_png sprintboard_img)

add_executable(${PROJECT_NAME}_test_avif avif.cpp)
add_test(NAME ${PROJECT_NAME}_test_avif COMMAND ${PROJECT_NAME}_test_avif WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(${PROJECT_NAME}_test_avif PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_link_libraries(${PROJECT_NAME}_test_avif sprintboard_img)

add_executable(
    ${PROJECT_NAME}_test_img_list
    img_list.cpp
    ../src/server/src/response/img_list.cpp
)
add_test(NAME ${PROJECT_NAME}_test_img_list COMMAND ${PROJECT_NAME}_test_img_list)
set_target_properties(${PROJECT_NAME}_test_img_list PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_include_directories(
    ${PROJECT_NAME}_test_img_list PRIVATE ../src/server/src
)
target_link_libraries(${PROJECT_NAME}_test_img_list sprintboard_img)

add_executable(
    ${PROJECT_NAME}_test_image_index
    image_index.cpp
    ../src/server/src/index/image_index.cpp
    ../src/server/src/response/img_list.cpp
)
add_test(NAME ${PROJECT_NAME}_test_image_index COMMAND ${PROJECT_NAME}_test_image_index)
set_target_properties(${PROJECT_NAME}_test_image_index PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_include_directories(
    ${PROJECT_NAME}_test_image_index PRIVATE ../src/server/src
)
target_link_libraries(
    ${PROJECT_NAME}_test_image_index
    sprintboard_img
    TBB::tbb
    unofficial::sqlite3::sqlite3
)
