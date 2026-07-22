file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")

list(LENGTH SOURCE_FILES SRC_FILES_SIZE)
message(STATUS "Found ${SRC_FILES_SIZE} source files")
foreach(source_file ${SOURCE_FILES})
    message(STATUS " ${source_file}")
endforeach()

set(PROJECT_SOURCES
    ${SOURCE_FILES}
)

if(MSVC)
    add_compile_options(/FS)
endif()

include(FetchContent)

# Keep release builds reproducible: selecting OpenSSL is explicit, while the
# native mode leaves TLS to the platform backend (Schannel on Windows).
# The legacy WUWE_ENABLE_OPENSSL cache option is mapped when the new setting is
# not present so existing build directories continue to configure naturally.
if(NOT DEFINED WUWE_TLS_BACKEND)
    if(DEFINED WUWE_ENABLE_OPENSSL)
        if(WUWE_ENABLE_OPENSSL)
            set(WUWE_TLS_BACKEND "auto")
        else()
            set(WUWE_TLS_BACKEND "native")
        endif()
    elseif(WIN32)
        set(WUWE_TLS_BACKEND "native")
    else()
        set(WUWE_TLS_BACKEND "auto")
    endif()
endif()
set(WUWE_TLS_BACKEND "${WUWE_TLS_BACKEND}" CACHE STRING
    "TLS backend selection: native, openssl, or auto")
set_property(CACHE WUWE_TLS_BACKEND PROPERTY STRINGS native openssl auto)
string(TOLOWER "${WUWE_TLS_BACKEND}" WUWE_TLS_BACKEND_NORMALIZED)
if(NOT WUWE_TLS_BACKEND_NORMALIZED STREQUAL "native" AND
   NOT WUWE_TLS_BACKEND_NORMALIZED STREQUAL "openssl" AND
   NOT WUWE_TLS_BACKEND_NORMALIZED STREQUAL "auto")
    message(FATAL_ERROR "WUWE_TLS_BACKEND must be 'native', 'openssl', or 'auto'")
endif()

set(WUWE_OPENSSL_AVAILABLE OFF)
if(WUWE_TLS_BACKEND_NORMALIZED STREQUAL "openssl" OR
   WUWE_TLS_BACKEND_NORMALIZED STREQUAL "auto")
    if(NOT OPENSSL_ROOT_DIR)
        find_program(WUWE_OPENSSL_EXECUTABLE openssl)
        if(WUWE_OPENSSL_EXECUTABLE)
            get_filename_component(WUWE_OPENSSL_BIN_DIR "${WUWE_OPENSSL_EXECUTABLE}" DIRECTORY)
            get_filename_component(WUWE_OPENSSL_ROOT_FROM_EXE "${WUWE_OPENSSL_BIN_DIR}" DIRECTORY)
            if(EXISTS "${WUWE_OPENSSL_ROOT_FROM_EXE}/include/openssl/ssl.h")
                set(OPENSSL_ROOT_DIR "${WUWE_OPENSSL_ROOT_FROM_EXE}" CACHE PATH
                    "OpenSSL root inferred from openssl executable" FORCE)
            endif()
        endif()
    endif()
    if(WUWE_TLS_BACKEND_NORMALIZED STREQUAL "openssl")
        find_package(OpenSSL REQUIRED)
    else()
        find_package(OpenSSL QUIET)
    endif()
    if(OpenSSL_FOUND)
        set(WUWE_OPENSSL_AVAILABLE ON)
    endif()
endif()

if(WUWE_OPENSSL_AVAILABLE)
    set(WUWE_TLS_BACKEND_RESOLVED "openssl")
    set(CPR_FORCE_OPENSSL_BACKEND ON CACHE BOOL "Use OpenSSL as cpr/curl TLS backend" FORCE)
    set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL for libcurl HTTPS support" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "Disable Schannel when OpenSSL is selected" FORCE)
    endif()
    message(STATUS "Wuwe TLS backend: OpenSSL")
