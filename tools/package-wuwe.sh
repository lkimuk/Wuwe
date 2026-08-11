#!/usr/bin/env bash

set -euo pipefail

configuration="Release"
build_dir=""
artifacts_dir="artifacts"
dist_dir="dist"
skip_build=false
keep_artifacts=false
include_tika=true
include_jre=true

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
  --without-tika
  --without-jre
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
    --without-tika)
      include_tika=false
      shift
      ;;
    --without-jre)
      include_jre=false
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
host_os="$(uname -s)"
host_arch="$(uname -m)"
case "$host_os:$host_arch" in
  Linux:x86_64)
    platform="linux-x64"
    default_build_dir="build-linux-vcpkg"
    jre_archive_name="temurin-21-jre-linux-x64.tar.gz"
    ;;
  Darwin:arm64)
    platform="macos-arm64"
    default_build_dir="build-macos-arm64-vcpkg"
    jre_archive_name="temurin-21-jre-macos-aarch64.tar.gz"
    ;;
  *)
    echo "This package entry point supports Linux x64 and macOS arm64 hosts only." >&2
    exit 1
    ;;
esac
build_dir="${build_dir:-$default_build_dir}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
version="$(tr -d '\r\n' < "$repo_root/VERSION")"

file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{ print $1 }'
  else
    shasum -a 256 "$1" | awk '{ print $1 }'
  fi
}

resolve_repo_path() {
  local value="$1"
  if [[ "$value" = /* ]]; then
    perl -MCwd=abs_path -e 'print abs_path($ARGV[0])' "$value"
  else
    perl -MCwd=abs_path -e 'print abs_path($ARGV[0])' "$repo_root/$value"
  fi
}

build_path="$(resolve_repo_path "$build_dir")"
artifacts_path="$(resolve_repo_path "$artifacts_dir")"
dist_path="$(resolve_repo_path "$dist_dir")"
package_root="$artifacts_path/wuwe"
archive_path="$dist_path/wuwe-$version-$platform.tar.gz"
archive_checksum_path="$archive_path.sha256"
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
  case "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')" in
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

# Static curl records the vcpkg library search directory in libcurl.pc. That
# build-tree path must not escape into a relocatable SDK; OpenSSL remains an
# explicitly declared consumer dependency and is resolved by name.
libcurl_pc="$package_root/lib/pkgconfig/libcurl.pc"
if [[ -f "$libcurl_pc" ]]; then
  perl -pi -e 's{[ ]*-L/[^ \n]*/vcpkg_installed/[^ \n]*/lib}{}g' "$libcurl_pc"
fi

leaked_package_metadata="$(find "$package_root/lib" -type f \( -name '*.cmake' -o -name '*.pc' \) \
  -exec grep -lF "$repo_root" {} + || true)"
if [[ -n "$leaked_package_metadata" ]]; then
  echo "Installed package metadata contains build-tree paths:" >&2
  printf '%s\n' "$leaked_package_metadata" >&2
  exit 1
fi

cp -a "$repo_root/README.md" "$repo_root/CHANGELOG.md" "$repo_root/LICENSE" "$repo_root/VERSION" "$package_root/"
cp -a "$repo_root/vcpkg.json" "$package_root/"
cp -a "$repo_root/docs" "$package_root/docs"
cp -a "$repo_root/examples" "$package_root/examples"

if [[ "$platform" == "macos-arm64" ]]; then
  java_path="$package_root/runtime/jre/Contents/Home/bin/java"
else
  java_path="$package_root/runtime/jre/bin/java"
fi
tika_path="$package_root/runtime/tika/tika-server-standard.jar"
jre_archive="$repo_root/third_party/runtime/jre/$jre_archive_name"
jre_checksum_path="$jre_archive.sha256"

if [[ "$include_tika" != true ]]; then
  rm -rf -- "$package_root/runtime/tika"
fi
if [[ "$include_jre" != true ]]; then
  rm -rf -- "$package_root/runtime/jre"
fi

if [[ "$include_jre" == true && ! -x "$java_path" ]]; then
  echo "Installed $platform JRE is missing or not executable: $java_path" >&2
  exit 1
fi
if [[ "$include_tika" == true && ! -f "$tika_path" ]]; then
  echo "Installed Tika runtime is missing: $tika_path" >&2
  exit 1
fi
if [[ "$include_jre" == true && ! -f "$jre_checksum_path" ]]; then
  echo "$platform JRE checksum is missing: $jre_checksum_path" >&2
  exit 1
fi

actual_jre_sha=""
if [[ "$include_jre" == true ]]; then
  expected_jre_sha="$(awk 'NR == 1 { print tolower($1) }' "$jre_checksum_path")"
  actual_jre_sha="$(file_sha256 "$jre_archive")"
  if [[ "$expected_jre_sha" != "$actual_jre_sha" ]]; then
    echo "$platform JRE SHA-256 mismatch: expected $expected_jre_sha, found $actual_jre_sha" >&2
    exit 1
  fi
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
tika_sha=""
tika_manifest_path=""
jre_manifest_path=""
jre_manifest_source=""
jre_manifest_archive=""
if [[ "$include_tika" == true ]]; then
  tika_sha="$(file_sha256 "$tika_path")"
  tika_manifest_path="runtime/tika/tika-server-standard.jar"
fi
if [[ "$include_jre" == true ]]; then
  jre_manifest_path="runtime/jre"
  jre_manifest_source="archive"
  jre_manifest_archive="third_party/runtime/jre/$jre_archive_name"
fi
generated_at="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"

cat > "$package_root/manifest.json" <<EOF
{
  "name": "wuwe",
  "version": "$version",
  "configuration": "$configuration",
  "platform": "$platform",
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
    "target_triplet": "$(cache_value VCPKG_TARGET_TRIPLET unknown)",
    "macos_deployment_target": "$(cache_value CMAKE_OSX_DEPLOYMENT_TARGET '')",
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
      "bundled": $include_tika,
      "jar": "$tika_manifest_path",
      "sha256": "$tika_sha",
      "internal_url": "http://127.0.0.1:9998"
    },
    "jre": {
      "bundled": $include_jre,
      "path": "$jre_manifest_path",
      "source": "$jre_manifest_source",
      "archive": "$jre_manifest_archive",
      "sha256": "$actual_jre_sha"
    }
  }
}
EOF

checksum_path="$package_root/checksums.sha256"
: > "$checksum_path"
while IFS= read -r relative_path; do
  printf '%s  %s\n' "$(file_sha256 "$package_root/$relative_path")" "$relative_path" \
    >> "$checksum_path"
done < <(cd "$package_root" && find . -type f ! -path './checksums.sha256' -print | \
  LC_ALL=C sort | sed 's#^\./##')

rm -f -- "$archive_path"
tar -czf "$archive_path" -C "$package_root" .
printf '%s  %s\n' "$(file_sha256 "$archive_path")" "$(basename "$archive_path")" \
  > "$archive_checksum_path"
echo "Created $archive_path"
echo "Created $archive_checksum_path"
