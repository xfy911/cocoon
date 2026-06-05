# Cocoon - 跨平台构建配置
#
# 支持 Linux 和 Windows 构建。
# 自动检测平台并选择正确的编译选项和链接库。
#
# 使用方式:
#   mkdir build && cd build
#   cmake ..
#   make  # Linux
#   cmake --build .  # Windows (MSBuild / Ninja)
#
# @author xfy

cmake_minimum_required(VERSION 3.10)
project(cocoon C)

# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 源文件列表
set(COCOON_SOURCES
    main.c
    server.c
    http.c
    static.c
    log.c
    config.c
    multipart.c
    platform.c
    tls.c
    http2.c
    access_log.c
    websocket.c
    middleware.c
)

# 头文件路径
set(COCOON_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/coco/include
)

# === 平台检测 ===
if(WIN32)
    # Windows 构建
    message(STATUS "平台: Windows")
    
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    
    # 编译选项
    add_compile_options(
        /W4
        /O2
        $<$<CONFIG:Debug>:/Zi>
    )
    
    # 链接库
    set(COCOON_LIBS ws2_32)
    
    # 静态运行时（可选，避免 MSVC 运行时依赖）
    # set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
else()
    # Linux / macOS 构建
    message(STATUS "平台: POSIX (Linux/macOS)")
    
    # 编译选项
    add_compile_options(
        -Wall
        -Wextra
        -O2
        -D_GNU_SOURCE
    )
    
    # 调试模式
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -DCOCOON_DEBUG)
    endif()
    
    # 链接库
    find_package(Threads REQUIRED)
    set(COCOON_LIBS
        Threads::Threads
        m
        z
    )
    
    # 可选：liburing（Linux 5.1+）
    find_library(URING_LIB uring)
    if(URING_LIB)
        message(STATUS "找到 liburing，启用 io_uring 支持")
        list(APPEND COCOON_LIBS ${URING_LIB})
    else()
        message(STATUS "未找到 liburing，跳过 io_uring 支持")
    endif()
    
    # Brotli 压缩库
    find_library(BROTLI_LIB brotlienc)
    if(BROTLI_LIB)
        list(APPEND COCOON_LIBS ${BROTLI_LIB})
    else()
        message(WARNING "未找到 brotlienc 库，Brotli 压缩将不可用")
    endif()
endif()

# === 目标定义 ===
add_executable(cocoon ${COCOON_SOURCES})

target_include_directories(cocoon PRIVATE ${COCOON_INCLUDE_DIRS})

# coco 库依赖（优先使用 submodule）
set(COCO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/coco" CACHE PATH "coco 库路径")

if(EXISTS "${COCO_DIR}/CMakeLists.txt")
    message(STATUS "使用 coco submodule: ${COCO_DIR}")
    add_subdirectory(${COCO_DIR} ${CMAKE_BINARY_DIR}/coco)
    target_link_libraries(cocoon PRIVATE coco ${COCOON_LIBS})
else()
    message(STATUS "未找到 coco 子模块，假设已预编译")
    # 假设 coco 已预编译并可通过 find_library 找到
    # target_link_libraries(cocoon PRIVATE ${COCOON_LIBS})
endif()

# === 安装规则 ===
install(TARGETS cocoon DESTINATION bin)

# === 测试支持 ===
enable_testing()

# 集成测试
find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
    add_test(
        NAME integration_test
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

# 单元测试（Unity 框架）
set(UNIT_TESTS test_server test_http test_static test_log test_config test_multipart)

foreach(test_name ${UNIT_TESTS})
    set(test_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/${test_name}.c")
    if(EXISTS ${test_src})
        add_executable(${test_name} ${test_src} ${COCOON_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unity/unity.c)
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/unity
        )
        target_link_libraries(${test_name} PRIVATE ${COCOON_LIBS})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()
