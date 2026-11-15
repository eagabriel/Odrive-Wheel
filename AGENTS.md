# Repository release rules

- Before publishing a new release or tag, preserve the exact HTML configurator
  from the previous published release in
  `Odrive-Wheel/tools/html-archive/odrive-wheel_<previous-tag>.html`.
- Read that HTML from the previous Git tag, not from the current working copy.
  If the archive already exists, verify it matches the tagged file; never
  overwrite an existing historical archive with a newer configurator.
- Commit the archive alongside the new release. Keep older archives.
- Increment the current HTML revision when preparing a release. Do not bump it
  for intermediate local changes when the user asks to keep the revision.
- Keep English and Portuguese release notes in `docs/releases/<tag>.md` and
  `docs/releases/<tag>_PT.md`, describing changes relevant to users.
- Never commit `docs/private/`, build outputs, local test toolchains, or
  unrelated personal media. Respect the repository's `.gitignore`.
- Before pushing a release tag, validate the firmware build and relevant tests,
  and confirm that the tag points to the exact commit being shipped.
