# Commit messages

Use the `[bridge,sdl]` prefix for non-merge commits related to the native
launcher bridge, including discovery, protocol messages, lifecycle
handling, and per-session options.

Include a body explaining the behavior, relevant defaults and compatibility,
and the reason for non-obvious implementation choices. Describe regression
coverage and validation actually performed when relevant.

# Branch integration

Count commits introduced by the source branch relative to the target
branch (`target..source`), excluding their shared history.

- For more than one introduced commit, use `git merge --no-ff` and
  preserve the individual branch commits.
- For one introduced commit, prefer `git merge --ff-only`.
  Rebase that commit onto the target first when necessary.
- Use `Merge: Description` with a capitalized imperative description,
  for example: `Merge: Add session performance monitoring`.
- Summarize the combined behavior, defaults, compatibility and
  validation actually performed in the merge commit body.
- Keep individual commit messages intact during integration.
