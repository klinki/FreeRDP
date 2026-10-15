# Disconnect-verifier regression

[Program.cs](Program.cs) executes the actual Microsoft SDK implementation loaded from a prepared suite. It does not duplicate the disconnect algorithm. Reflection creates the internal TLS transport queue and invokes the actual receive loop with controlled streams; sockets and network threads are unnecessary for these deterministic checks.

| Check | Patched SDK | Original SDK |
| --- | --- | --- |
| Ordinary received packet waits until the bounded deadline and remains buffered | Passed | Failed |
| Matching disconnect succeeds and removes the session | Passed | Passed |
| Ordinary packet followed by matching disconnect succeeds and preserves the packet | Passed | Failed |
| Unrelated decoder exception propagates with its original cause | Passed | Passed |
| Another session's disconnect does not satisfy the expectation | Passed | Passed |
| TLS stream EOF converts to an accepted disconnect | Passed | Passed |
| TLS stream connection reset converts to an accepted disconnect | Passed | Passed |

The patched run returns exit status 0, with seven checks passed. The original run deliberately returns status 1, with five passed and two failed. The original SDK throws `InvalidOperationException("Unknown object received in transport.")` for ordinary packets; its adapter previously converted that exception into a false pass. The repair waits for actual disconnect evidence instead, and removes that adapter catch. These checks exercise the SDK and transport conversion; the companion live tests exercise the adapter and original Microsoft test assertions end to end.

[Patched results](patched/result.json) · [patched log](patched/run.log) · [original results](original/result.json) · [original log](original/run.log)

To run against any prepared suite:

```sh
python3 run.py --suite-bin /path/to/prepared-suite/Bin --output /path/to/new-regression-results
```

The runner creates a fresh temporary .NET 8 project on each invocation and copies the exact selected assemblies into the runtime directory. It records their hashes, the program hash, logs, outcomes and process exit code. This prevents MSBuild reference-cache or copy behavior from silently substituting another assembly. No SDK production code is edited by this check. A .NET 8 SDK and prepared suite are required; no additional test package is used.

The 2026-10-15 validation used the patched assemblies from the drive batch and the original pinned prepared suite. Runtime dependencies, generated binaries and temporary build files are not committed.

## Preparation code review

The accompanying `prepare.py`, `test_preparation.py`, `profiles.suite_patches()` and runner provenance guard were independently reviewed. No actionable defect was found. All three preparation tests passed independently: patch bytes/path validation, rejecting stale assembly provenance, and exporting/patching pinned source without modifying the source checkout or including untracked files. This review does not replace the combined-suite build and full protocol requalification.
