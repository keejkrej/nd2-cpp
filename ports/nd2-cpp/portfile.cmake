vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL "https://github.com/keejkrej/nd2-cpp.git"
    REF 0ced7c914858dac7f87b15a81b302c15b9098db5
    FETCH_REF main
)

set(ND2CPP_BUILD_EXAMPLE OFF)
if("tools" IN_LIST FEATURES)
    set(ND2CPP_BUILD_EXAMPLE ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DND2CPP_BUILD_EXAMPLE=${ND2CPP_BUILD_EXAMPLE}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME nd2-cpp CONFIG_PATH share/nd2-cpp)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES nd2info AUTO_CLEAN)
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
