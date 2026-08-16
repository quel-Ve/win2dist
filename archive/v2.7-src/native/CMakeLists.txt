cmake_minimum_required(VERSION 3.16)
project(window2clear_native LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static link MinGW runtime to avoid missing DLL errors on other machines
if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -static")
endif()
# Route E: DDA + CPU blur (experimental)
add_executable(blur_backdrop src/d3d_backdrop.cpp)
target_link_libraries(blur_backdrop d3d11 dxgi dwmapi user32 gdi32)
# Acrylic Overlay: zero-flicker DWM-level blur (production)
add_executable(acrylic_overlay src/acrylic_overlay.cpp)
target_link_libraries(acrylic_overlay dwmapi user32 gdi32)
# Welcome Demo: effect cycling window (GUI, no console)
add_executable(welcome_demo src/welcome_demo.cpp)
target_link_libraries(welcome_demo dwmapi user32 gdi32)
set_target_properties(welcome_demo PROPERTIES WIN32_EXECUTABLE TRUE)
# Tray App: native C++ rewrite of tray_app.py (zero Python dependency)
add_executable(win2blur src/tray_app.cpp src/resource.rc)
target_link_libraries(win2blur dwmapi user32 shell32 comctl32)
set_target_properties(win2blur PROPERTIES WIN32_EXECUTABLE TRUE)
# Copy built child executables to source dir so resource.rc embeds fresh binaries
add_custom_target(copy_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:acrylic_overlay>
        ${CMAKE_SOURCE_DIR}/src/acrylic_overlay.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:welcome_demo>
        ${CMAKE_SOURCE_DIR}/src/welcome_demo.exe
    DEPENDS acrylic_overlay welcome_demo
    COMMENT "Copying child exes for resource embedding...")
add_dependencies(win2blur copy_resources)
