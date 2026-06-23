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

option(WUWE_ENABLE_OPENSSL "Enable OpenSSL-backed HTTPS support when OpenSSL is available" ON)
if(WUWE_ENABLE_OPENSSL)
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
    find_package(OpenSSL QUIET)
endif()

if(OpenSSL_FOUND)
    set(CPR_FORCE_OPENSSL_BACKEND ON CACHE BOOL "Use OpenSSL as cpr/curl TLS backend" FORCE)
    set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL for libcurl HTTPS support" FORCE)
    set(CURL_USE_SCHANNEL OFF CACHE BOOL "Disable Schannel when OpenSSL is selected" FORCE)
    message(STATUS "OpenSSL detected: cpr/libcurl and cpp-httplib HTTPS support enabled")
else()
    message(STATUS "OpenSSL not found: cpr/libcurl will use its platform TLS backend")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build curl static libraries" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "Disable libpsl to avoid meson dependency on Windows" FORCE)
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
if(WUWE_ENABLE_HTTPLIB_SSL AND OpenSSL_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "cpp-httplib HTTPS support enabled")
elseif(WUWE_ENABLE_HTTPLIB_SSL)
    message(STATUS "cpp-httplib HTTPS support disabled: OpenSSL not found")
endif()

option(WUWE_ENABLE_SQLITE "Enable SQLite-backed memory store when SQLite3 is available" ON)
if(WUWE_ENABLE_SQLITE)
    find_package(SQLite3 QUIET)
    if(NOT SQLite3_FOUND)
        find_package(unofficial-sqlite3 CONFIG QUIET)
    endif()
endif()

if(WUWE_ENABLE_SQLITE AND SQLite3_FOUND)
    target_link_libraries(${PROJECT_NAME} PUBLIC SQLite::SQLite3)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WUWE_HAS_SQLITE=1)
    message(STATUS "SQLite memory store enabled")
elseif(WUWE_ENABLE_SQLITE AND unofficial-sqlite3_FOUND)
    target_link_libraries(${PROJECT_NAME} PUBLIC unofficial::sqlite3::sqlite3)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WUWE_HAS_SQLITE=1)
    message(STATUS "SQLite memory store enabled")
else()
    target_compile_definitions(${PROJECT_NAME} PUBLIC WUWE_HAS_SQLITE=0)
    message(STATUS "SQLite memory store disabled")
endif()

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
set(WUWE_BUNDLED_JRE_ARCHIVE
    "${CMAKE_SOURCE_DIR}/third_party/runtime/jre/temurin-21-jre-windows-x64.zip"
    CACHE FILEPATH "Bundled JRE archive")

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

    if(WIN32)
        if(NOT EXISTS "${WUWE_BUNDLED_JRE_ARCHIVE}")
            message(FATAL_ERROR
                "Bundled JRE archive not found: ${WUWE_BUNDLED_JRE_ARCHIVE}. "
                "Disable WUWE_INSTALL_BUNDLED_RUNTIME only for non-release developer installs.")
        endif()
        install(CODE "
            set(_archive [[${WUWE_BUNDLED_JRE_ARCHIVE}]])
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
            file(COPY \"\${_jre_root}/\" DESTINATION \"\${_destination}\")
            file(REMOVE_RECURSE \"\${_extract_root}\")
        ")
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
