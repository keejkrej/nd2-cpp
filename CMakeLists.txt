cmake_minimum_required(VERSION 3.16)
project(nd2-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)

add_library(nd2-cpp SHARED
    src/Nd2ReadSdk.cpp
)

target_include_directories(nd2-cpp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(nd2-cpp
    PRIVATE
        ND2CPP_BUILDING_LIB
)

set_target_properties(nd2-cpp PROPERTIES
    OUTPUT_NAME nd2readsdk
    VERSION 1.0.0
    SOVERSION 1
)

target_link_libraries(nd2-cpp PRIVATE ZLIB::ZLIB)

option(ND2CPP_BUILD_EXAMPLE "Build nd2info-style example" OFF)
if (ND2CPP_BUILD_EXAMPLE)
    add_executable(nd2info
        examples/nd2info.cpp
    )
    target_include_directories(nd2info PRIVATE include)
    target_link_libraries(nd2info PRIVATE nd2-cpp)
endif()

install(TARGETS nd2-cpp
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if (NOT CMAKE_SKIP_INSTALL_RULES)
    install(DIRECTORY include/ DESTINATION include)
endif()
