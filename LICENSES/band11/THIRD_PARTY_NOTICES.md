# Third-party components

External contributions remain unconfirmed. Moving files, process separation, Git author labels and JavaScript imports do not establish relicensing rights. Earlier blanket ownership/license assertions are withdrawn. Preserve original notices and publish uncertain source for review; commercial clearance is pending.

| Component | Use | License / handling |
| --- | --- | --- |
| Golden Layout 2.6.0 | Vendored docking UI | MIT; include `web-ui/vendor/golden-layout/LICENSE` and local modification notes |
| aiohttp and its dependencies | Installed HTTP/WebSocket runtime | Preserve the licenses/notices from the installed distributions when bundling |
| cryptography and its dependencies / bundled OpenSSL | Optional synthetic BLE SMP implementation | Preserve its Apache/BSD and bundled-library notices; installed via requirements |
| QEMU | Separate executable | GPL, with per-file upstream exceptions; supply complete corresponding source when distributing |
| GDB | Separate debugger executable | Its own GPL; public in-process integration scripts retain original notices; their licensing/derivation needs review |

`golden-layout-provenance.json` records the npm source and local modifications.
The browser import graph has no external CDN or additional vendored JS package.
Local application module ownership remains under review. An `import` statement alone
does not determine licensing; the actual dependency license does.

The Python environment is installed separately. A packaged application that
bundles Python or wheels must include their supplied license directories and
notices, including transitive/native dependencies. `requirements.lock` records
the environment used for verification; it is not permission to omit notices.

AstroBox-NG is cited as a protocol interoperability reference in the resource
documentation. `resource_protocol.py` implements wire-format primitives with
Python's standard library; no AstroBox runtime or source bundle is included.
Protocol references are retained and must not be mistaken for a license grant
to copy additional upstream implementations in future changes.

Firmware, fonts, watchfaces and application packages remain separately supplied
by the user. No right to redistribute those assets is granted here.
