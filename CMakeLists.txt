cmake_minimum_required(VERSION 3.10)
project(HFTCryptoTrading)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall")

find_package(Boost REQUIRED COMPONENTS system)
find_package(spdlog REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(hft_system 
    src/hft_system.cpp
)

target_include_directories(hft_system PRIVATE 
    ${Boost_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hft_system PRIVATE 
    ${Boost_LIBRARIES}
    spdlog::spdlog
    pthread
)

install(TARGETS hft_system DESTINATION bin)
