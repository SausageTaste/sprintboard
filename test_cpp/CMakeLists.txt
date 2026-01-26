add_executable(${PROJECT_NAME}_test_png png.cpp)
add_test(NAME ${PROJECT_NAME}_test_png COMMAND ${PROJECT_NAME}_test_png WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(${PROJECT_NAME}_test_png PROPERTIES FOLDER "${PROJECT_NAME}/test")
target_link_libraries(${PROJECT_NAME}_test_png sprintboard_img)
