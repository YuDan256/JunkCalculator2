# ==============================================================================
# Junk Calculator 2 - Ultimate CMake Build Script
# Version: 2.4.4.3
# ==============================================================================
cmake_minimum_required(VERSION 3.15)

# ★ Enable CMake 3.15+ MSVC clean runtime control policy
cmake_policy(SET CMP0091 NEW)

# ★ Elegantly force static linking (MT/MTd) across all configs.
# This permanently eliminates the "missing VCRUNTIME140.dll" error on target machines!
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(JunkCalculator2 VERSION 2.4.4.3 LANGUAGES CXX)

# Enforce C++ 20 Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Generate HelpText header from JSON (Configure Time)
# ==============================================================================
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "data/documentation.json")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/documentation.json")
    # 读取文件为十六进制字符串 (例如 "7b2274...")
    file(READ "data/documentation.json" HEX_CONTENT HEX)
    # 使用正则将每两个字符替换为 "0xXX, " 格式
    string(REGEX REPLACE "(..)" "0x\\1, " BYTE_ARRAY "${HEX_CONTENT}")
    # 生成 C 风格的字节数组，彻底绕过 MSVC 字符串字面量 16KB 长度限制
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/GeneratedHelpText.h"
        "// Auto-generated from documentation.json\n"
        "namespace jc {\n"
        "inline constexpr const unsigned char RAW_HELP_JSON[] = {\n"
        "${BYTE_ARRAY}0x00\n"
        "};\n"
        "} // namespace jc\n"
    )
endif()

# ==============================================================================
# Core Sources & Headers Collection
# ==============================================================================
set(JC2_HEADERS
    "src/frontend/Token.h" "src/frontend/Lexer.h" "src/frontend/Expr.h" "src/frontend/Parser.h" "src/frontend/Highlight.h" "src/frontend/Compiler.h"
    "src/memory/Value.h" "src/memory/GcHeap.h" "src/memory/SipHash.h"
    "src/math/Complex.h" "src/math/Matrix.h" "src/math/BigInt.h" "src/math/Fraction.h" "src/math/Base.h" "src/math/Tolerance.h" "src/math/Tensor.h"
    "src/modules/Image.h" "src/modules/Probability.h" "src/modules/Module.h"
    "src/vm/HelpRouter.h" "src/vm/GeneratedHelpText.h" "src/vm/Bytecode.h" "src/vm/VM.h" "src/vm/BuiltinRegistry.h" "src/vm/EngineInterrupt.h"
    "src/cas/Symbolic.h" "src/cas/Integration.h" "src/cas/Factorization.h" "src/cas/Groebner.h"
    "src/modules/json_module.h"
    "src/modules/prob_module.h"
    "src/modules/bytes_module.h" 
    "src/modules/socket_module.h"
    "src/modules/window_module.h"
    "src/modules/latex_module.h"
    "src/modules/ffi_module.h"
    "src/modules/ExtensionBridge.h"
    "src/modules/jc2_extension_api.h"
    "src/modules/jc2_extension_cpp.h"
)

set(JC2_SOURCES
    "src/modules/ExtensionBridge.cpp"
    "src/modules/ffi_module.cpp"
    "src/frontend/Lexer.cpp" "src/frontend/Parser.cpp" "src/frontend/Compiler.cpp"
    "src/vm/VM.cpp" "src/vm/BuiltinRegistry.cpp" "src/main.cpp"
    "src/cas/Symbolic.cpp" "src/cas/Integration.cpp" "src/cas/Factorization.cpp" "src/cas/Groebner.cpp"
    "src/resource.rc"
)

