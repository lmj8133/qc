cmake_minimum_required(VERSION 3.10)
project(helloworld C)
add_executable(hello hello.c)
install(TARGETS hello DESTINATION .)
