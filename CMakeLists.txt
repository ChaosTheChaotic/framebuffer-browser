cmake_minimum_required(VERSION 3.29)
project(fbbrowser LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

add_compile_options(
	$<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:MSVC>:/O2>>
	$<$<CONFIG:Release>:$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3>>
)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Network WebEngineWidgets Concurrent)
add_definitions (-DQT_DISABLE_DEPRECATED_BEFORE=0x060000)

find_library(FBB_ADBLOCK fbb_adblock REQUIRED)

set(PROJECT_SOURCES
	main.cc
	mainwindow.cc
	mainwindow.hh
	adblock.cc
	adblock.hh
	adblock.qrc
	omnibox.cc
	omnibox.hh
)

add_executable(fbbrowser
	${PROJECT_SOURCES}
)

target_link_libraries(fbbrowser PRIVATE
	Qt6::Core
	Qt6::Gui
	Qt6::GuiPrivate
	Qt6::WebEngineWidgets
	Qt6::Network
	Qt6::Concurrent
	${FBB_ADBLOCK}
	gcc_s
	util
	rt
	pthread
	m
	dl
	c
)
