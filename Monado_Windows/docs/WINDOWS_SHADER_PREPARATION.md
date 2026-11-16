# Native shader preparation

Shader preparation is an explicit launcher/build option. Play never starts it
automatically and preparation never launches the game. The native child scans
the selected read-only RomFS, finishes discovery before starting compile workers,
and builds supported shader guesses and this profile's locally recorded variants.
New/unsupported runtime variants can still compile during play. No game-derived
seed, old completion marker or permanent crash blacklist is imported.

The launcher and portable setup supervise their own child and show measured
phase/counts. Complete requires that child's successful exit, its exact fresh
session and executable identity, joined workers, and successful SPIR-V/pipeline
cache commits. Partial, failed and cancelled runs are distinct; useful partial
cache work remains available. Explicit Retry starts a new session. Cancellation
requests the current session first, then the supervisor may stop its owned job;
it never closes a human game or unrelated process.

The source CLI uses the same protocol:

```powershell
$runtime = 'C:/My installation/runtime'
$profile = 'C:/My stable profile'
$exe = Join-Path $runtime 'build/xcde.exe'
$identity = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant()
$session = [Guid]::NewGuid().ToString('N')
Push-Location -LiteralPath $profile
try { & $exe --precompile (Join-Path $profile 'targets') --session $session --identity $identity }
finally { Pop-Location }
```

`cache/precompile_work.txt` is bounded ASCII protocol version1 with the current
session, runtime executable SHA256, scan/build/seed/pipelines/done phase, outcome,
actual attempted/built/failed/unsupported/error counts and Vulkan device/cache
identity. Completion is `phase done`, `state complete` and child exit0 together.
A copied/stale status file establishes no current completion. Cancellation is an
exclusive new `cache/precompile_cancel-SESSION.txt`, containing exactly:

```text
monado_precompile_cancel 1
session SESSION
```

Cache/status operations use native Unicode extended paths, retained ordinary
ancestor handles, current-user ownership and single-link checks. Reparse points,
hardlinks and unrecognized status/variant/pipeline-cache files are preserved on
failure. Writes flush the new file and use a write-through replacement under a
cooperating-writer mutex; this is Windows persistence, not POSIX directory fsync
or a same-user malicious-writer compare-and-swap guarantee. SPIR-V records are
bounded and structurally checked before reflection/driver use; malformed cache
records are misses and may be rebuilt. Successful compilation still supplies
the shader to a normal draw even if storage fails. Preparation reports those
storage errors instead of declaring completion.

Authored native tests cover real shaderc/pthreads/Win32 storage, malformed and
compressed synthetic containers, metadata initialization, cancellation/session
matching, partial/fatal outcomes, cache repair, sparse offsets beyond4GiB,
write/flush/replace/lock failures, unknown-file retention, CJK/literal paths and
paths beyond260 characters. They use disposable data and an authored pipeline
query boundary. They do not qualify game-container coverage, GPU compilation,
driver performance or every runtime variant.