else()
    set(WUWE_TLS_BACKEND_RESOLVED "native")
    set(CPR_FORCE_OPENSSL_BACKEND OFF CACHE BOOL "Do not force the OpenSSL cpr/curl backend" FORCE)
    if(WIN32)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "Do not use OpenSSL for the native Windows build" FORCE)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Schannel for the native Windows build" FORCE)
    endif()
    message(STATUS "Wuwe TLS backend: native platform backend")
endif()
set(WUWE_BUILT_WITH_OPENSSL ${WUWE_OPENSSL_AVAILABLE} CACHE INTERNAL
    "Whether Wuwe links OpenSSL" FORCE)
set(WUWE_TLS_BACKEND_RESOLVED "${WUWE_TLS_BACKEND_RESOLVED}" CACHE INTERNAL
    "TLS backend used by this Wuwe build" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build curl static libraries" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "Disable libpsl to avoid meson dependency on Windows" FORCE)
# Avoid host-environment-dependent static link requirements. Wuwe does not
# advertise Brotli or zstd content decoding in 0.1.0; gzip/deflate remain
# available through curl's pinned zlib dependency.
set(CURL_BROTLI OFF CACHE STRING "Disable optional Brotli support in bundled curl" FORCE)
set(CURL_ZSTD OFF CACHE STRING "Disable optional zstd support in bundled curl" FORCE)
# curl's find modules cache discovery results. Clear those results as well so
# switching an existing build directory from AUTO to OFF removes stale link
# entries instead of requiring users to delete the whole build tree.
set(BROTLI_FOUND OFF CACHE INTERNAL "Brotli disabled for bundled curl" FORCE)
set(ZSTD_FOUND OFF CACHE INTERNAL "zstd disabled for bundled curl" FORCE)
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git
                         GIT_TAG f091b2c061b307ee89b164c39976fc9202a1c79d)
FetchContent_MakeAvailable(cpr)
set_target_properties(cpr PROPERTIES
    DEBUG_POSTFIX d
)

set(WUWE_HTTP_BACKEND "cpr" CACHE STRING "Default HTTP backend: cpr or httplib")
set_property(CACHE WUWE_HTTP_BACKEND PROPERTY STRINGS cpr httplib)
string(TOLOWER "${WUWE_HTTP_BACKEND}" WUWE_HTTP_BACKEND_NORMALIZED)
if(NOT WUWE_HTTP_BACKEND_NORMALIZED STREQUAL "cpr" AND
   NOT WUWE_HTTP_BACKEND_NORMALIZED STREQUAL "httplib")
    message(FATAL_ERROR "WUWE_HTTP_BACKEND must be either 'cpr' or 'httplib'")
endif()

# ---------------------------------------------------------------------------------------
# Static/Shared library
# ---------------------------------------------------------------------------------------
add_library(${PROJECT_NAME} STATIC ${PROJECT_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES
    DEBUG_POSTFIX d
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
)

target_link_libraries(${PROJECT_NAME} PRIVATE cpr::cpr)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PUBLIC Advapi32 Userenv)
endif()

if(WUWE_HTTP_BACKEND_NORMALIZED STREQUAL "httplib")
    target_compile_definitions(${PROJECT_NAME} PRIVATE WUWE_DEFAULT_HTTP_BACKEND_HTTPLIB=1)
    message(STATUS "Wuwe default HTTP backend: cpp-httplib")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE WUWE_DEFAULT_HTTP_BACKEND_CPR=1)
    message(STATUS "Wuwe default HTTP backend: cpr/libcurl")
endif()

