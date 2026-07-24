# ==============================================================================
# Junk Calculator 2 - Ultimate CMake Build Script
# Version: 2.5.2.0
# ==============================================================================
cmake_minimum_required(VERSION 3.15)

# ★ Enable CMake 3.15+ MSVC clean runtime control policy
cmake_policy(SET CMP0091 NEW)

# ★ Elegantly force static linking (MT/MTd) across all configs.
# This permanently eliminates the "missing VCRUNTIME140.dll" error on target machines!
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(JunkCalculator2 VERSION 2.5.2.0 LANGUAGES CXX)

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
    "src/frontend/Token.h" "src/frontend/Lexer.h" "src/frontend/Expr.h" "src/frontend/Parser.h" "src/frontend/Highlight.h" "src/frontend/ASTConverter.h"
    "src/memory/Value.h" "src/memory/GcHeap.h" "src/memory/SipHash.h"
    "src/math/Complex.h" "src/math/Matrix.h" "src/math/BigInt.h" "src/math/Fraction.h" "src/math/Base.h" "src/math/Tolerance.h"
    "src/lib/image/Image.h"
    "src/vm/HelpRouter.h" "src/vm/GeneratedHelpText.h" "src/vm/Bytecode.h" "src/vm/BytecodeSerializer.h" "src/vm/BuiltinRegistry.h" "src/vm/PredefinedClasses.h" "src/vm/EngineInterrupt.h" "src/vm/VM.h"
    "src/cas/Symbolic.h" "src/cas/Integration.h" "src/cas/Factorization.h" "src/cas/Groebner.h"
    "src/compiler/Resolver.h" "src/compiler/IR.h" "src/compiler/IRBuilder.h" "src/compiler/IROptimizer.h" "src/compiler/RegisterAllocator.h" "src/compiler/Emitter.h"
    "src/lib/ExtensionBridge.h"
    "src/lib/jc2_extension_api.h"
    "src/lib/jc2_extension_cpp.h"
)

set(JC2_SOURCES
    "src/lib/ExtensionBridge.cpp"
    "src/frontend/Lexer.cpp" "src/frontend/Parser.cpp" "src/frontend/ASTConverter.cpp"
    "src/compiler/Resolver.cpp"
    "src/compiler/IRBuilder.cpp"
    "src/compiler/IROptimizer.cpp"
    "src/compiler/RegisterAllocator.cpp"
    "src/compiler/Emitter.cpp"
    "src/vm/VM.cpp"
    "src/vm/BytecodeSerializer.cpp"
    "src/vm/BuiltinRegistry.cpp" "src/vm/PredefinedClasses.cpp" "src/main.cpp"
    "src/cas/Symbolic.cpp" "src/cas/Integration.cpp" "src/cas/Factorization.cpp" "src/cas/Groebner.cpp"
    "src/resource.rc"
)

