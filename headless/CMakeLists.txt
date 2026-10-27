include(FetchContent)
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG b3ce1e612d44d573d17836db6459b52fcfca7113
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(fastgltf)

add_executable(curast-headless main.cpp)
target_compile_features(curast-headless PRIVATE cxx_std_20)
target_include_directories(curast-headless PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/libs ${PROJECT_SOURCE_DIR}/libs/glm)
list(GET CUDAToolkit_INCLUDE_DIRS 0 CURAST_CUDA_INCLUDE)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE CURAST_BUILD_REV OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(curast-headless PRIVATE
    CURAST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    CURAST_BUILD_REV="${CURAST_BUILD_REV}"
    CURAST_CUDA_INCLUDE="${CURAST_CUDA_INCLUDE}")
target_link_libraries(curast-headless PRIVATE
    fastgltf::fastgltf CUDA::cuda_driver CUDA::cudart CUDA::nvrtc)

if(BUILD_TESTING)
    add_test(NAME headless-help COMMAND curast-headless --help)
    add_test(NAME headless-reject-empty COMMAND curast-headless)
    set_tests_properties(headless-reject-empty PROPERTIES WILL_FAIL TRUE)
    option(CURAST_GPU_TESTS "Enable tests that create a CUDA context" OFF)
    if(CURAST_GPU_TESTS)
        add_test(NAME headless-three-stages COMMAND curast-headless --self-test
            --resolution 512 --repeat 5 --report ${CMAKE_CURRENT_BINARY_DIR}/self-test.json)
        set_tests_properties(headless-three-stages PROPERTIES LABELS gpu TIMEOUT 60)
    endif()
endif()
