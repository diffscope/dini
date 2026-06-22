cmake_minimum_required(VERSION 3.17)

project(dini VERSION 0.0.1.0 LANGUAGES C CXX)

# ----------------------------------
# Build Options
# ----------------------------------
option(DINI_BUILD_STATIC "Build static library" OFF)
option(DINI_INSTALL "Install library" ON)
option(DINI_BUILD_TESTS "Build unit tests" OFF)
option(DINI_BUILD_BENCHMARKS "Build benchmark targets" OFF)

# ----------------------------------
# CMake Settings
# ----------------------------------
if (NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif ()

if (NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif ()

if (NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif ()

if (NOT DEFINED CMAKE_BUILD_SHARE_DIR)
    set(CMAKE_BUILD_SHARE_DIR ${CMAKE_BINARY_DIR}/share)
endif ()

if (MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /manifest:no")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    if (NOT DEFINED CMAKE_DEBUG_POSTFIX)
        set(CMAKE_DEBUG_POSTFIX "d")
    endif ()
endif ()

if (DINI_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
endif ()

# ----------------------------------
# Project Variables
# ----------------------------------
set(DINI_VERSION ${PROJECT_VERSION})
set(DINI_INSTALL_NAME ${PROJECT_NAME})
set(DINI_TARGET ${PROJECT_NAME})

set(CURRENT_YEAR)
string(TIMESTAMP CURRENT_YEAR "%Y")

if (WIN32)
    include(cmake/winrc.cmake)
endif ()

find_package(stdcorelib CONFIG REQUIRED)

# ----------------------------------
# Main Project
# ----------------------------------
set(CMAKE_POSITION_INDEPENDENT_CODE on)

if (DINI_BUILD_STATIC)
    add_library(${DINI_TARGET} STATIC)
    target_compile_definitions(${DINI_TARGET} PUBLIC DINI_STATIC)
else ()
    add_library(${DINI_TARGET} SHARED)
endif ()

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${DINI_TARGET})

file(GLOB_RECURSE _src include/dini/*.h src/*.cpp src/*.h)
target_sources(${DINI_TARGET} PRIVATE ${_src})

target_compile_features(${DINI_TARGET} PRIVATE cxx_std_20 cxx_std_17)
target_compile_definitions(${DINI_TARGET} PRIVATE DINI_LIBRARY)

target_include_directories(${DINI_TARGET} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_include_directories(${DINI_TARGET} PRIVATE include/dini)

target_link_libraries(${DINI_TARGET} PUBLIC stdcorelib::stdcorelib)

if (WIN32)
    add_winrc(${DINI_TARGET}
        NAME "${PROJECT_NAME}"
        DESCRIPTION "${PROJECT_NAME}"
        COPYRIGHT "Copyright ${CURRENT_YEAR}"
    )
endif ()

if (DINI_BUILD_TESTS OR DINI_BUILD_BENCHMARKS)
    enable_testing()
endif ()

if (DINI_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

if (DINI_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

if (DINI_INSTALL)
    target_include_directories(${DINI_TARGET} PUBLIC
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )

    install(TARGETS ${DINI_TARGET}
        EXPORT ${DINI_INSTALL_NAME}Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" OPTIONAL
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" OPTIONAL
    )

    set(_install_dir ${CMAKE_INSTALL_LIBDIR}/cmake/${DINI_INSTALL_NAME})

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/${DINI_INSTALL_NAME}ConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_LIST_DIR}/${DINI_INSTALL_NAME}Config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/${DINI_INSTALL_NAME}Config.cmake"
        INSTALL_DESTINATION ${_install_dir}
        NO_CHECK_REQUIRED_COMPONENTS_MACRO
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${DINI_INSTALL_NAME}Config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${DINI_INSTALL_NAME}ConfigVersion.cmake"
        DESTINATION ${_install_dir}
    )

    install(EXPORT ${DINI_INSTALL_NAME}Targets
        FILE "${DINI_INSTALL_NAME}Targets.cmake"
        NAMESPACE dini::
        DESTINATION ${_install_dir}
    )

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
    )
endif ()