option(WUWE_ENABLE_HTTPLIB_SSL "Enable HTTPS support for the cpp-httplib HTTP backend when OpenSSL is available" ON)
set(WUWE_HTTPLIB_HTTPS_AVAILABLE OFF)
if(WUWE_ENABLE_HTTPLIB_SSL AND WUWE_OPENSSL_AVAILABLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set(WUWE_HTTPLIB_HTTPS_AVAILABLE ON)
    message(STATUS "cpp-httplib HTTPS support enabled")
elseif(WUWE_ENABLE_HTTPLIB_SSL)
    message(STATUS "cpp-httplib HTTPS support disabled: the selected TLS backend does not provide OpenSSL")
endif()
set(WUWE_BUILT_WITH_HTTPLIB_HTTPS ${WUWE_HTTPLIB_HTTPS_AVAILABLE} CACHE INTERNAL
    "Whether cpp-httplib HTTPS support is enabled" FORCE)

# AUTO is convenient for local development. Official release presets use ON so
# a missing SQLite development package is a configuration error, not a silent
# capability downgrade. Preserve the old boolean option for existing caches.
if(NOT DEFINED WUWE_SQLITE_MODE)
    if(DEFINED WUWE_ENABLE_SQLITE)
        if(WUWE_ENABLE_SQLITE)
            set(WUWE_SQLITE_MODE "auto")
        else()
            set(WUWE_SQLITE_MODE "off")
        endif()
    else()
        set(WUWE_SQLITE_MODE "auto")
    endif()
endif()
set(WUWE_SQLITE_MODE "${WUWE_SQLITE_MODE}" CACHE STRING
    "SQLite capability: auto, on (required), or off")
set_property(CACHE WUWE_SQLITE_MODE PROPERTY STRINGS auto on off)
string(TOLOWER "${WUWE_SQLITE_MODE}" WUWE_SQLITE_MODE_NORMALIZED)
if(NOT WUWE_SQLITE_MODE_NORMALIZED STREQUAL "auto" AND
   NOT WUWE_SQLITE_MODE_NORMALIZED STREQUAL "on" AND
   NOT WUWE_SQLITE_MODE_NORMALIZED STREQUAL "off")
    message(FATAL_ERROR "WUWE_SQLITE_MODE must be 'auto', 'on', or 'off'")
endif()

if(NOT WUWE_SQLITE_MODE_NORMALIZED STREQUAL "off")
    find_package(SQLite3 QUIET)
    if(NOT SQLite3_FOUND)
        find_package(unofficial-sqlite3 CONFIG QUIET)
    endif()
endif()

set(WUWE_SQLITE_DEPENDENCY "none")
set(WUWE_SQLITE_AVAILABLE OFF)
if(SQLite3_FOUND AND NOT WUWE_SQLITE_MODE_NORMALIZED STREQUAL "off")
    target_link_libraries(${PROJECT_NAME} PUBLIC SQLite::SQLite3)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WUWE_HAS_SQLITE=1)
    set(WUWE_SQLITE_DEPENDENCY "cmake")
    set(WUWE_SQLITE_AVAILABLE ON)
    message(STATUS "SQLite memory and knowledge persistence enabled (SQLite::SQLite3)")
elseif(unofficial-sqlite3_FOUND AND NOT WUWE_SQLITE_MODE_NORMALIZED STREQUAL "off")
    target_link_libraries(${PROJECT_NAME} PUBLIC unofficial::sqlite3::sqlite3)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WUWE_HAS_SQLITE=1)
    set(WUWE_SQLITE_DEPENDENCY "unofficial")
    set(WUWE_SQLITE_AVAILABLE ON)
    message(STATUS "SQLite memory and knowledge persistence enabled (unofficial-sqlite3)")
else()
    if(WUWE_SQLITE_MODE_NORMALIZED STREQUAL "on")
        message(FATAL_ERROR
            "WUWE_SQLITE_MODE=on requires SQLite3. Install a SQLite3 development package "
            "or configure with -DWUWE_SQLITE_MODE=off.")
    endif()
    target_compile_definitions(${PROJECT_NAME} PUBLIC WUWE_HAS_SQLITE=0)
    message(STATUS "SQLite memory and knowledge persistence disabled")
endif()
set(WUWE_BUILT_WITH_SQLITE ${WUWE_SQLITE_AVAILABLE} CACHE INTERNAL
    "Whether SQLite capabilities are compiled into Wuwe" FORCE)
set(WUWE_SQLITE_DEPENDENCY "${WUWE_SQLITE_DEPENDENCY}" CACHE INTERNAL
    "SQLite package used by the installed Wuwe target" FORCE)

