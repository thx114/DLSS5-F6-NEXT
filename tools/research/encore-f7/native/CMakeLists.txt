cmake_minimum_required(VERSION 3.20)
project(NrParameterStoreCpuFixture LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NrParameterStore requires a 64-bit C++ toolchain")
endif()

set(NGX_INCLUDE_DIR "" CACHE PATH "Directory containing existing public nvsdk_ngx_params.h and nvsdk_ngx_defs.h")
if(NOT NGX_INCLUDE_DIR)
    message(FATAL_ERROR "Set NGX_INCLUDE_DIR explicitly; no repository-global SDK path is assumed")
endif()
get_filename_component(NGX_INCLUDE_DIR "${NGX_INCLUDE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
foreach(_header IN ITEMS nvsdk_ngx_params.h nvsdk_ngx_defs.h)
    if(NOT EXISTS "${NGX_INCLUDE_DIR}/${_header}" OR IS_DIRECTORY "${NGX_INCLUDE_DIR}/${_header}")
        message(FATAL_ERROR "Missing public header: ${NGX_INCLUDE_DIR}/${_header}")
    endif()
endforeach()

# Bind precisely these eight inputs; edits trigger CMake's normal regeneration.
# The generated header lives only in the build tree, never in the source tree.
set(INPUT_STORE "${CMAKE_CURRENT_SOURCE_DIR}/NrParameterStore.h")
set(INPUT_CONSUMER_CPP "${CMAKE_CURRENT_SOURCE_DIR}/NrParameterStoreConsumer.cpp")
set(INPUT_CONSUMER_H "${CMAKE_CURRENT_SOURCE_DIR}/NrParameterStoreConsumer.h")
set(INPUT_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/NrParameterStoreTests.cpp")
set(INPUT_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(INPUT_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/NativeBuildInputs.h.in")
set(INPUT_NGX_PARAMS "${NGX_INCLUDE_DIR}/nvsdk_ngx_params.h")
set(INPUT_NGX_DEFS "${NGX_INCLUDE_DIR}/nvsdk_ngx_defs.h")
foreach(_input IN ITEMS STORE CONSUMER_CPP CONSUMER_H TESTS CMAKE TEMPLATE NGX_PARAMS NGX_DEFS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${INPUT_${_input}}")
    file(SHA256 "${INPUT_${_input}}" SHA256_${_input})
endforeach()
configure_file("${INPUT_TEMPLATE}" "${CMAKE_CURRENT_BINARY_DIR}/generated/NativeBuildInputs.h" @ONLY)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
add_executable(NrParameterStoreTests NrParameterStoreTests.cpp NrParameterStoreConsumer.cpp)
set_target_properties(NrParameterStoreTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    INTERPROCEDURAL_OPTIMIZATION FALSE
)
foreach(_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_config}" _config_upper)
    set_property(TARGET NrParameterStoreTests PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_config_upper} FALSE)
endforeach()
target_include_directories(NrParameterStoreTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(NrParameterStoreTests SYSTEM PRIVATE "${NGX_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(NrParameterStoreTests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(NrParameterStoreTests PRIVATE -Wall -Wextra -Werror)
endif()

# Public declarations only: no NGX/D3D libraries, initialization or loading.
enable_testing()
add_test(NAME NrParameterStoreTests COMMAND NrParameterStoreTests)
