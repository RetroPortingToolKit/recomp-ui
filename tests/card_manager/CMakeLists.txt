cmake_minimum_required(VERSION 3.20)
project(recomp_card_manager_ui_tests LANGUAGES CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(RUI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT WIN32)
    return() # the current writable host service is Windows-only
endif()
add_executable(card_manager_ui_test test_ui.cpp
    "${RUI_ROOT}/src/third_party/imgui/imgui.cpp"
    "${RUI_ROOT}/src/third_party/imgui/imgui_draw.cpp"
    "${RUI_ROOT}/src/third_party/imgui/imgui_tables.cpp"
    "${RUI_ROOT}/src/third_party/imgui/imgui_widgets.cpp")
target_include_directories(card_manager_ui_test PRIVATE "${RUI_ROOT}/src" "${RUI_ROOT}/src/third_party/imgui" "${RUI_ROOT}/src/common/backends/imgui")
target_link_libraries(card_manager_ui_test PRIVATE comdlg32)
add_test(NAME card_manager_ui COMMAND card_manager_ui_test)

add_executable(card_manager_picker_test test_picker.cpp)
target_include_directories(card_manager_picker_test PRIVATE "${RUI_ROOT}/src/common/backends/imgui")
target_link_libraries(card_manager_picker_test PRIVATE comdlg32)
add_test(NAME card_manager_picker COMMAND card_manager_picker_test)
