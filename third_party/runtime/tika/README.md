# Apache Tika Runtime

This directory stores the pinned Tika Server runtime used by `wuwe`
packaging.

Current artifact:

- Maven group: `org.apache.tika`
- Maven artifact: `tika-server-standard`
- Version: `3.3.1`
- Packaged filename: `tika-server-standard.jar`
- Source URL:
  `https://repo1.maven.org/maven2/org/apache/tika/tika-server-standard/3.3.1/tika-server-standard-3.3.1.jar`
- Maven SHA-1:
  `437fbc6e3ace5de51c01865cb9e18619bf7df136`
- Local SHA-256:
  `755d252de43a1995151db3a25c825332d2f27371272c41459bb5b78e21b028bd`

`tools/package-wuwe.ps1` uses this jar by default when building `wuwe` and
validates `tika-server-standard.jar.sha1` when the checksum file is present.

Do not replace this jar with an unpinned download. Update this README and the
checksum file together when changing Tika versions.