message(STATUS "Wuwe capability summary:")
message(STATUS "  HTTP backend: ${WUWE_HTTP_BACKEND_NORMALIZED}")
message(STATUS "  TLS backend: ${WUWE_TLS_BACKEND_RESOLVED} (requested: ${WUWE_TLS_BACKEND_NORMALIZED})")
message(STATUS "  OpenSSL linked: ${WUWE_BUILT_WITH_OPENSSL}")
message(STATUS "  cpp-httplib HTTPS: ${WUWE_BUILT_WITH_HTTPLIB_HTTPS}")
message(STATUS "  SQLite: ${WUWE_BUILT_WITH_SQLITE} (${WUWE_SQLITE_DEPENDENCY})")

# target_compile_options(arks PRIVATE)
# target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

# ---------------------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
        EXPORT ${PROJECT_NAME}-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/include/wuwe
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    REGEX "/runtime($|/)" EXCLUDE
)

option(WUWE_INSTALL_BUNDLED_RUNTIME "Install bundled runtime sidecars" ON)
set(WUWE_BUNDLED_TIKA_DIR
    "${CMAKE_SOURCE_DIR}/third_party/runtime/tika"
    CACHE PATH "Bundled Tika runtime directory")

set(WUWE_DEFAULT_BUNDLED_JRE_ARCHIVE "")
set(WUWE_DEFAULT_BUNDLED_JAVA_RELATIVE_PATH "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WUWE_DEFAULT_BUNDLED_JRE_ARCHIVE
        "${CMAKE_SOURCE_DIR}/third_party/runtime/jre/temurin-21-jre-windows-x64.zip")
    set(WUWE_DEFAULT_BUNDLED_JAVA_RELATIVE_PATH "bin/java.exe")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(WUWE_DEFAULT_BUNDLED_JRE_ARCHIVE
        "${CMAKE_SOURCE_DIR}/third_party/runtime/jre/temurin-21-jre-linux-x64.tar.gz")
    set(WUWE_DEFAULT_BUNDLED_JAVA_RELATIVE_PATH "bin/java")
endif()

set(WUWE_BUNDLED_JRE_ARCHIVE
    "${WUWE_DEFAULT_BUNDLED_JRE_ARCHIVE}"
    CACHE FILEPATH "Platform-specific bundled JRE archive")
set(WUWE_BUNDLED_JRE_SHA256_FILE
    "${WUWE_BUNDLED_JRE_ARCHIVE}.sha256"
    CACHE FILEPATH "SHA-256 file for the platform-specific bundled JRE archive")
set(WUWE_BUNDLED_JAVA_RELATIVE_PATH
    "${WUWE_DEFAULT_BUNDLED_JAVA_RELATIVE_PATH}"
    CACHE STRING "Java executable path relative to the bundled JRE root")

