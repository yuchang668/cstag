cmake_minimum_required(VERSION 3.16)
project(cstag C)

set(CMAKE_C_STANDARD 99)

link_libraries(iconv sqlite3)

add_executable(cstag src/main.c src/task.c src/task.h src/dbop.c src/dbop.h src/path.c src/path.h)