#!/usr/bin/env bash

set -euo pipefail

configuration="Release"
build_dir="build-linux-vcpkg"
artifacts_dir="artifacts"
dist_dir="dist"
skip_build=false
keep_artifacts=false

usage() {
  cat <<'EOF'
Usage: tools/package-wuwe.sh [options]

Options:
  --configuration <Release|Debug>
  --build-dir <path>
  --artifacts-dir <path>
  --dist-dir <path>
  --skip-build
  --keep-artifacts
  --help
EOF
}

while (($# > 0)); do
  case "$1" in
    --configuration)
      configuration="$2"
      shift 2
      ;;
    --build-dir)
      build_dir="$2"
      shift 2
      ;;
    --artifacts-dir)
      artifacts_dir="$2"
      shift 2
      ;;
    --dist-dir)
      dist_dir="$2"
      shift 2
      ;;
    --skip-build)
      skip_build=true
      shift
      ;;
    --keep-artifacts)
      keep_artifacts=true
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$configuration" != "Release" && "$configuration" != "Debug" ]]; then
  echo "--configuration must be Release or Debug" >&2
  exit 2
fi
if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "This package entry point supports Linux x64 hosts only." >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
version="$(tr -d '\r\n' < "$repo_root/VERSION")"

resolve_repo_path() {
  local value="$1"
  if [[ "$value" = /* ]]; then
    realpath -m "$value"
  else
    realpath -m "$repo_root/$value"
  fi
}

build_path="$(resolve_repo_path "$build_dir")"
artifacts_path="$(resolve_repo_path "$artifacts_dir")"
dist_path="$(resolve_repo_path "$dist_dir")"
package_root="$artifacts_path/wuwe"
archive_path="$dist_path/wuwe-$version-linux-x64.tar.gz"
cache_path="$build_path/CMakeCache.txt"

case "$package_root" in
  "$repo_root"/*) ;;
  *)
    echo "Artifacts directory must stay inside the repository: $artifacts_path" >&2
    exit 1
    ;;
esac

if [[ ! -f "$cache_path" ]]; then
  echo "CMake cache not found: $cache_path" >&2
  exit 1
fi

cache_value() {
  local name="$1"
  local fallback="${2:-}"
  local value
  value="$(sed -n "s/^${name}:[^=]*=//p" "$cache_path" | head -n 1)"
  printf '%s' "${value:-$fallback}"
}

cmake_bool_json() {
  case "${1^^}" in
    1|ON|TRUE|YES|Y) printf 'true' ;;
    *) printf 'false' ;;
  esac
}

vcpkg_version() {
  local package_name="$1"
  local status_path="$build_path/vcpkg_installed/vcpkg/status"
  if [[ ! -f "$status_path" ]]; then
    printf 'unknown'
    return
  fi

  local value
  value="$(awk -v package_name="$package_name" '
    BEGIN { RS=""; FS="\n" }
    $0 ~ "(^|\\n)Package: " package_name "(\\n|$)" {
      version="unknown"; port="0"
      for (i=1; i<=NF; ++i) {
        if ($i ~ /^Version: /) { sub(/^Version: /, "", $i); version=$i }
        if ($i ~ /^Port-Version: /) { sub(/^Port-Version: /, "", $i); port=$i }
      }
      if (port != "" && port != "0") { print version "#" port }
      else { print version }
      exit
    }
  ' "$status_path")"
  printf '%s' "${value:-unknown}"
}

if [[ "$skip_build" != true ]]; then
  cmake --build "$build_path" --config "$configuration" --parallel 2
fi

mkdir -p "$artifacts_path" "$dist_path"
if [[ "$keep_artifacts" != true && -e "$package_root" ]]; then
  rm -rf -- "$package_root"
fi

cmake --install "$build_path" --config "$configuration" --prefix "$package_root"
cp -a "$repo_root/README.md" "$repo_root/LICENSE" "$repo_root/VERSION" "$package_root/"
cp -a "$repo_root/vcpkg.json" "$package_root/"
cp -a "$repo_root/docs" "$package_root/docs"
cp -a "$repo_root/examples" "$package_root/examples"

java_path="$package_root/runtime/jre/bin/java"
tika_path="$package_root/runtime/tika/tika-server-standard.jar"
jre_archive="$repo_root/third_party/runtime/jre/temurin-21-jre-linux-x64.tar.gz"
jre_checksum_path="$jre_archive.sha256"

if [[ ! -x "$java_path" ]]; then
  echo "Installed Linux JRE is missing or not executable: $java_path" >&2
  exit 1
fi
if [[ ! -f "$tika_path" ]]; then
  echo "Installed Tika runtime is missing: $tika_path" >&2
  exit 1
fi
if [[ ! -f "$jre_checksum_path" ]]; then
  echo "Linux JRE checksum is missing: $jre_checksum_path" >&2
  exit 1
fi

expected_jre_sha="$(awk 'NR == 1 { print tolower($1) }' "$jre_checksum_path")"
actual_jre_sha="$(sha256sum "$jre_archive" | awk '{ print $1 }')"
if [[ "$expected_jre_sha" != "$actual_jre_sha" ]]; then
  echo "Linux JRE SHA-256 mismatch: expected $expected_jre_sha, found $actual_jre_sha" >&2
  exit 1
fi

with_openssl="$(cmake_bool_json "$(cache_value WUWE_BUILT_WITH_OPENSSL OFF)")"
with_httplib_https="$(cmake_bool_json "$(cache_value WUWE_BUILT_WITH_HTTPLIB_HTTPS OFF)")"
with_sqlite="$(cmake_bool_json "$(cache_value WUWE_BUILT_WITH_SQLITE OFF)")"
sqlite_provider="$(cache_value WUWE_SQLITE_DEPENDENCY none)"
sqlite_search="disabled"
if [[ "$with_sqlite" == true ]]; then
  sqlite_search="persistent-linear-scan"
fi

openssl_version="not-linked"
if [[ "$with_openssl" == true ]]; then
  openssl_version="$(vcpkg_version openssl)"
  if [[ "$openssl_version" == "unknown" ]] && command -v openssl >/dev/null 2>&1; then
    openssl_version="$(openssl version | awk '{ print $2 }')"
  fi
fi

sqlite_version="not-linked"
if [[ "$with_sqlite" == true ]]; then
  sqlite_version="$(vcpkg_version sqlite3)"
  if [[ "$sqlite_version" == "unknown" ]] && command -v pkg-config >/dev/null 2>&1; then
    sqlite_version="$(pkg-config --modversion sqlite3 2>/dev/null || printf unknown)"
  fi
  sqlite_include_dir="$(cache_value SQLite3_INCLUDE_DIR '')"
  if [[ "$sqlite_version" == "unknown" && -f "$sqlite_include_dir/sqlite3.h" ]]; then
    sqlite_version="$(sed -n 's/^#define SQLITE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$sqlite_include_dir/sqlite3.h" | head -n 1)"
  fi
fi

vcpkg_baseline="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$repo_root/vcpkg.json" | head -n 1)"
tika_sha="$(sha256sum "$tika_path" | awk '{ print $1 }')"
generated_at="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"

cat > "$package_root/manifest.json" <<EOF
{
  "name": "wuwe",
  "version": "$version",
  "configuration": "$configuration",
  "platform": "linux-x64",
  "generated_at_utc": "$generated_at",
  "layout": {
    "include": "C++ headers",
    "lib": "C++ libraries and CMake package files",
    "docs": "documentation copied from repository docs",
    "examples": "example source files when present",
    "runtime": "bundled runtime sidecars"
  },
  "capabilities": {
    "http_backend": "$(cache_value WUWE_HTTP_BACKEND cpr)",
    "tls_backend": "$(cache_value WUWE_TLS_BACKEND_RESOLVED native)",
    "openssl_linked": $with_openssl,
    "httplib_https": $with_httplib_https,
    "sqlite_memory_store": $with_sqlite,
    "sqlite_knowledge_index": $with_sqlite,
    "sqlite_knowledge_search": "$sqlite_search"
  },
  "build_dependencies": {
    "vcpkg_baseline": "$vcpkg_baseline",
    "openssl": {
      "linked": $with_openssl,
      "version": "$openssl_version"
    },
    "sqlite3": {
      "linked": $with_sqlite,
      "provider": "$sqlite_provider",
      "version": "$sqlite_version"
    },
    "cpr_libcurl": {
      "source": "pinned-fetchcontent",
      "included_cmake_package": true
    }
  },
  "runtime": {
    "tika": {
      "jar": "runtime/tika/tika-server-standard.jar",
      "sha256": "$tika_sha",
      "internal_url": "http://127.0.0.1:9998"
    },
    "jre": {
      "bundled": true,
      "path": "runtime/jre",
      "source": "archive",
      "archive": "third_party/runtime/jre/temurin-21-jre-linux-x64.tar.gz",
      "sha256": "$actual_jre_sha"
    }
  }
}
EOF

checksum_path="$package_root/checksums.sha256"
(
  cd "$package_root"
  find . \( -type f -o -type l \) ! -path './checksums.sha256' -print0 |
    sort -z |
    xargs -0 sha256sum |
    sed 's#  \./#  #'
) > "$checksum_path"

rm -f -- "$archive_path"
tar -czf "$archive_path" -C "$package_root" .
echo "Created $archive_path"
