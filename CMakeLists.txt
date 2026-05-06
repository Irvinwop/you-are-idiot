cmake_minimum_required(VERSION 3.20)

project(you_are_idiot_native LANGUAGES CXX)

add_executable(you_are_idiot WIN32
    src/main.cpp
)

target_compile_features(you_are_idiot PRIVATE cxx_std_17)

target_link_libraries(you_are_idiot PRIVATE
    gdi32
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    shell32
    user32
)

add_custom_command(TARGET you_are_idiot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:you_are_idiot>/media"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/media/youare.mp4"
        "$<TARGET_FILE_DIR:you_are_idiot>/media/youare.mp4"
)

if(MSVC)
    target_compile_options(you_are_idiot PRIVATE /W4 /permissive-)
else()
    target_compile_options(you_are_idiot PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    target_link_options(you_are_idiot PRIVATE -municode)
endif()
