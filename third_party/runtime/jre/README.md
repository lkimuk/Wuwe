# Temurin JRE Runtime

This directory stores the platform-specific JRE archives used by `wuwe`
packaging. Each package expands the matching archive into `runtime/jre` so
bundled Tika can start without requiring users to install Java separately.

Current artifacts:

| Platform | Archive | SHA-256 |
| --- | --- | --- |
| Windows x64 | `temurin-21-jre-windows-x64.zip` | `be26677aaa20b39a62edcaab4c8857a8b76673b0f45abc0b6143b142b62717e4` |
| Linux x64 | `temurin-21-jre-linux-x64.tar.gz` | `e5038aae3ca9ff670bc696496b0728dbd23d280026bad30291cb919221ecfdcb` |

Both archives use:

- Distribution: Eclipse Temurin
- Java line: 21 LTS
- Version: `jdk-21.0.11+10-jre`
- Windows source:
  `https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jre_x64_windows_hotspot_21.0.11_10.zip`
- Linux source:
  `https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jre_x64_linux_hotspot_21.0.11_10.tar.gz`

JRE binaries are operating-system and architecture specific even though the
Tika JAR is shared. Do not substitute one platform archive for another or
replace these files with an unpinned download. Update this README and the
matching `.sha256` file together when changing either artifact.