if(WUWE_INSTALL_BUNDLED_RUNTIME)
    if(NOT EXISTS "${WUWE_BUNDLED_TIKA_DIR}")
        message(FATAL_ERROR
            "Bundled Tika runtime directory not found: ${WUWE_BUNDLED_TIKA_DIR}. "
            "Disable WUWE_INSTALL_BUNDLED_RUNTIME only for non-release developer installs.")
    endif()
    install(
        DIRECTORY "${WUWE_BUNDLED_TIKA_DIR}/"
        DESTINATION runtime/tika
        FILES_MATCHING
            PATTERN "tika-server*.jar"
            PATTERN "*.sha1"
            PATTERN "README.md"
    )
    install(
        FILES
            "${CMAKE_SOURCE_DIR}/tools/runtime/start-tika.ps1"
            "${CMAKE_SOURCE_DIR}/tools/runtime/start-tika.sh"
        DESTINATION runtime/tika
    )

    if(NOT WUWE_BUNDLED_JRE_ARCHIVE STREQUAL "")
        if(NOT EXISTS "${WUWE_BUNDLED_JRE_ARCHIVE}")
            message(FATAL_ERROR
                "Bundled JRE archive not found: ${WUWE_BUNDLED_JRE_ARCHIVE}. "
                "Disable WUWE_INSTALL_BUNDLED_RUNTIME only for non-release developer installs.")
        endif()
        if(NOT EXISTS "${WUWE_BUNDLED_JRE_SHA256_FILE}")
            message(FATAL_ERROR
                "Bundled JRE checksum file not found: ${WUWE_BUNDLED_JRE_SHA256_FILE}")
        endif()
        file(STRINGS "${WUWE_BUNDLED_JRE_SHA256_FILE}"
            WUWE_BUNDLED_JRE_SHA256_LINE LIMIT_COUNT 1)
        string(REGEX MATCH "^[0-9A-Fa-f]+"
            WUWE_BUNDLED_JRE_EXPECTED_SHA256 "${WUWE_BUNDLED_JRE_SHA256_LINE}")
        string(LENGTH "${WUWE_BUNDLED_JRE_EXPECTED_SHA256}"
            WUWE_BUNDLED_JRE_EXPECTED_SHA256_LENGTH)
        if(NOT WUWE_BUNDLED_JRE_EXPECTED_SHA256_LENGTH EQUAL 64)
            message(FATAL_ERROR
                "Bundled JRE checksum file is invalid: ${WUWE_BUNDLED_JRE_SHA256_FILE}")
        endif()
        file(SHA256 "${WUWE_BUNDLED_JRE_ARCHIVE}" WUWE_BUNDLED_JRE_ACTUAL_SHA256)
        string(TOLOWER "${WUWE_BUNDLED_JRE_EXPECTED_SHA256}"
            WUWE_BUNDLED_JRE_EXPECTED_SHA256)
        string(TOLOWER "${WUWE_BUNDLED_JRE_ACTUAL_SHA256}"
            WUWE_BUNDLED_JRE_ACTUAL_SHA256)
        if(NOT "${WUWE_BUNDLED_JRE_EXPECTED_SHA256}" STREQUAL
               "${WUWE_BUNDLED_JRE_ACTUAL_SHA256}")
            message(FATAL_ERROR
                "Bundled JRE SHA-256 mismatch for ${WUWE_BUNDLED_JRE_ARCHIVE}: "
                "expected ${WUWE_BUNDLED_JRE_EXPECTED_SHA256}, "
                "found ${WUWE_BUNDLED_JRE_ACTUAL_SHA256}")
        endif()
        if(WUWE_BUNDLED_JAVA_RELATIVE_PATH STREQUAL "")
            message(FATAL_ERROR
                "WUWE_BUNDLED_JAVA_RELATIVE_PATH is required when a bundled JRE archive is configured.")
        endif()
        install(CODE "
            set(_archive [[${WUWE_BUNDLED_JRE_ARCHIVE}]])
            set(_java_relative_path [[${WUWE_BUNDLED_JAVA_RELATIVE_PATH}]])
            set(_destination \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/runtime/jre\")
            set(_extract_root [[${CMAKE_CURRENT_BINARY_DIR}/wuwe-install-jre]])
            file(REMOVE_RECURSE \"\${_destination}\")
            file(REMOVE_RECURSE \"\${_extract_root}\")
            file(MAKE_DIRECTORY \"\${_destination}\")
            file(MAKE_DIRECTORY \"\${_extract_root}\")
            file(ARCHIVE_EXTRACT INPUT \"\${_archive}\" DESTINATION \"\${_extract_root}\")
            file(GLOB _children LIST_DIRECTORIES true \"\${_extract_root}/*\")
            list(LENGTH _children _count)
            if(_count EQUAL 1 AND IS_DIRECTORY \"\${_children}\")
                set(_jre_root \"\${_children}\")
            else()
                set(_jre_root \"\${_extract_root}\")
            endif()
            if(NOT EXISTS \"\${_jre_root}/\${_java_relative_path}\")
                message(FATAL_ERROR
                    \"Bundled JRE archive does not contain \${_java_relative_path}: \${_archive}\")
            endif()
            file(COPY \"\${_jre_root}/\" DESTINATION \"\${_destination}\")
            file(REMOVE_RECURSE \"\${_extract_root}\")
        ")
    else()
        message(STATUS
            "No platform-specific bundled JRE is configured; installed Tika will use java from PATH")
    endif()
endif()

install(
    EXPORT ${PROJECT_NAME}-targets
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake
)

include(CMakePackageConfigHelpers)
set(LIB_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME})
configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/${PROJECT_NAME}-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake
    PATH_VARS LIB_INSTALL_DIR
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake
)

write_basic_package_version_file(
    "${PROJECT_NAME}-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}/cmake    
)
