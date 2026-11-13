# CS2 Replay Lab: self-hosted service proposal

**Status: a gated guest beta is implemented.** See `..\README.md` for current
startup, security gates, tested limits and live validation. `web\` and the
native `csda-service` are the live implementation. The design draft below is
historical, not the deployed configuration: the beta uses a 512 MiB upload cap,
dynamic bounded worker commit, SQLite continuity and the existing C++ analysis
library inside AppContainer workers; accounts/MongoDB history remain deferred.
Player selection must still be active when its request is committed. Acceptance
starts a bounded one-hour analysis queue/processing deadline, separate from
selection and completed-report retention. Session cookies match exact field
names and ambiguous duplicates are denied. Login histories expire before new-IP
capacity checks; request socket I/O runs outside the shared state mutex.

The current browser invalidates cancelled job requests and uses bounded polling
recovery with an explicit **Retry status** action. Receipts count completed
result/report GET bodies only (not HEAD or interrupted transfers).
`tests\service_tests.cpp` and `tests\web_app_tests.js` cover these paths offline;
see the README for the scoped commands that do not use live credentials or
create containment profiles. The historical proposal below is unchanged.

Open `service-mockup.html` beside this document for the original
interactive static GUI. It reads file metadata only; it cannot upload, decode or
analyze a demo. All progress and results are explicitly synthetic.

## Product

The inverse of ClipShare's outbound delivery: users send a replay to an isolated
temporary intake area, not into a public view of the operator's SSD. They get an
owner-only analysis page and downloadable artifacts, then the job expires.
No phone application, Steam credentials, account scraping, paid AI/search API,
GPU model or automatic cheating verdict is required.

The first release accepts one decompressed CS2 `.dem` per job. It derives a roster
from the replay, lets the user pick a player, and reports observed combat totals,
round context, weapons and source-bound review markers. It does not accept ZIPs,
arbitrary file types, directory paths or remote URLs. Batch matches, supplied
JSON, position exports, provider acquisition and cross-user deduplication are out
of scope for the first internet-facing cut.

## The visible flow

| Stage | User sees | Actual service work |
|---|---|---|
| Intake | File, bytes transferred, permission/retention notice | Authenticate; reserve bounded capacity; stream to a random exclusive job file |
| Queued | Real queue position; cancel button | Seal and hash complete bytes; store durable state; never guess a completion time |
| Decode | Stage and elapsed time; not fake event percentages | Validate supported demo framing; run the pinned CSDA binary in isolation |
| Select | Players actually found in this demo | Return a bounded roster; accept only a SteamID present in that job |
| Analyze | Stage and any actionable failure | Run the existing native C++ analyzer with fixed arguments |
| Ready | Inline metrics/events, downloads, precise expiry | Confirm complete manifest/output hashes; publish owner-only artifacts |
| Cancel/expire | Deleted, or explicit deletion-pending/error | Stop only the job's child tree, then remove that job's resolved files |

Upload progress is measurable. The current CSDA CLI does not provide a qualified
percentage/ETA contract, so decode/analysis show real stages and elapsed time.
"As soon as possible" means begin immediately when the one worker is idle; it
is not a promise of immediate results for large or queued demos.

## Recommended implementation

```text
browser
  -> HTTPS ingress (body/time limits; no directory share)
  -> native C++ service (auth, intake, durable bounded queue, status)
  -> restricted job worker
       pinned csda.exe -> sealed CSDA JSON -> cs2-analyzer.exe
  -> checked manifest + bounded analysis.json -> owner-only website / downloads
  -> expiry/cancel cleanup
