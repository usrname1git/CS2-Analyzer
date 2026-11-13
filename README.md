# CS2 Analyzer

Standalone Windows C++ analysis and offline reporting over CSDA demo exports.
The local CLI has no cloud dependency, Node runtime, account scraper or
automatic cheating verdict. An opt-in native web beta is described below;
it has no GodBrain command surface.

## Private web beta

`https://csda.autom8ed.me` serves `web\` through the existing ClipShare Caddy
edge, with a separate native loopback backend on `127.0.0.1:8091`. This is real
guest upload/analysis, not `docs\service-mockup.html`, which remains synthetic.
Accounts and MongoDB receipt history are not implemented.

```powershell
.\build\Release\csda-service.exe setup
.\Start-CSDA.ps1
.\build\Release\csda-service.exe invite
# Explicit shutdown:
.\build\Release\csda-service.exe stop
```

Run setup/invite as the normal service user. The invite command asks before
copying the beta code to the clipboard; it never prints it. The code is kept
in that user's Windows Credential Manager, separately from all other tokens.
The start door drops elevation when necessary and returns after readiness;
the service refuses an elevated token. No scheduled task or Windows service
is installed: run the door again after reboot. The shared ingress has its own
start/stop control; stopping ClipShare through Desk also stops that shared edge.

Guest sessions use Secure/HttpOnly/SameSite cookies, origin and CSRF checks,
and owner-bound job identifiers. Uploads are streamed to private SSD storage,
limited to one decompressed CS2 replay (512 MiB) per job. One serial worker
handles the pinned decoder, bounded roster extraction and selected-player
analysis using the existing C++ library. Each child runs in a fresh AppContainer
with zero capabilities, a credential-free environment and an exact-child Job.
Startup checks real private-file/network denial before accepting requests.
No shell, model, arbitrary URL intake or public filesystem browsing is exposed.

Admission requires 8 GiB available RAM and disk; at most four unfinished jobs
and 32 retained jobs are accepted, with one unfinished job per owner. The
provisional per-phase commit ceiling is the smaller of 16 GiB and available
RAM minus 4 GiB; monitoring stops the worker if the host reserve falls below
4 GiB. Each phase has a 300-second timeout and monitored 3 GiB write budget
(not a filesystem hard quota). Complete raw replay/JSON is removed before
report publication; selection expires after 15 minutes. Accepted player analysis
has a separate one-hour queue/processing deadline, without extending phase
timeouts; completed reports then expire after one hour. Cancellation and expired
selection cannot renew a job. Cleanup resumes after restart; failures close new
admission.

Resource receipts distinguish CPU-seconds, worker elapsed time, peak committed
memory, peak working set and I/O bytes. Upload bytes/time and served payload
bytes are application measurements, not WAN capacity, NIC utilization or TLS
overhead. `served_payload_bytes` counts only completed server sends of result/report
GET bodies, not HEAD, receipts, or failed partial sends. A download already
authorized before cancellation may finish; it cannot restore the deleted ledger.
One 124 MiB reference replay decoded/analyzed in roughly two seconds
with about 162 MiB peak worker working set on the qualification host; this is
one fixture, not a concurrency benchmark or optimum RAM budget.

Opt-in live validation (uses the normal user's local beta credential without
printing/exporting it):

```powershell
.\tests\Test-Service.ps1 -DemoPath 'C:\Demos\esportal_6045888_2024_mirage.dem'
```

This tests the actual TLS hostname from the hosting PC, including upload,
roster, native reference totals, report hashes and raw cleanup. It is not
proof of an authenticated upload from a separate external device.

## Setup

Requirements: Windows x64, PowerShell 7, CMake 3.25+ and Visual Studio's C++ x64
build tools. The first CMake configure downloads nlohmann/json 3.12.0 with a
pinned SHA-256. Subsequent builds and generated reports do not need the network.

```powershell
.\scripts\Build.ps1 -Test
.\scripts\Install-Csda.ps1
```

CSDA v1.11.0 is pinned in `tools\csda.lock.json`. The installer verifies its
release archive SHA-256, installs only under this project and records the
executable hash. It never changes PATH or requires an installer. To upgrade,
review the new export schema and release, update the lock, and replace the
local installation explicitly; the installer refuses to overwrite a different
existing installation. A custom `-CsdaPath` is supported, but its provenance is
labelled `custom_binary` unless accompanied by a matching installation receipt.

## Verification

The offline suite uses synthetic data. Its containment fixture owns an ephemeral
loopback port, so it can run alongside the live service on 8091 without stopping
or reconfiguring that service. The production startup probe still uses 8091.
The separate `service-api-fixtures` target exercises production request handling
on fixture-owned ephemeral sockets with synthetic credentials, a disposable
SQLite ledger, and a controllable clock. It checks exact cookies/CSRF/ownership,
slow-body and backpressured-response concurrency, bounded login recovery,
selection expiry, and completed-transfer accounting. It does not run service
setup, read Windows credentials, or create AppContainer profiles.

For scoped offline verification after configuring CMake, keep child scratch data
inside the project and exclude the containment/live qualification suites:

```powershell
$null = New-Item -ItemType Directory -Path '.\results\fixtures' -Force
$env:TEMP = (Resolve-Path '.\results\fixtures').Path
$env:TMP = $env:TEMP
cmake --build .\build --config Release --target `
    csda-service service-tests analysis-tests cs2-analyzer test-decoder
ctest --test-dir .\build -C Release --output-on-failure `
    -R '^(service-api-fixtures|analysis-fixtures|cli-and-launcher)$'
