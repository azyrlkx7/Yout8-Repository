cmake_minimum_required(VERSION 3.16)
project(Yout8 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core Network)

# O WIN32 impede que o terminal do Windows abra junto com o seu aplicativo
add_executable(Yout8 WIN32 Main.cpp Resources.rc)

target_link_libraries(Yout8 PRIVATE 
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::Network
)
