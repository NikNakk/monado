# GitHub-only CI

This branch contains Actions configuration on top of the upstream base. It is
published only to GitHub, not the GitLab fork, and is not part of the Monado
submission branch.

Keep the GitHub PR from `macos-inproc-mvk` into `github-ci` open as a CI harness;
it is not intended to be merged. Source pushes and PR edits trigger the checks.
Jobs check out `github.event.pull_request.head.sha`, so they build the exact
submission commit, without adding these workflow files to that branch.

Builds and tests cover Ubuntu 24.04, macOS 15 arm64 and Windows Server 2022 x64.
Windows uses Visual Studio 2022 and the submission’s vcpkg manifest. macOS core tests and the GPU-labelled
Metal interop test run as separate steps. An unavailable GPU/driver is reported
as skipped, while unexpected initialization or interop failures fail the job.
Hosted runner results do not establish headset presentation or cadence.

No default branch change is needed for PR checks. There is no manual-dispatch
trigger because this workflow is not on the repository's default branch.
When changing this branch's workflow, refresh the harness PR base by retargeting
it to `main` and back to `github-ci`, then edit its title or description to trigger
a fresh run. GitHub can otherwise retain the old base SHA. Keep source changes
on the submission branch.

Each run retains JUnit results, CTest logs and the CMake configuration for seven
days. Actions are pinned to commit SHAs; update the pins explicitly as needed.

Contribution checks cover changed-line clang-format, changed CMake files, added-text
spelling, whitespace, REUSE licensing, numbered changelog fragments and author DCO
sign-offs. Helpers are read from the harness branch; source and commit checks use
the submission branch and its merge base. Missing sign-offs fail until the author
certifies and pushes the commits. Tool versions are pinned in the workflow.

clang-format is pinned to 14.0.6 to match the GitLab Bookworm style job. Use
that version locally too; newer releases can produce different formatting.