add_executable(JunkCalculator2 ${JC2_HEADERS} ${JC2_SOURCES})
target_include_directories(JunkCalculator2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ★ Build Image Module as a standalone DLL
add_library(image SHARED "src/modules/image_module.cpp")
target_include_directories(image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(image PROPERTIES PREFIX "") # Output image.dll instead of libimage.dll

# ★ Build Tensor Module as a standalone DLL
add_library(tensor SHARED "src/modules/tensor_module.cpp")
target_include_directories(tensor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(tensor PROPERTIES PREFIX "") # Output tensor.dll instead of libtensor.dll

# ★ Critical Linkage for Win32 OS:
# ws2_32 (Networking), User32 & Gdi32 (Native GUI), imm32 (IME Focus Control)
if(WIN32)
    target_link_libraries(JunkCalculator2 PRIVATE ws2_32 User32 Gdi32 imm32)
endif()

# ==============================================================================
# Extreme Compiler Optimization & Safety Levels
# ==============================================================================
if(MSVC)
    # /utf-8: Prevent encoding corruption | /EHsc: Standard C++ exception handling
    target_compile_options(JunkCalculator2 PRIVATE /W4 /WX /utf-8 /bigobj /EHsc)
    target_compile_options(image PRIVATE /W4 /WX /utf-8 /bigobj /EHsc)
    target_compile_options(tensor PRIVATE /W4 /WX /utf-8 /bigobj /EHsc)
    
    # Enable Whole Program Optimization (LTCG) in Release mode for maximum speed
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
else()
    target_compile_options(JunkCalculator2 PRIVATE -Wall -Wextra -Werror -O3)
    target_compile_options(image PRIVATE -Wall -Wextra -Werror -O3)
    target_compile_options(tensor PRIVATE -Wall -Wextra -Werror -O3)
endif()

# ==============================================================================
# Post-Build Resource Injection (Auto-Copying directories)
# ==============================================================================
# ★ Create shorthand wrapper scripts to save space (instead of copying the .exe)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:JunkCalculator2>/jc.bat" CONTENT "@echo off\n\"%~dp0JunkCalculator2.exe\" %*\n")
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:JunkCalculator2>/jc" CONTENT "#!/bin/sh\nexec \"$(dirname \"$0\")/JunkCalculator2\" \"$@\"\n")

# ★ Use a custom target (ALL) to ensure directories are synced on EVERY build,
# even if the C++ executable itself didn't need to be recompiled.
# Using COMMAND directly inside add_custom_target guarantees execution across all generators.
set(SYNC_CMD_DATA "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    set(SYNC_CMD_DATA COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:JunkCalculator2>/data")
endif()

set(SYNC_CMD_LIB "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib")
    set(SYNC_CMD_LIB COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/lib" "$<TARGET_FILE_DIR:JunkCalculator2>/lib")
endif()

set(SYNC_CMD_EXAMPLES "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    set(SYNC_CMD_EXAMPLES COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/examples" "$<TARGET_FILE_DIR:JunkCalculator2>/examples")
endif()

set(SYNC_CMD_TESTS "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set(SYNC_CMD_TESTS COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/tests" "$<TARGET_FILE_DIR:JunkCalculator2>/tests")
endif()

add_custom_target(SyncResources ALL
    ${SYNC_CMD_DATA}
    ${SYNC_CMD_LIB}
    ${SYNC_CMD_EXAMPLES}
    ${SYNC_CMD_TESTS}
    COMMENT "Syncing resource directories (data, lib, examples, tests) to build output..."
)
add_dependencies(SyncResources JunkCalculator2)

# ==============================================================================
# CPack Installation & Release Packaging Rules
# ==============================================================================
install(TARGETS JunkCalculator2 DESTINATION bin)
install(TARGETS image DESTINATION bin)
install(TARGETS tensor DESTINATION bin)
if(WIN32)
    install(PROGRAMS "$<TARGET_FILE_DIR:JunkCalculator2>/jc.bat" DESTINATION bin)
else()
    install(PROGRAMS "$<TARGET_FILE_DIR:JunkCalculator2>/jc" DESTINATION bin)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data" DESTINATION bin PATTERN "Prime.bin" EXCLUDE)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib" DESTINATION bin)
endif()
# ★ Make sure examples are included in the released ZIP!
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples" DESTINATION bin)
endif()
# ★ Make sure tests are included in the released ZIP!
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests" DESTINATION bin)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION .)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt" DESTINATION .)
endif()

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "JunkCalculator2")
set(CPACK_PACKAGE_VERSION "2.4.4.3")
set(CPACK_PACKAGE_VENDOR "Yu Liangyang")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-Windows-x64")
include(CPack)

