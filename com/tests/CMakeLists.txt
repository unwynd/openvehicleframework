# SPDX-License-Identifier: Apache-2.0

find_package(GTest CONFIG QUIET)
if(GTest_FOUND)
    add_executable(ovf_com_runtime_test runtime_test.cpp)
    target_compile_features(ovf_com_runtime_test PRIVATE cxx_std_20)
    target_link_libraries(ovf_com_runtime_test PRIVATE
        ovf::com ovf::com_transport_inproc GTest::gtest_main)
    add_test(NAME ovf_com_runtime_test COMMAND ovf_com_runtime_test)
else()
    message(STATUS "GoogleTest not found; C++ unit tests are provided by Bazel")
endif()

add_executable(ovf_com_c_abi_test c_abi_test.c)
target_include_directories(ovf_com_c_abi_test PRIVATE ${PROJECT_SOURCE_DIR}/com/include)
add_test(NAME ovf_com_c_abi_test COMMAND ovf_com_c_abi_test)

add_executable(ovf_com_provider_conformance_test provider_conformance_test.cpp)
target_link_libraries(ovf_com_provider_conformance_test PRIVATE ovf::com_transport_inproc)
target_compile_features(ovf_com_provider_conformance_test PRIVATE cxx_std_20)
add_test(NAME ovf_com_provider_conformance_test COMMAND ovf_com_provider_conformance_test)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OVF_IR_TOOL tools.validate_ir)
set(OVF_IR_EXAMPLES ${CMAKE_SOURCE_DIR}/com/model/examples)
add_test(NAME ovf_com_d1_ir_valid
    COMMAND ${Python3_EXECUTABLE} -m ${OVF_IR_TOOL}
            ${OVF_IR_EXAMPLES}/radar.ovf-ir.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ovf_com_d1_ir_additive_compatible
    COMMAND ${Python3_EXECUTABLE} -m ${OVF_IR_TOOL}
            ${OVF_IR_EXAMPLES}/radar-additive.ovf-ir.json
            --baseline ${OVF_IR_EXAMPLES}/radar.ovf-ir.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ovf_com_d1_ir_breaking_rejected
    COMMAND ${Python3_EXECUTABLE} -m ${OVF_IR_TOOL}
            ${OVF_IR_EXAMPLES}/radar-breaking.ovf-ir.json
            --baseline ${OVF_IR_EXAMPLES}/radar.ovf-ir.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ovf_com_d1_ir_breaking_rejected PROPERTIES WILL_FAIL TRUE)