node --test .\tests\web_app_tests.js
```

Node is only a development test runner here; the CLI and service have no Node
runtime or npm dependency. Browser tests execute the actual `web\app.js` with
deferred requests and timers. Cancelling invalidates pending job responses and
upload callbacks. Transient status/result failures retry five times with bounded
backoff; exhaustion exposes **Retry status**, which queries the same job without
resubmitting upload or analysis. Authentication, missing-job, and expiry errors
stop automatic retries.

For an opt-in real-demo regression,
download `cs2/esportal_6045888_2024_mirage.dem` from
https://gitlab.com/akiver/cs-demos/-/raw/main/cs2/esportal_6045888_2024_mirage.dem
and, optionally, its v1.11.0 reference snapshot from
https://raw.githubusercontent.com/akiver/cs-demo-analyzer/v1.11.0/tests/snapshots/esportal_6045888_2024_mirage.json.
These are not included in Git or automatically downloaded by tests.

```powershell
.\tests\Test-PublicDemo.ps1 -DemoPath 'C:\Demos\esportal_6045888_2024_mirage.dem' `
    -ReferenceJson 'C:\Demos\reference.json'
```

This verifies every player's combat totals against CSDA's scoreboard and the
reference snapshot, plus event tables. Comparison follows upstream weapon ID
redaction, stable shot ordering and first-appearance projectile aliases;
player color is not treated as a combat statistic. A receipt is saved under
the local ignored `results` folder.

## Analyze demos or existing CSDA JSON

```powershell
.\Invoke-CS2Analysis.ps1 `
    -InputPath 'C:\CS2_Demos' `
    -OutDir 'C:\CS2_Analysis' `
    -SteamId '76561198000000001' `
    -Platform Faceit -SuspectName Player -Elo 3000
```

The input can be a `.dem`, a CSDA `.json`, or a directory containing either.
Use `-Recurse` to scan subdirectories. `Premier` maps to CSDA's `valve` source;
`Unknown` leaves source detection to CSDA. ELO and platform labels are supplied
metadata, not externally verified data or a statistical reference population.
No account access, uploads, paid APIs or scraping occurs.

`-TimeoutSeconds` defaults to 300 per decoder/analyzer invocation. Decoding is
serial and CPU-only. Only the process started by the launcher and its
descendants are terminated on timeout. `-Positions` requests the much larger
position export; it is off by default. Inputs must be decompressed demos.

Each run creates a new uniquely named directory under `OutDir`. The original
argument names are retained, but the misleading old cheat-detection filename
and verdicts are not. The C++ CLI can also import JSON directly:

```powershell
.\build\Release\cs2-analyzer.exe --input 'C:\Exports\match.json' `
    --steam-id '76561198000000001' --output 'C:\CS2_Analysis\new-run' --name Player
