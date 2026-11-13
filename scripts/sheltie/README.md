# Sheltie runtime releases

This fork publishes the FreeRDP SDL3 client that Sheltie bundles. Each fork
release tag (`v<upstream>-sheltie.N`, see [fork versioning](../../README.md#fork-versioning-and-upstream-updates))
builds, tests and publishes one runtime. Sheltie pins a release tag and its
checksums; it does not build FreeRDP.

## Workflow

`.github/workflows/sheltie-runtime.yml` runs on pull requests and pushes to
`main`, on fork release tags, and manually. On macOS 26 it:

1. Builds the runtime configuration with `BUILD_TESTING=ON` and runs every SDL
   client test plus the YUV codec tests (`build-engine.sh`).
2. Stages the relocatable client and the sample server, collects matching
   sources, records build provenance and completes the manifest.
3. Smoke-tests the staged client: bridge checks, the `release` loopback session
   suite against the sample server, and reverse mouse-wheel delivery.
4. Packages the assets below and uploads them as a workflow artifact.

For a release tag, the tag must be the exact release `sheltie-version.py`
derives for that commit, the commit must be on `main`, and the checkout must be
clean. A separate job then creates the GitHub Release. Release creation never
replaces an existing release or its assets.

## Release assets

| Asset | Contents |
| --- | --- |
| `runtime.tar.gz` | `Contents/` with `Helpers/sdl-freerdp`, bundled libraries, licenses and `Resources/runtime-manifest.json`; `build-provenance.json` at the root |
| `validation-runtime.tar.gz` | `Contents/` with the test-only `Helpers/sfreerdp-server`; `test-kit/` |
| `runtime-sources.tar.gz` | FreeRDP source at the release revision, dependency sources, recipes and receipts. Distribute it with any product that bundles the runtime. |
| `runtime-manifest.json` | Copy of the runtime manifest |
| `SHA256SUMS` | SHA-256 checksums of the four files above |

## Manifest

`runtime-manifest.json` contains:

- `engine_version`, `release_tag` (null for development builds), `engine_revision`
  and `engine_repository`.
- `upstream_version` and `upstream_base`: the FreeRDP release the fork is based on.
- `architecture` (`arm64`) and `minimum_macos`.
- `bridge`: `schema_version`, `protocol_version` and the sorted `capabilities`
  list reported by the built client's `/launcher-capabilities`.
- `dependencies`: bundled Homebrew formulae with versions, licenses and source
  URLs; `binaries`: every bundled Mach-O file with its source checksum.
- `engine_build_options`: relevant CMake cache values.

Consumers should require the capabilities they use from `bridge.capabilities`
instead of inferring them from versions.

## Test kit

`validation-runtime.tar.gz` ships `test-kit/` with the tools matching that
engine release, so consumers validate a packaged client with the same tests:

```sh
python3 test-kit/macos_bundle.py verify /path/to/App.app --target 26.0
python3 test-kit/bridge-smoke.py /path/to/sdl-freerdp
python3 test-kit/local-session-smoke.py /path/to/sdl-freerdp \
  --sample-server Contents/Helpers/sfreerdp-server --case connect_cancel \
  --diagnostics-dir /path/to/new/results
```

These command lines, the `--suite release` selection and the exit status
(0 = passed) are a compatibility contract with Sheltie. Keep them stable
across engine releases, or coordinate a change with a Sheltie update.
`test-kit/engine-version.json` records the version metadata of that build.

## Building locally

Install the Homebrew dependencies listed in the workflow, fetch upstream for
version calculation, then build and package from a clean, committed checkout:

```sh
git fetch --no-tags https://github.com/FreeRDP/FreeRDP.git +refs/heads/master:refs/remotes/upstream/master
MACOSX_DEPLOYMENT_TARGET=26.0 BUILD_SAMPLE_SERVER=ON BUILD_ENGINE_TESTS=ON \
  bash scripts/sheltie/build-engine.sh "$PWD"
bash scripts/sheltie/package-runtime.sh "$PWD" /path/to/new/output
python3 -m unittest discover -s scripts/sheltie/tests
```

Locally built runtimes use the installed Homebrew versions and are for testing;
published runtimes come from the workflow.

## Hosted rendering tools

`hosted-host.m` and `hosted-driver.py` exercise hosted rendering
(`client/SDL/SDL3/launcher-bridge.md`) without the launcher. The host stands in
for the launcher's windows: it starts the engine with the surface port and shows
the surfaces it receives. The driver answers the bridge prompts and plays a
script of bridge commands. Build the host as described at the top of
`hosted-host.m`, then:

```sh
python3 scripts/sheltie/hosted-driver.py build/launcher/client/SDL/SDL3/sdl-freerdp \
  build/launcher/server/Sample/sfreerdp-server build/launcher/hosted-host --log /tmp/hosted
```

Add `--target HOST --user NAME --password PASSWORD` for a real host instead of
the loopback sample server. These are development tools and are not packaged.