add_executable(JunkCalculator2 ${JC2_HEADERS} ${JC2_SOURCES})
target_include_directories(JunkCalculator2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ★ Build Image Module as a standalone DLL
add_library(image SHARED "src/lib/image/image_module.cpp")
target_include_directories(image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(image PROPERTIES PREFIX "") # Output image.dll instead of libimage.dll

# ★ Build Tensor Module as a standalone DLL
add_library(tensor SHARED "src/lib/tensor/tensor_module.cpp")
target_include_directories(tensor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(tensor PROPERTIES PREFIX "") # Output tensor.dll instead of libtensor.dll

# ★ Build Prob Module as a standalone DLL
add_library(prob SHARED "src/lib/prob/prob_module.cpp")
target_include_directories(prob PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(prob PROPERTIES PREFIX "") # Output prob.dll instead of libprob.dll

# ★ Build Bytes Module as a standalone DLL
add_library(bytes SHARED "src/lib/bytes/bytes_module.cpp")
target_include_directories(bytes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(bytes PROPERTIES PREFIX "") # Output bytes.dll instead of libbytes.dll

# ★ Build FFI Module as a standalone DLL
add_library(ffi SHARED "src/lib/ffi/ffi_module.cpp")
target_include_directories(ffi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(ffi PROPERTIES PREFIX "") # Output ffi.dll instead of libffi.dll

# ★ Build Socket Module as a standalone DLL
add_library(socket SHARED "src/lib/socket/socket_module.cpp")
target_include_directories(socket PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(socket PROPERTIES PREFIX "") # Output socket.dll instead of libsocket.dll

# ★ Build JSON Module as a standalone DLL
add_library(json SHARED "src/lib/json/json_module.cpp")
target_include_directories(json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(json PROPERTIES PREFIX "") # Output json.dll instead of libjson.dll

# ★ Build Latex Module as a standalone DLL
add_library(latex SHARED "src/lib/latex/latex_module.cpp")
target_include_directories(latex PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(latex PROPERTIES PREFIX "") # Output latex.dll instead of liblatex.dll

# ★ Build Window Module as a standalone DLL
add_library(window SHARED "src/lib/window/window_module.cpp")
target_include_directories(window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(window PROPERTIES PREFIX "") # Output window.dll instead of libwindow.dll

# ★ Critical Linkage for Win32 OS:
# ws2_32 (Networking), User32 & Gdi32 (Native GUI), imm32 (IME Focus Control)
if(WIN32)
    target_link_libraries(JunkCalculator2 PRIVATE ws2_32 User32 Gdi32 imm32)
    target_link_libraries(socket PRIVATE ws2_32)
    target_link_libraries(window PRIVATE User32 Gdi32 imm32)
endif()

# ★ Output libraries directly to the lib/ directory
set(JC2_LIBS image tensor prob bytes ffi socket json latex window)
foreach(mod ${JC2_LIBS})
    set_target_properties(${mod} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
    )
endforeach()

# ==============================================================================
# Extreme Compiler Optimization & Safety Levels
# ==============================================================================
if(MSVC)
    # /utf-8: Prevent encoding corruption | /EHsc: Standard C++ exception handling
    foreach(target JunkCalculator2 ${JC2_LIBS})
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /bigobj /EHsc)
    endforeach()
    
    # Extreme Release mode optimization (Enabled LTCG for maximum performance)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /Ot /arch:AVX2 /Gw /Gy /Zc:inline /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF /LTCG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF /LTCG")
else()
    foreach(target JunkCalculator2 ${JC2_LIBS})
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -O3 -march=native)
        # Force static linking for MinGW to ensure portability (Green version)
        if(MINGW OR CYGWIN)
            target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endforeach()

    # Enable Link Time Optimization (LTO) for GCC/Clang
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set_target_properties(JunkCalculator2 ${JC2_LIBS} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
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

set(SYNC_CMD_MODULES "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules")
    set(SYNC_CMD_MODULES COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/modules" "$<TARGET_FILE_DIR:JunkCalculator2>/modules")
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
    ${SYNC_CMD_MODULES}
    ${SYNC_CMD_EXAMPLES}
    ${SYNC_CMD_TESTS}
    COMMENT "Syncing resource directories (data, modules, examples, tests) to build output..."
)
add_dependencies(SyncResources JunkCalculator2)

# ==============================================================================
# CPack Installation & Release Packaging Rules
# ==============================================================================
install(TARGETS JunkCalculator2 RUNTIME DESTINATION bin)
install(TARGETS ${JC2_LIBS}
    RUNTIME DESTINATION bin/lib
    LIBRARY DESTINATION bin/lib
)
if(WIN32)
    install(PROGRAMS "$<TARGET_FILE_DIR:JunkCalculator2>/jc.bat" DESTINATION bin)
else()
    install(PROGRAMS "$<TARGET_FILE_DIR:JunkCalculator2>/jc" DESTINATION bin)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data" DESTINATION bin PATTERN "Prime.bin" EXCLUDE)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/modules" DESTINATION bin)
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
set(CPACK_PACKAGE_VERSION "2.5.2.0")
set(CPACK_PACKAGE_VENDOR "Yu Liangyang")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-Windows-x64")
include(CPack)

