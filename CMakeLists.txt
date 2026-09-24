cmake_minimum_required(VERSION 3.16)
project(usercheck CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(usercheck usercheck.cpp)

if(WIN32)
  target_link_libraries(usercheck PRIVATE winhttp)
else()
  find_package(CURL REQUIRED)
  target_link_libraries(usercheck PRIVATE CURL::libcurl)
endif()

if(MSVC)
  target_compile_options(usercheck PRIVATE /utf-8)
else()
  target_compile_options(usercheck PRIVATE -Wall -Wextra)
endif()
