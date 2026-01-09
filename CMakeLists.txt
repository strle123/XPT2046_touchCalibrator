cmake_minimum_required(VERSION 3.10)
project(xpt2046_calibrator)

set(CMAKE_CXX_STANDARD 17)

add_executable(xpt2046_calibrator src/xpt2046_calibrator.cpp)
add_executable(xpt2046_uinputd src/xpt2046_uinputd.cpp)

# Ako budeš koristio udev ili druge libove, dodaj ih ovako:
# target_link_libraries(xpt2046_driver udev)

# SDL2 GUI test (optional, built if SDL2 is available)
find_package(SDL2 QUIET)
if(SDL2_FOUND)
	message(STATUS "Found SDL2: ${SDL2_INCLUDE_DIRS}")
	include_directories(${SDL2_INCLUDE_DIRS})
	add_executable(xpt_basic_gui_sdl2 src/basic_gui.cpp)
	target_link_libraries(xpt_basic_gui_sdl2 ${SDL2_LIBRARIES})
	add_executable(xpt_advanced_gui_sdl2 src/advanced_gui.cpp)
	target_link_libraries(xpt_advanced_gui_sdl2 ${SDL2_LIBRARIES})
	if(EXISTS "${CMAKE_SOURCE_DIR}/TestGUI/test_gui_sdl2.cpp")
		add_executable(xpt_test_gui_sdl2 TestGUI/test_gui_sdl2.cpp)
		target_link_libraries(xpt_test_gui_sdl2 ${SDL2_LIBRARIES})
	else()
		message(STATUS "TestGUI not present; xpt_test_gui_sdl2 will not be built.")
	endif()
else()
	message(STATUS "SDL2 not found; xpt_basic_gui_sdl2 will not be built.")
endif()