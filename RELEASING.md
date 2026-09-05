# Publishing tmplay on GitHub

The repository contains the source and build scripts. Local dependencies,
models, build directories, test fixtures and release applications are not
published in the GitHub source archive.

To build the optional smoke tests in a development checkout, configure with
`-DTPLAY_BUILD_TESTS=ON`. They are deliberately disabled for release builds.

1. Build the Universal 2 application:

   ```bash
   ./scripts/build_macos_universal.sh
   ./scripts/audit_macos_bundle.sh dist/tmplay-universal2/tmplay.app
   ```

2. Commit the release source and create a tag.

3. Create the compact source archive:

   ```bash
   ./scripts/create_github_source_archive.sh 1.0.0
   ```

Upload both `dist/tmplay-universal2-macos.zip` and
`dist/tmplay-1.0.0-source.zip` to the GitHub Release. The former is the
ready-to-run app; the latter contains only source, resources and build files.
