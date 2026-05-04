cmake_minimum_required(VERSION 3.16)
project(nd2-cpp
    VERSION 1.0.0
    DESCRIPTION "Standalone C++ implementation of the Nikon ND2 read SDK C API"
    HOMEPAGE_URL "https://github.com/keejkrej/nd2-cpp"
    LANGUAGES CXX
)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

add_library(nd2-cpp
    src/Nd2ReadSdk.cpp
)

target_include_directories(nd2-cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(nd2-cpp PRIVATE GNR_ND2_SDK_EXPORTS)
else()
    target_compile_definitions(nd2-cpp PUBLIC LX_STATIC_LINKING)
endif()

set_target_properties(nd2-cpp PROPERTIES
    OUTPUT_NAME nd2readsdk
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

target_link_libraries(nd2-cpp PRIVATE ZLIB::ZLIB)

option(ND2CPP_BUILD_EXAMPLE "Build nd2info-style example" OFF)
if (ND2CPP_BUILD_EXAMPLE)
    add_executable(nd2info
        examples/nd2info.cpp
    )
    target_link_libraries(nd2info PRIVATE nd2-cpp)
    install(TARGETS nd2info
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(TARGETS nd2-cpp
    EXPORT nd2-cppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if (NOT CMAKE_SKIP_INSTALL_RULES)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

set(ND2CPP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}")

install(EXPORT nd2-cppTargets
    NAMESPACE nd2-cpp::
    DESTINATION ${ND2CPP_INSTALL_CMAKEDIR}
    FILE nd2-cppTargets.cmake
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nd2-cppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nd2-cppConfig.cmake"
    INSTALL_DESTINATION ${ND2CPP_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/nd2-cppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/nd2-cppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/nd2-cppConfigVersion.cmake"
    DESTINATION ${ND2CPP_INSTALL_CMAKEDIR}
)
