# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

project(OpenVehicleFramework VERSION 0.0.1 LANGUAGES C CXX)

option(OVF_BUILD_TESTS "Build OpenVehicleFramework tests" ON)
option(OVF_BUILD_INPROC_TRANSPORT "Build the reference in-process transport" ON)

add_library(ovf_exec
    exec/src/application.cpp
    exec/src/backend_binding.cpp
    exec/src/dinit_lifecycle.cpp
    exec/src/engine.cpp
    exec/src/model.cpp
    exec/src/planner.cpp
    exec/src/scheduler.cpp
)
add_library(ovf::exec ALIAS ovf_exec)

target_compile_features(ovf_exec PUBLIC cxx_std_20)
target_include_directories(ovf_exec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/exec/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(ovf_exec PROPERTIES
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_library(ovf_com
    com/src/runtime.cpp
    com/src/provider_binding.cpp
)
add_library(ovf::com ALIAS ovf_com)

target_compile_features(ovf_com PUBLIC cxx_std_20)
target_include_directories(ovf_com
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/com/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(ovf_com PROPERTIES
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(OVF_BUILD_INPROC_TRANSPORT)
    add_library(ovf_com_transport_inproc
        com/transports/inproc/src/inproc_transport.cpp
    )
    add_library(ovf::com_transport_inproc ALIAS ovf_com_transport_inproc)
    target_compile_features(ovf_com_transport_inproc PRIVATE cxx_std_20)
    target_include_directories(ovf_com_transport_inproc
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/com/transports/inproc/include>
            ${CMAKE_CURRENT_SOURCE_DIR}/com/include
    )
    set_target_properties(ovf_com_transport_inproc PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(OVF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(com/tests)
endif()