find_program(OVF_RUSTC rustc)
if(OVF_RUSTC)
    set(OVF_RUST_PROVIDER_LIB ${CMAKE_CURRENT_BINARY_DIR}/libovf_rust_provider.a)
    add_custom_command(OUTPUT ${OVF_RUST_PROVIDER_LIB}
        COMMAND ${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=15.5
                ${OVF_RUSTC} --edition=2021 --crate-type=staticlib
                ${CMAKE_CURRENT_SOURCE_DIR}/rust_provider/lib.rs
                -o ${OVF_RUST_PROVIDER_LIB}
        DEPENDS rust_provider/lib.rs VERBATIM)
    add_custom_target(ovf_com_rust_provider ALL DEPENDS ${OVF_RUST_PROVIDER_LIB})
    add_executable(ovf_com_rust_provider_interop_test rust_provider_interop_test.c)
    add_dependencies(ovf_com_rust_provider_interop_test ovf_com_rust_provider)
    target_include_directories(ovf_com_rust_provider_interop_test PRIVATE
        ${CMAKE_SOURCE_DIR}/com/include)
    target_link_libraries(ovf_com_rust_provider_interop_test PRIVATE ${OVF_RUST_PROVIDER_LIB})
    add_test(NAME ovf_com_rust_provider_interop_test COMMAND ovf_com_rust_provider_interop_test)
endif()

set(OVF_RADAR_IR ${CMAKE_SOURCE_DIR}/com/model/examples/radar.ovf-ir.json)
set(OVF_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

set(OVF_DEPLOYMENT_TOOL tools.validate_deployment)
set(OVF_DEPLOYMENT_TOOL_SOURCE ${CMAKE_SOURCE_DIR}/tools/validate_deployment.py)
set(OVF_DEPLOYMENT_PROFILES ${CMAKE_SOURCE_DIR}/com/deployment/profiles)
foreach(OVF_VALID_DEPLOYMENT IN ITEMS radar-inproc radar-multitransport radar-iceoryx2)
    string(REPLACE "-" "_" OVF_PLAN_TARGET ${OVF_VALID_DEPLOYMENT})
    set(OVF_PLAN ${OVF_GENERATED_DIR}/${OVF_VALID_DEPLOYMENT}.plan.json)
    set(OVF_PLAN_REPEAT ${OVF_GENERATED_DIR}/${OVF_VALID_DEPLOYMENT}.plan-repeat.json)
    add_custom_command(OUTPUT ${OVF_PLAN} ${OVF_PLAN_REPEAT}
        COMMAND ${Python3_EXECUTABLE} -m ${OVF_DEPLOYMENT_TOOL}
            --contract ${OVF_RADAR_IR}
            --deployment ${CMAKE_SOURCE_DIR}/com/deployment/examples/${OVF_VALID_DEPLOYMENT}.json
            --profiles ${OVF_DEPLOYMENT_PROFILES} --output-plan ${OVF_PLAN}
        COMMAND ${Python3_EXECUTABLE} -m ${OVF_DEPLOYMENT_TOOL}
            --contract ${OVF_RADAR_IR}
            --deployment ${CMAKE_SOURCE_DIR}/com/deployment/examples/${OVF_VALID_DEPLOYMENT}.json
            --profiles ${OVF_DEPLOYMENT_PROFILES} --output-plan ${OVF_PLAN_REPEAT}
        DEPENDS ${OVF_DEPLOYMENT_TOOL_SOURCE} ${OVF_RADAR_IR}
            ${CMAKE_SOURCE_DIR}/com/deployment/examples/${OVF_VALID_DEPLOYMENT}.json
            ${CMAKE_SOURCE_DIR}/com/deployment/profiles/inproc.json
            ${CMAKE_SOURCE_DIR}/com/deployment/profiles/cyclonedds.json
            ${CMAKE_SOURCE_DIR}/com/deployment/profiles/iceoryx2.json
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
    add_custom_target(ovf_com_d3_plan_${OVF_PLAN_TARGET} ALL DEPENDS ${OVF_PLAN} ${OVF_PLAN_REPEAT})
    add_test(NAME ovf_com_d3_deployment_${OVF_VALID_DEPLOYMENT}
        COMMAND ${Python3_EXECUTABLE} -m ${OVF_DEPLOYMENT_TOOL}
            --contract ${OVF_RADAR_IR}
            --deployment ${CMAKE_SOURCE_DIR}/com/deployment/examples/${OVF_VALID_DEPLOYMENT}.json
            --profiles ${OVF_DEPLOYMENT_PROFILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME ovf_com_d3_plan_reproducible_${OVF_VALID_DEPLOYMENT}
        COMMAND ${CMAKE_COMMAND} -E compare_files ${OVF_PLAN} ${OVF_PLAN_REPEAT})
endforeach()
add_test(NAME ovf_com_d3_invalid_capabilities_rejected
    COMMAND ${Python3_EXECUTABLE} -m ${OVF_DEPLOYMENT_TOOL}
        --contract ${OVF_RADAR_IR}
        --deployment ${CMAKE_SOURCE_DIR}/com/deployment/examples/radar-invalid-capabilities.json
        --profiles ${OVF_DEPLOYMENT_PROFILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ovf_com_d3_invalid_capabilities_rejected PROPERTIES WILL_FAIL TRUE)
set(OVF_SMITHY_EXECUTABLE "" CACHE FILEPATH "Path to the pinned Smithy CLI")
if(NOT OVF_SMITHY_EXECUTABLE)
    find_program(OVF_SMITHY_EXECUTABLE smithy)
endif()
if(OVF_SMITHY_EXECUTABLE)
    execute_process(COMMAND ${OVF_SMITHY_EXECUTABLE} --version
        OUTPUT_VARIABLE OVF_SMITHY_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE OVF_SMITHY_VERSION_RESULT)
    if(NOT OVF_SMITHY_VERSION_RESULT EQUAL 0 OR NOT OVF_SMITHY_VERSION STREQUAL "1.72.1")
        message(FATAL_ERROR "D3 requires the pinned Smithy CLI 1.72.1")
    endif()
    set(OVF_SMITHY_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/smithy)
    set(OVF_SMITHY_AST ${OVF_SMITHY_OUTPUT}/ovf/model/model.json)
    set(OVF_SMITHY_IR ${OVF_SMITHY_OUTPUT}/radar.ovf-ir.json)
    add_custom_command(OUTPUT ${OVF_SMITHY_AST}
        COMMAND ${OVF_SMITHY_EXECUTABLE} build
            --config ${CMAKE_SOURCE_DIR}/com/model/smithy-build.json
            --output ${OVF_SMITHY_OUTPUT} --no-color
        DEPENDS ${CMAKE_SOURCE_DIR}/com/model/smithy-build.json
            ${CMAKE_SOURCE_DIR}/com/model/ovf-profile.smithy
            ${CMAKE_SOURCE_DIR}/contracts/radar/radar.smithy
        VERBATIM)
    add_custom_command(OUTPUT ${OVF_SMITHY_IR}
        COMMAND ${Python3_EXECUTABLE} -m tools.smithy_ast_to_ir
            ${OVF_SMITHY_AST} --output ${OVF_SMITHY_IR}
        DEPENDS ${OVF_SMITHY_AST} ${CMAKE_SOURCE_DIR}/tools/smithy_ast_to_ir.py
            ${CMAKE_SOURCE_DIR}/tools/validate_ir.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
    add_custom_target(ovf_com_d3_smithy ALL DEPENDS ${OVF_SMITHY_IR})
    add_test(NAME ovf_com_d3_smithy_ir_equivalence
        COMMAND ${Python3_EXECUTABLE} -m tools.compare_ir
            ${OVF_SMITHY_IR} ${OVF_RADAR_IR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    message(STATUS "Smithy CLI not found; reference-assembler gate is unavailable")
endif()
