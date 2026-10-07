set(LLAMA_CPP_DIR "${CMAKE_CURRENT_LIST_DIR}/../entry/src/main/cpp/third_party/llama.cpp")
set(LLAMA_OHOS_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/llama-cpp-ohos.patch")

if(NOT EXISTS "${LLAMA_CPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "llama.cpp submodule is missing. Run: git submodule update --init --recursive")
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check "${LLAMA_OHOS_PATCH}"
    WORKING_DIRECTORY "${LLAMA_CPP_DIR}"
    RESULT_VARIABLE LLAMA_PATCH_CAN_APPLY
    OUTPUT_QUIET
    ERROR_QUIET)

if(LLAMA_PATCH_CAN_APPLY EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${LLAMA_OHOS_PATCH}"
        WORKING_DIRECTORY "${LLAMA_CPP_DIR}"
        RESULT_VARIABLE LLAMA_PATCH_RESULT)
    if(NOT LLAMA_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply the llama.cpp HarmonyOS patch")
    endif()
else()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${LLAMA_OHOS_PATCH}"
        WORKING_DIRECTORY "${LLAMA_CPP_DIR}"
        RESULT_VARIABLE LLAMA_PATCH_ALREADY_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT LLAMA_PATCH_ALREADY_APPLIED EQUAL 0)
        message(FATAL_ERROR
            "llama.cpp does not match the pinned revision or expected HarmonyOS patch")
    endif()
endif()
