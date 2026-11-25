cmake_minimum_required(VERSION 3.16)
project(brodalf VERSION 0.9.0 LANGUAGES C)

# Link the C runtime statically so brodalf.exe runs on a PC without the
# Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BRODALF_BUILD_TESTS "Build the core tests" ON)

# Third-party code, vendored so a Windows build needs nothing but a compiler.
add_library(brodalf_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(brodalf_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(brodalf_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_ENABLE_FTS5)

# BLAKE3. On x86-64 the SSE2, SSE4.1, AVX2 and AVX-512 paths (the C
# intrinsics files, not the assembly) are built with their own flags, and
# blake3_dispatch.c picks the best one the CPU has at run time. Elsewhere
# only the portable code is built.
if(MSVC)
    set(BRODALF_ARCH "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
else()
    set(BRODALF_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${BRODALF_ARCH}" BRODALF_ARCH)
set(BRODALF_BLAKE3_SOURCES
    third_party/blake3/blake3.c
    third_party/blake3/blake3_dispatch.c
    third_party/blake3/blake3_portable.c)
set(BRODALF_BLAKE3_OFF BLAKE3_USE_NEON=0)
if(BRODALF_ARCH MATCHES "^(x86_64|amd64|x64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND BRODALF_BLAKE3_SOURCES third_party/blake3/blake3_sse2.c third_party/blake3/blake3_sse41.c)
    set(BRODALF_BLAKE3_WIDE ON)
    set(BRODALF_AVX_FLAGS "")
    if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # GCC does not realign the stack for 32- and 64-byte vectors on 64-bit
        # Windows (GCC bug 54412): have the assembler use unaligned moves, or
        # leave AVX2 and AVX-512 out if it cannot.
        include(CheckCCompilerFlag)
        check_c_compiler_flag("-Wa,-muse-unaligned-vector-move" BRODALF_HAVE_UNALIGNED_MOVE)
        if(BRODALF_HAVE_UNALIGNED_MOVE)
            set(BRODALF_AVX_FLAGS "-Wa,-muse-unaligned-vector-move")
        else()
            set(BRODALF_BLAKE3_WIDE OFF)
        endif()
    endif()
    if(BRODALF_BLAKE3_WIDE)
        list(APPEND BRODALF_BLAKE3_SOURCES third_party/blake3/blake3_avx2.c third_party/blake3/blake3_avx512.c)
    else()
        list(APPEND BRODALF_BLAKE3_OFF BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
    endif()
    if(MSVC)
        set_source_files_properties(third_party/blake3/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        set_source_files_properties(third_party/blake3/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    else()
        set_source_files_properties(third_party/blake3/blake3_sse2.c PROPERTIES COMPILE_OPTIONS "-msse2")
        set_source_files_properties(third_party/blake3/blake3_sse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(third_party/blake3/blake3_avx2.c PROPERTIES
            COMPILE_OPTIONS "-mavx2;${BRODALF_AVX_FLAGS}")
        set_source_files_properties(third_party/blake3/blake3_avx512.c PROPERTIES
            COMPILE_OPTIONS "-mavx512f;-mavx512vl;${BRODALF_AVX_FLAGS}")
    endif()
else()
    list(APPEND BRODALF_BLAKE3_OFF BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif()
add_library(brodalf_blake3 STATIC ${BRODALF_BLAKE3_SOURCES})
target_include_directories(brodalf_blake3 PUBLIC third_party/blake3)
target_compile_definitions(brodalf_blake3 PUBLIC ${BRODALF_BLAKE3_OFF})

add_library(brodalf_zstd STATIC third_party/zstd/zstd.c)
target_include_directories(brodalf_zstd PUBLIC third_party/zstd)
target_compile_definitions(brodalf_zstd PRIVATE ZSTD_DISABLE_ASM)

# Argon2id and XChaCha20-Poly1305 for optional encryption.
add_library(brodalf_monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(brodalf_monocypher PUBLIC third_party/monocypher)

# CI passes the commit, so an error report says exactly which build it came from.
set(BRODALF_BUILD_ID "" CACHE STRING "Commit the build came from")

add_library(brodalf_core STATIC
    src/backup.c
    src/catalog.c
    src/compress.c
    src/crypto.c
    src/drive_hw.c
    src/store_local.c
    src/marks.c
    src/media.c
    src/names.c
    src/platform_posix.c
    src/platform_win32.c
    src/query.c
    src/recover.c
    src/report.c
    src/risk.c
    src/scan.c
    src/shadow.c
    src/sheet.c
    src/status.c
    src/util.c)
target_include_directories(brodalf_core PUBLIC include PRIVATE src)
target_link_libraries(brodalf_core PUBLIC brodalf_sqlite brodalf_blake3 brodalf_zstd brodalf_monocypher)
target_compile_definitions(brodalf_core PRIVATE
    BRODALF_VERSION="${PROJECT_VERSION}"
    BRODALF_BUILD_ID="${BRODALF_BUILD_ID}")
if(WIN32)
    target_link_libraries(brodalf_core PUBLIC bcrypt advapi32 ole32 user32)
    target_compile_definitions(brodalf_core PRIVATE UNICODE _UNICODE)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(brodalf_core PUBLIC Threads::Threads m)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(brodalf_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(brodalf-cli cli/main.c)
target_link_libraries(brodalf-cli PRIVATE brodalf_core)
if(WIN32)
    target_link_libraries(brodalf-cli PRIVATE shell32)
endif()

if(BRODALF_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.c)
    target_link_libraries(test_core PRIVATE brodalf_core)
    target_include_directories(test_core PRIVATE src)
    add_test(NAME core COMMAND test_core)
    add_executable(test_crypto tests/test_crypto.c)
    target_link_libraries(test_crypto PRIVATE brodalf_core)
    target_include_directories(test_crypto PRIVATE src)
    add_test(NAME crypto COMMAND test_crypto)
    add_executable(test_blake3 tests/test_blake3.c)
    target_link_libraries(test_blake3 PRIVATE brodalf_core)
    target_include_directories(test_blake3 PRIVATE src)
    add_test(NAME blake3 COMMAND test_blake3)
    add_executable(bench_many tests/bench_many.c)
    target_link_libraries(bench_many PRIVATE brodalf_core)
    target_include_directories(bench_many PRIVATE src)
    add_test(NAME many_files COMMAND bench_many 5000)
endif()

if(WIN32)
    add_executable(brodalf WIN32 gui/main.c gui/brodalf.rc)
    # The translations are built into the .exe (tools/lang.py checks them).
    file(GLOB BRODALF_PO ${CMAKE_CURRENT_SOURCE_DIR}/gui/lang/*.po)
    set_source_files_properties(gui/brodalf.rc PROPERTIES OBJECT_DEPENDS "${BRODALF_PO}")
    target_link_libraries(brodalf PRIVATE brodalf_core comctl32 shell32 ole32 uuid uxtheme)
    target_compile_definitions(brodalf PRIVATE UNICODE _UNICODE)
    if(MSVC)
        # The manifest is embedded through the .rc file.
        set_target_properties(brodalf PROPERTIES LINK_FLAGS "/MANIFEST:NO")
    else()
        target_link_options(brodalf PRIVATE -municode)
        target_compile_options(brodalf PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()