```

The direct CLI requires a new output directory. It writes into a private
staging directory and publishes the run only after all reports and the
completion manifest have been written. On failure, no final run is published;
incomplete staging data is labelled in stderr and retained for diagnosis.

## Outputs

The timestamped `CS2-Analysis-<name>-<time>` prefix is used for:

| File | Contents |
|---|---|
| `*-kills.csv` | Target-player kill, death and assist events, match/round/tick references and timing status |
| `*-byfile.csv` | Event-derived per-match statistics and original CSDA scoreboard fields |
| `*-global.csv` | Counts and ratios aggregated from their numerators/denominators |
| `*-rounds.csv` | Observed player rounds, including zero-kill rounds, side switches and overtime |
| `*-weapons.csv` | Weapon kills, headshots and timing sample coverage |
| `*-summary.txt` | Readable summary and measurement limits |
| `*-report.html` | Self-contained dashboard, SVG charts, review markers and searchable event table |
| `analysis.json` | Machine-readable summary, match metrics, weapons and warnings |
| `manifest.json` | Completion receipt, SHA-256 input/output hashes, decoder provenance and duplicate decisions |
| `raw\mN\*.csv` | Every CSDA event-array table plus player rows, without losing extra schema fields |
| `raw\mN\match.json` | Original non-array metadata, teams and results |

The HTML opens directly from disk and needs no CDN, web server or network.
The locally generated public-demo example is
`results\example\CS2-Example-report.html` when present.
All untrusted display text is escaped. CSV text beginning with formula
characters is prefixed with an apostrophe. Nested raw fields use JSON strings.
SteamIDs remain exact decimal integers/strings; **import SteamID columns into
Excel as text**, since Excel's numeric cells cannot represent SteamID64 exactly.

Original demo bytes are never modified. The launcher caches JSON under a key
bound to demo SHA-256, decoder executable SHA-256, forced source and position
option; reuse also verifies the JSON hash. Existing JSON input is labelled
as supplied, not falsely claimed to have been decoded here. CSDA's short
checksum is used for demo deduplication, not as a cryptographic guarantee.
Conflicting core exports sharing that checksum fail rather than silently
choosing a winner. The core comparison ignores the source label, run-dependent
weapon/projectile IDs and player color, and normalizes shot/economy ordering;
source labels are recorded for every input. The original raw
exports and their full file hashes are still preserved. Processing is
sequential; concurrent writers to one cache directory are not supported in
this release.

## Definitions and limitations

- **Kills:** enemy kills by the target, excluding suicides, team kills and
  controlled-bot kills. Deaths include ordinary suicides/world deaths but
  exclude CSDA's self/World disconnection convention and controlled-bot deaths.
  Assists and headshots are independently counted. CSDA scoreboard mismatches
  are warnings, not silently overwritten.
- **Round denominator:** completed live rounds with a target-player economy
  snapshot. Zero-kill rounds are included. If recorded target activity lacks
  these snapshots, round-based rates become unavailable. This is an observed
  snapshot definition, not a claim to know every disconnect/reconnect interval.
- **KPR/ADR:** total enemy kills / observed rounds and CSDA-reported enemy
  health damage / observed rounds. Utility damage is separately counted.
- **Multi-kill rate:** rounds with at least two enemy kills / all observed
  rounds, not / rounds where a multi-kill occurred.
- **Opening duel rate:** target opening kills / target opening kills plus
  opening deaths. The first eligible enemy kill is selected in each match/round.
  Trades use CSDA's source flags.
- **Wins:** player side is mapped to team A/B for each economy snapshot.
  Team switching or missing winner data leaves match outcome unresolved.
  Match win rate uses resolved results only; unresolved matches are reported.
- **Timing:** first matched positive damage to an enemy until the kill,
  restricted to the same attacker, victim, weapon and round. A same-tick
  terminal damage event is required. A gap of more than two seconds between
  damage events resets the encounter. Missing data yields `null` plus a reason.
  Firearm categories only; no fabricated fractional-tick/subtick correction.
- **Review marker:** firearm kill with one or two positive damage events and
  damage-to-kill interval at most 200 ms. This includes normal one-shot kills.
  Damage events are not bullets. It is neither a suspicion score nor a verdict.
- **Not reaction time:** damage timestamps do not establish when an opponent
  became visible, what the player knew, pre-aim, audio, teammate communication,
  latency, or human input timing. Radar/wallhack or aim-assistance probabilities
  are not inferred from KPR, inventory value, player age, ELO or fast kills.
- Zero denominators produce `null`/`N/A`, never a made-up finite ratio.
  Global ratios use summed counts, not averages of match percentages.
- This release accepts CSDA's CS2 match schema, with a 512 MiB JSON input limit.
  Malformed schema, non-integral IDs, missing required event tables, duplicate
  round numbers and unknown round references fail explicitly. Positions and
  large raw tables can still consume substantial memory; this is a local tool,
  not a hardened internet upload service.

## Later, not implemented

SteamID/profile URL lookup, Premier/Faceit match acquisition and date selection
need authorized provider APIs, actual demo availability, rate limits and
retention rules. They cannot be guaranteed from a SteamID alone.
CSDA's date can come from the demo file's modification time, so it is not
automatically an authoritative match date.
Platform/rank-aware anomaly analysis requires labelled, representative
reference data and measured false-positive rates; an attractive report is not
evidence that a detector works.

CSDA and demoinfocs are open source. Forking or embedding them in a service is
possible under their licenses; reverse engineering is unnecessary. A fork
still needs maintenance as Valve changes demo protocols. A C++ decoder rewrite
would be a separate large project, not an automatic consequence of owning
this analyzer. An upload service additionally needs sandboxed decoding,
CPU/RAM/disk limits, bounded queues, authentication, privacy and cleanup policy.

Third-party notices are in `THIRD_PARTY_NOTICES.md`. No repository publishing
or license choice for the newly written project is performed by this setup.
