cmake_minimum_required(VERSION 4.1)
project(manapiqt)

set(CMAKE_CXX_STANDARD 23)

find_package(manapihttp REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Quick Widgets)

add_executable(${PROJECT_NAME} main.cpp
        include/ManapiQt.hpp)

target_link_libraries(${PROJECT_NAME} PRIVATE manapihttp Qt6::Core Qt6::Quick Qt6::Widgets)

target_include_directories(${PROJECT_NAME} PRIVATE ./include)
