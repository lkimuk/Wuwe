# Temurin JRE Runtime

This directory stores the pinned Windows x64 JRE archive used by `wuwe`
packaging. The package expands this archive into `runtime/jre` so bundled
Tika can start without requiring users to install Java separately.

Current artifact:

- Distribution: Eclipse Temurin
- Java line: 21 LTS
- Version: `jdk-21.0.11+10-jre`
- Platform: Windows x64
- Packaged filename: `temurin-21-jre-windows-x64.zip`
- Source URL:
  `https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jre/hotspot/normal/eclipse?project=jdk`
- Local SHA-256:
  `be26677aaa20b39a62edcaab4c8857a8b76673b0f45abc0b6143b142b62717e4`

Do not replace this archive with an unpinned download. Update this README and
`temurin-21-jre-windows-x64.zip.sha256` together when changing the JRE version.
