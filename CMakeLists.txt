
cmake_minimum_required(VERSION 3.15)

project(Server LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Common source files (compiled on both platforms)
set(SERVER_SOURCES
    src/main.cpp
    src/transport/notifier.cpp
    src/application/dispatcher.cpp
    src/application/workers/chatWorker.cpp
)

# Platform-specific source selection
if(WIN32)

    message(STATUS "Configuring Server for Windows")

    list(APPEND SERVER_SOURCES
        src/transport/windows/iocpTransport.cpp
    )

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    message(STATUS "Configuring Server for Linux")

    list(APPEND SERVER_SOURCES
        src/transport/linux/epollTransport.cpp
    )

else()

    message(FATAL_ERROR "Unsupported operating system")

endif()

# Create executable
add_executable(server ${SERVER_SOURCES})

# Platform-specific libraries
if(WIN32)

    target_link_libraries(server PRIVATE ws2_32)

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    find_package(Threads REQUIRED)
    target_link_libraries(server PRIVATE Threads::Threads)

endif()

# Statically link MinGW runtime libraries on Windows.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(server PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()
