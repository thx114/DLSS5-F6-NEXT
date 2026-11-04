# Low workspace preparation

`scripts/Prepare-LowWorkspace.ps1` prepares a bounded handoff for a high worker.
It records the coordinator (`/root`), task name, repository and base commit,
the current dirty boundary, explicit source provenance, owned files and SHA256
hashes, and the build command supplied by the coordinator.

Use `-Mode dry-run` first. It performs no writes. Use `-Mode prepare` only after
the dry-run is reviewable; it creates or reuses `worktrees/<TaskName>` at the
resolved `BaseRef` and writes `.low-workspace.json` there. Reuse requires the
existing marker to match repository, task, and base commit; conflicts fail
closed. `-Mode validate` checks the marker and current worktree status without
altering source or deployment.

`SourceRoot` is explicit because the Git HEAD may not contain the deployed or
ignored source. An external or ignored source is reported as
`external-or-ignored-source`; this is a provenance gap until the coordinator
supplies a hash or patch/receipt. The script never reset, checkout, stash,
delete, copy a source tree, modify configuration, or touch deployment files.

Reuse and validation compare the marker with the actual worktree `HEAD`, Git
common directory, and branch/detached state. The marker is a receipt, not the
authority. Owned files must be relative paths that resolve inside SourceRoot;
absolute paths, drive-rooted paths, and `..` escapes are rejected.

High workers should consume the manifest, check only its baseline, owned files,
and local constraints, then proceed with their bounded task. They should not
repeat a full repository review or create another worktree unless the manifest
is invalid or a new evidence question requires it.
