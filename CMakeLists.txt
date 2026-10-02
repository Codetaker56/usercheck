cmake_minimum_required(VERSION 3.16)
project(ringer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ringer ringer.cpp)

find_package(Threads REQUIRED)
target_link_libraries(ringer PRIVATE Threads::Threads)

if(WIN32)
  target_link_libraries(ringer PRIVATE winhttp winmm)
else()
  find_package(CURL REQUIRED)
  target_link_libraries(ringer PRIVATE CURL::libcurl)
endif()

if(MSVC)
  target_compile_options(ringer PRIVATE /utf-8)
else()
  target_compile_options(ringer PRIVATE -Wall -Wextra)
endif()
