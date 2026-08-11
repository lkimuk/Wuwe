cmake_minimum_required(VERSION 3.20)

get_filename_component(WUWE_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(READ "${WUWE_REPOSITORY_ROOT}/VERSION" WUWE_RELEASE_VERSION)
string(STRIP "${WUWE_RELEASE_VERSION}" WUWE_RELEASE_VERSION)

if(NOT WUWE_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "VERSION must contain a stable semantic version (major.minor.patch), found: "
        "${WUWE_RELEASE_VERSION}")
endif()

string(REPLACE "." ";" WUWE_RELEASE_VERSION_COMPONENTS "${WUWE_RELEASE_VERSION}")
list(GET WUWE_RELEASE_VERSION_COMPONENTS 0 WUWE_RELEASE_VERSION_MAJOR)
list(GET WUWE_RELEASE_VERSION_COMPONENTS 1 WUWE_RELEASE_VERSION_MINOR)
list(GET WUWE_RELEASE_VERSION_COMPONENTS 2 WUWE_RELEASE_VERSION_PATCH)

function(wuwe_require_release_literal relative_path expected_literal)
    set(absolute_path "${WUWE_REPOSITORY_ROOT}/${relative_path}")
    if(NOT EXISTS "${absolute_path}")
        message(FATAL_ERROR "Release metadata file is missing: ${relative_path}")
    endif()
    file(READ "${absolute_path}" content)
    string(FIND "${content}" "${expected_literal}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "${relative_path} does not contain synchronized release metadata: "
            "${expected_literal}")
    endif()
endfunction()

wuwe_require_release_literal(
    "vcpkg.json" "\"version-string\": \"${WUWE_RELEASE_VERSION}\"")
wuwe_require_release_literal(
    "website/package.json" "\"version\": \"${WUWE_RELEASE_VERSION}\"")
wuwe_require_release_literal(
    "website/package-lock.json" "\"version\": \"${WUWE_RELEASE_VERSION}\"")
wuwe_require_release_literal(
    "README.md" "wuwe-${WUWE_RELEASE_VERSION}-windows-x64.zip")
wuwe_require_release_literal(
    "README.md" "wuwe-${WUWE_RELEASE_VERSION}-linux-x64.tar.gz")
wuwe_require_release_literal(
    "README.md" "wuwe-${WUWE_RELEASE_VERSION}-macos-arm64.tar.gz")
wuwe_require_release_literal(
    "RELEASE_NOTES.md" "# Wuwe v${WUWE_RELEASE_VERSION}")
wuwe_require_release_literal(
    "RELEASE_NOTES.md" "wuwe-${WUWE_RELEASE_VERSION}-windows-x64.zip")
wuwe_require_release_literal(
    "RELEASE_NOTES.md" "wuwe-${WUWE_RELEASE_VERSION}-linux-x64.tar.gz")
wuwe_require_release_literal(
    "RELEASE_NOTES.md" "wuwe-${WUWE_RELEASE_VERSION}-macos-arm64.tar.gz")
wuwe_require_release_literal(
    "CHANGELOG.md" "## [${WUWE_RELEASE_VERSION}]")
wuwe_require_release_literal(
    "CHANGELOG.md" "Installable Windows, Linux, and macOS SDK packages")
wuwe_require_release_literal(
    "CHANGELOG.md" "macOS 14+ on Apple Silicon is part of the ${WUWE_RELEASE_VERSION} certification matrix")
wuwe_require_release_literal(
    "docs/migration-1.0.md" "on Windows and macOS, using AppContainer")
wuwe_require_release_literal(
    "docs/migration-1.0.md" "Official Windows, Linux, and macOS packages")
wuwe_require_release_literal(
    "docs/security-governance.md" "The Windows and macOS restricted backends")
wuwe_require_release_literal(
    "tests/package_smoke/main.cpp" "framework_version != \"${WUWE_RELEASE_VERSION}\"")
wuwe_require_release_literal(
    "tests/package_smoke/main.cpp" "framework_version_major == ${WUWE_RELEASE_VERSION_MAJOR}")
wuwe_require_release_literal(
    "tests/package_smoke/main.cpp" "framework_version_minor == ${WUWE_RELEASE_VERSION_MINOR}")
wuwe_require_release_literal(
    "tests/package_smoke/main.cpp" "framework_version_patch == ${WUWE_RELEASE_VERSION_PATCH}")
wuwe_require_release_literal(
    "tests/source_subdirectory_smoke/main.cpp" "framework_version == \"${WUWE_RELEASE_VERSION}\"")
wuwe_require_release_literal(
    "tests/header_independence/version_header.cpp" "framework_version == \"${WUWE_RELEASE_VERSION}\"")
wuwe_require_release_literal(
    "src/CMakeLists.txt" "COMPATIBILITY SameMajorVersion")

message(STATUS "Wuwe release metadata is synchronized at ${WUWE_RELEASE_VERSION}")
