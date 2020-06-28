cmake_minimum_required(VERSION 2.8.3)
project(LWC_Web_Server)

add_compile_options(-std=c++11)

set(SRC 
    ${PROJECT_SOURCE_DIR}
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

add_executable(lwcWebServer main.cpp http_conn.cpp)