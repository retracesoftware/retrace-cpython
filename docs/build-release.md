# Build And Release

This repository builds CPython from upstream release tags, applies Retrace
patches, and packages the resulting runtime artifacts.

## Local Patch And Build

Apply the patch stack to a clean upstream CPython checkout:

```bash
scripts/apply-patches 3.12.13
```

Build and install a patched interpreter:

```bash
scripts/build-release 3.12.13
```

Package an installed interpreter:

```bash
scripts/package 3.12.13
```

By default:

- patched sources go under `build/src/`
- installed interpreters go under `build/install/`
- release archives go under `build/dist/`

Set `CPYTHON_REPO_URL` to use a CPython mirror or fork instead of
`https://github.com/python/cpython.git`.

## Documentation Build

Install the docs dependencies:

```bash
python3 -m pip install -r requirements-docs.txt
```

Build the MkDocs site and refresh AI-readable docs:

```bash
python3 scripts/build-docs
```

The docs build writes the MkDocs site to `build/site/` and refreshes repository
root `llms.txt` and `llms-full.txt` from the generated MkDocs output.

The GitHub release workflow runs the same docs build before wheel packaging and
passes the generated site directory into `scripts/package-wheel`, which copies
the LLM-readable files into the wheel package.

## Wheel Packaging

The GitHub workflow builds platform wheels for the `retracesoftware-cpython`
PyPI project. Wheels contain the minimal runtime overlay plus the
`retrace-python` launcher.

To build a wheel from an already packaged runtime:

```bash
scripts/package-wheel \
  3.12.13 \
  "$(scripts/package-version)" \
  macos-arm64 \
  build/package/retrace-cpython-3.12.13-macos-arm64 \
  --docs-dir build/site
```

`--docs-dir` should point at the MkDocs output directory containing `llms.txt`
and `llms-full.txt`.

## Release Workflow

Release versions live in the tracked `VERSION` file. To release:

1. Update `VERSION`.
2. Commit the release tree.
3. Create and push a tag such as `v0.4.3`.
4. Run the GitHub workflow against that tag.

The workflow checks out the tag before reading patch manifests or building
wheels, so later platform builds can be run retroactively against the same
release tree.

Built wheels are uploaded as GitHub Release assets. Uploads are additive: if a
wheel with the same filename already exists on the release, the workflow leaves
it alone. PyPI publishing downloads wheel assets from the GitHub Release and
uses `skip-existing`, so rerunning publish is also additive.

Important workflow inputs:

- `release_tag=v0.4.3` builds from that Git tag.
- `python_version=manifest-all` builds every verified CPython release.
- `python_version=manifest-latest` builds the latest verified release per
  series.
- `python_version=3.12.13` builds one exact CPython release.
- `target=all` builds every supported platform.
- `target=macos-arm64` builds only that platform.
- `target=none` skips builds and only publishes existing GitHub Release wheel
  assets when `publish_pypi=true`.
- `package_version=0.4.3` overrides the version read from `VERSION`.
- `skip_tests` skips CPython test-suite runs for faster smoke publishing.
- `upload_release_assets` uploads missing wheel assets to the GitHub Release.
- `publish_pypi` opts into PyPI Trusted Publishing from GitHub Release assets.
