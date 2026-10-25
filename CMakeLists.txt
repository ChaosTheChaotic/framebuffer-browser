cmake_minimum_required(VERSION 3.29)
project(fbbrowser LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network WebEngineWidgets Concurrent)
add_definitions (-DQT_DISABLE_DEPRECATED_BEFORE=0x060000)

find_library(FBB_ADBLOCK fbb_adblock REQUIRED)

set(PROJECT_SOURCES
    main.cc
    mainwindow.cc
    mainwindow.hh
    adblock.cc
    adblock.hh
    adblock.qrc
)

add_executable(fbbrowser
    ${PROJECT_SOURCES}
)

target_link_libraries(fbbrowser PRIVATE
    Qt6::Core
    Qt6::Gui
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
