# GitHub-only CI

This branch contains Actions configuration on top of the upstream base. It is
published only to GitHub, not the GitLab fork, and is not part of the Monado
submission branch.

Keep the GitHub PR from `macos-inproc-mvk` into `github-ci` open as a CI harness;
it is not intended to be merged. Source pushes and PR edits trigger the checks.
Jobs check out `github.event.pull_request.head.sha`, so they build the exact
submission commit, without adding these workflow files to that branch.

Checks cover Ubuntu 24.04 and macOS 15 arm64. macOS core tests and the GPU-labelled
Metal interop test run as separate steps. An unavailable GPU/driver is reported
as skipped, while unexpected initialization or interop failures fail the job.
Hosted runner results do not establish headset presentation or cadence.

No default branch change is needed for PR checks. There is no manual-dispatch
trigger because this workflow is not on the repository's default branch.
When changing this branch's workflow, edit the harness PR description to trigger
a fresh run using the new workflow. Keep source changes on the submission branch.

Each run retains JUnit results, CTest logs and the CMake configuration for seven
days. Actions are pinned to commit SHAs; update the pins explicitly as needed.
