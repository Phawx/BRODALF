cmake_minimum_required(VERSION 3.16)
project(brodalf C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BRODALF_BUILD_TESTS "Build the core tests" ON)

# Third-party code, vendored so a Windows build needs nothing but a compiler.
add_library(brodalf_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(brodalf_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(brodalf_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_ENABLE_FTS5)

# Portable BLAKE3 build. The SIMD paths can be switched on later for speed.
add_library(brodalf_blake3 STATIC
    third_party/blake3/blake3.c
    third_party/blake3/blake3_dispatch.c
    third_party/blake3/blake3_portable.c)
target_include_directories(brodalf_blake3 PUBLIC third_party/blake3)
target_compile_definitions(brodalf_blake3 PUBLIC
    BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

add_library(brodalf_zstd STATIC third_party/zstd/zstd.c)
target_include_directories(brodalf_zstd PUBLIC third_party/zstd)
target_compile_definitions(brodalf_zstd PRIVATE ZSTD_DISABLE_ASM)

# Argon2id and XChaCha20-Poly1305 for optional encryption.
add_library(brodalf_monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(brodalf_monocypher PUBLIC third_party/monocypher)

add_library(brodalf_cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(brodalf_cjson PUBLIC third_party/cjson)

# Cloud app registrations. The IDs are public (sign-in uses PKCE), so they
# can be baked in at build time or supplied through environment variables.
set(BRODALF_ONEDRIVE_CLIENT_ID "" CACHE STRING "Microsoft Entra application (client) ID")
set(BRODALF_DROPBOX_CLIENT_ID "" CACHE STRING "Dropbox app key")

if(WIN32)
    set(BRODALF_HTTP_SRC src/http_win32.c)
else()
    set(BRODALF_HTTP_SRC src/http_posix.c)
endif()

add_library(brodalf_core STATIC
    src/backup.c
    src/catalog.c
    src/crypto.c
    src/cloud.c
    src/http_common.c
    ${BRODALF_HTTP_SRC}
    src/secrets.c
    src/store_local.c
    src/media.c
    src/platform_posix.c
    src/platform_win32.c
    src/query.c
    src/scan.c
    src/util.c)
target_include_directories(brodalf_core PUBLIC include PRIVATE src)
target_link_libraries(brodalf_core PUBLIC brodalf_sqlite brodalf_blake3 brodalf_zstd brodalf_monocypher brodalf_cjson)
target_compile_definitions(brodalf_core PRIVATE
    BRODALF_ONEDRIVE_CLIENT_ID="${BRODALF_ONEDRIVE_CLIENT_ID}"
    BRODALF_DROPBOX_CLIENT_ID="${BRODALF_DROPBOX_CLIENT_ID}")
if(WIN32)
    target_link_libraries(brodalf_core PUBLIC bcrypt winhttp ws2_32 advapi32 crypt32)
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
    # Cloud code against a local mock of OneDrive and Dropbox (POSIX only:
    # the test forks a fake browser).
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT WIN32 AND Python3_FOUND)
        add_executable(test_cloud tests/test_cloud.c)
        target_link_libraries(test_cloud PRIVATE brodalf_core)
        target_include_directories(test_cloud PRIVATE src)
        add_test(NAME cloud COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_cloud.py $<TARGET_FILE:test_cloud>)
    endif()
endif()

if(WIN32)
    add_executable(brodalf WIN32 gui/main.c gui/brodalf.rc)
    target_link_libraries(brodalf PRIVATE brodalf_core comctl32 shell32 ole32 uuid)
    target_compile_definitions(brodalf PRIVATE UNICODE _UNICODE)
    if(MSVC)
        # The manifest is embedded through the .rc file.
        set_target_properties(brodalf PROPERTIES LINK_FLAGS "/MANIFEST:NO")
    else()
        target_link_options(brodalf PRIVATE -municode)
        target_compile_options(brodalf PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()