```

One native service, one serial CPU execution slot, and the two existing CLI
children. No Node production backend, agent graph or model orchestration.
Use a conventional static HTML/CSS/JS website. The service must not use the
GodBrain kernel's privileged commands, personal vault, model slot or ClipShare
storage roots. A reverse proxy is the HTTPS boundary, not an application wrapper.
Library/server selection remains an implementation decision, not an instruction
to install another framework for this draft.

Reuse the analyzer's tested calculation/report logic, with the unchanged pinned
CSDA v1.11.0 release/hash contract (`tools\csda.lock.json`) unless protocol tests
justify a reviewed upgrade. Do not invoke `Invoke-CS2Analysis.ps1` as an arbitrary
public shell bridge: the service must use fixed native argument vectors,
owner-scoped storage, exact-child timeouts and a job-scoped cache.

The existing CLI needs `--steam-id` up front. The service's new decode/roster step
therefore precedes that CLI. It must validate the same CSDA schema before offering
players, and pass the selected identifier as a string rather than a JavaScript
number. SteamID64 exceeds JavaScript's exact integer range.

The current launcher terminates its child tree on timeout, but it is not a parser
sandbox. Likewise, the current JSON limit and escaped report are not proof that
the local tool is hardened for hostile internet uploads.

## RAM versus SSD

**Default to streamed SSD intake.** Hash while writing with a small fixed buffer;
do not hold the whole demo or multipart body in RAM. CSDA already expects a path,
and decoded JSON can exceed the original replay size. A RAM disk adds another
memory-pressure/failure mechanism without improving the parsing bottleneck by
itself. Consider it only after measuring the I/O share and qualifying a hard
memory budget. This CPU service must coexist with the operator's model/game.

Proposed conservative starting policy, **not yet load-tested**:

| Resource | Initial limit |
|---|---|
| Input | One nonempty `.dem`, at most 256 MiB |
| Intake | Stream byte limit plus deadline; reject size mismatches/truncation |
| Jobs | One active worker, at most three accepted waiting jobs, one active intake per owner |
| Worker | 4 GiB job memory ceiling, 300 s per decoder/analyzer invocation |
| Export | At most 512 MiB JSON, matching the current importer limit |
| Storage | 1 GiB per-job workspace; aggregate free-space reserve/admission gate |
| Idle selection | Expire after 15 minutes without a selected player |
| Result | One-hour owner-only retention after successful publication |

These ceilings can reject legitimate large matches; return a precise reason
rather than restarting indefinitely or exceeding the host budget. Validate
resource settings against representative real demos before promising capacity.
When the queue, host budget or storage reserve is exhausted, reject before
accepting bytes. Do not pause models or kill unrelated processes to free space.

## Proposed application contract

All `/api/jobs` routes require the current owner's authenticated session, not
merely an unguessable ID. Use short-lived Secure/HttpOnly/SameSite cookies,
CSRF protection for changes, exact-origin checks and owner binding at every
read/write. Never reuse kernel/voice keys or ship a bearer in frontend HTML.
An initial invite-only private rollout is preferable to anonymous public intake.

| Route | Contract |
|---|---|
| `POST /api/jobs` | Reserve queue/storage; return random job ID and bounded upload deadline |
| `PUT /api/jobs/{id}/demo` | One raw binary upload; fixed server filename; verify complete input |
| `GET /api/jobs/{id}` | Owner-only state, elapsed time, real upload bytes, queue position, expiry/error |
| `GET /api/jobs/{id}/players` | Bounded validated roster after decode; IDs are strings |
| `POST /api/jobs/{id}/analysis` | Select a roster member once; repeat same selection is idempotent |
| `GET /api/jobs/{id}/result` | Bounded summary only from a complete, hash-confirmed report |
| `GET /api/jobs/{id}/artifacts/{name}` | Exact artifact allowlist and matching job manifest |
| `DELETE /api/jobs/{id}` | Idempotent cancel/delete; never claim deletion before child exit and cleanup |

Use a small native SQLite ledger for state/leases/restart reconciliation, not a
new database constellation. State transitions check owner, expected state and
lease. A second upload/selection must not race the current writer. Interrupted
uploads and worker crashes become explicit failed/retryable jobs; startup
reconciles pending cleanup before accepting new work. Polling is enough initially;
do not add sockets/processes merely to draw progress.

## Required public boundary

- Uploads are hostile data. Ignore client filenames as paths. Reject links,
  reparse escapes, path traversal and incomplete framing; atomically seal a
  complete input before a decoder can open it.
- A Job Object can bound memory/time and kill exact children, but **does not**
  prevent filesystem/network access. Qualify a restricted low-privilege worker
  with per-job ACLs and real network denial; use a stronger disposable VM boundary
  if it cannot be demonstrated to protect the operator's private host data.
- Never run workers elevated, as SYSTEM, or with GodBrain/model/API credentials.
  Do not execute uploads or use filenames/labels to construct a shell command.
- Render untrusted names/errors via `textContent`, keep errors bounded/redacted,
  and never leak absolute paths, credentials or another owner's job. Serve raw
  reports as attachments or in a separate sandboxed origin, not as trusted
  same-origin HTML. Keep existing CSV formula escaping.
- SHA-256 identifies exact bytes and pinned binaries; it is not proof a parser
  input is safe. Maintain decoder protocol fixtures and license notices.
- Raw demo/JSON is removed after successful report publication; cancel/failure
  cleanup runs after the worker exits. Persist deletion failures as
  `cleanup_pending`, retry boundedly, and stop new admission if storage is unsafe.
  Per-job keys on encrypted storage can bound recoverability; file deletion alone
  is not a promise of physical SSD erasure or removal from backups.
- No persistent cross-user cache, shared demo library, telemetry of gameplay,
  or private-SSD share. Hashes/filenames can themselves be sensitive: retain only
  minimal owner-scoped receipts for the stated period.

## Implementation cuts and acceptance checks

1. Qualify the existing parser/analyzer on representative demos: exact report
   parity, peak RAM, disk growth, elapsed time and unsupported protocols.
2. Implement local native intake/ledger/worker isolation. Test malformed/truncated
   uploads, oversize bodies, parser crashes, timeouts, queue saturation, cancel,
   restart reconciliation, cleanup failure, traversal and child-tree escape.
3. Add owner sessions and HTTPS; prove cross-owner reads/downloads/cancels denied,
   CSRF rejected, no worker network/private-path access and no credentials inherited.
4. Integrate this static UI with actual states and manifest-confirmed results;
   test real desktop/mobile upload, roster selection, report download and expiry.
5. Run an invite-only soak with measured resource budgets before public exposure.

This draft does not authorize host firewall/registry/service changes, public
exposure, software installation, private-file movement or deployment.
