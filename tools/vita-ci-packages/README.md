# Pinned Vita CI packages

These VitaSDK package archives match the dependency set used by the
hardware-validated local Release build:

| Package | SHA-256 |
| --- | --- |
| `libjpeg-turbo-3.2.0-1-vita.pkg.tar.xz` | `8b4b0328c16362b006e8eeae7a6c6400558d5a6de9ccb776bf055d827533eaa7` |
| `libvita2d-0.0.0.r188.ga8f15ab-1-vita.pkg.tar.xz` | `d604b050e6e445e4019258a75806f24fb325e8abb8dfdc48f6c1fbaa7346fd43` |
| `taihen-0.11-1-vita.pkg.tar.xz` | `93d2075b6b61a164224a2aad651d3d6787f31f3a19c3e2bada7c99844f9fae90` |

They were produced by VitaSDK's
[`vitasdk-autobuild` run 32871304465](https://github.com/vitasdk/vitasdk-autobuild/actions/runs/32871304465)
on 2026-08-25. The archives retain their upstream package metadata,
documentation, and license files.

Keeping these small target packages in the repository prevents the CI build
from silently changing when the mutable `vitasdk/packages` `master` release is
replaced. Update them only after a newly pinned set passes hardware startup and
gameplay validation.
