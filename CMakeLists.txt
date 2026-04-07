cmake_minimum_required(VERSION 3.10)
project(allocator)

file(GLOB SOURCES "*.cpp")
add_executable(code ${SOURCES})
