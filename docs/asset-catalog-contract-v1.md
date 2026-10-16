# Asset catalog contract v1 (coordination draft)

Owner: firmware defines this wire contract; the asset producer and Inky consume it.
This draft coordinates local implementation of specification version 3. It does
not declare any endpoint deployed, any license cleared, or a shipping gate passed.

## Catalog

UTF-8 JSON, at most 8192 bytes including whitespace. One catalog per kind.
Required root fields:

| Field | Value |
| --- | --- |
| `schema` | integer `1` |
| `kind` | `hyphenation` or `translation` |
| `channel` | `stable` or `rc` |
| `revision` | full lowercase 40-hex immutable source snapshot ID |
| `format` | integer `1` (CPHY v1 or CrossInk flat YAML v1 respectively) |
| `requires` | integer `0`; any nonzero capabilities currently unsupported |
| `notices` | literal `notices.json` |
| `sources` | literal `sources.json` |
| `entries` | array, maximum 9 hyphenation or 48 translation entries |

Translation roots additionally require `source` and `keyset`, lowercase 64-hex
SHA-256 of English source bytes and ordered source key names, as emitted by
`generate_language_template.py` (`englishSourceSha256`, `keysetSha256`). Both must
match the running firmware. Hyphenation roots omit these fields; translation
compatibility never blocks a hyphenation snapshot. Firmware release labels are
not exact-version locks. Entries inherit kind, format and requirements.

Required entry fields: `code` (ASCII identifier, maximum 31 bytes), `bytes`
(integer 1..262144), `sha256` (lowercase 64-hex). Optional `previous` is exactly
one lowercase 64-hex hash, different from current; arrays/history are forbidden.
Translation entries require `name`, nonempty valid UTF-8, maximum 95 decoded
bytes, with no control characters. Hyphenation entries omit `name` and use the
firmware's labels. Hyphenation codes are exactly `fr,de,ru,es,it,pl,pt,sv,uk`.
Translation codes must match the existing supported UI-language registry; English
is built in and not an online asset. Identifiers match
`[A-Za-z][A-Za-z0-9_-]{0,30}`. Duplicate and ASCII-case-colliding codes fail.

Objects may put fields in any order. Duplicate fields, unknown fields, wrong
types, nonintegral numbers, unknown schemas/formats/capabilities, extra trailing
JSON and malformed UTF-8 fail closed. No optional extension fields in v1.
Maximum container depth is 3 (root object / entries array / entry object).
Field keys are at most 31 bytes; string tokens at most 95 decoded bytes.
Producers use compact JSON and literal UTF-8 (`ensure_ascii=False`). JSON escapes
must decode before checking limits, identities and duplicates.

## Immutable paths and companions

Mutable discovery pointers are `catalogs/<channel>/<kind>.json` below a compiled
catalog base prefix. The payload base prefix may differ. No base URL is inferred
from catalog data. Channel is exactly `stable` or `rc`; kind is exactly the root
kind. Promote these pointers only after all immutable references are available.

Paths are derived by consumers, never accepted as arbitrary catalog URLs:

* `snapshots/<revision>/hyphenation/<sha256>.cphyph`
* `snapshots/<revision>/translation/<sha256>.yaml`
* `snapshots/<revision>/notices.json`
* `snapshots/<revision>/sources.json`

The two companion indexes are keyed first by kind, then by code. Every advertised
entry must have an entry in both. Notices identify applicable licenses and
attribution plus relative immutable notice file references. Sources identify
pinned upstream revisions, original source paths, immutable source references,
SHA-256 and compiled/source correspondence. Firmware streams both indexes and
the complete selected union of their `files` references to SD before publishing
the installable payload. References have exactly `path`, `bytes`, and `sha256`.
Each path stays under `snapshots/<revision>/source/`; no URL, traversal, FAT
alias, case collision, or file/directory prefix collision is accepted. Repeated
paths across indexes must have identical size and digest. Unknown descriptive
index metadata is validated as JSON and skipped without retaining it in RAM.
Their publication schema and licensing evidence are owned by the asset worker.
Missing companions block both catalog promotion and device payload publication.

Indexes have no catalog-pinned digest in v1: fresh certificate-bundle HTTPS and
hostname validation authenticate their exact fixed paths. Reference size/digests
then bind companion bytes to those authenticated indexes. Local completion
markers are recovery evidence, never authentication. An incremental digest of
each HTTPS index body must match its SD read-back, and the reference spool is
likewise bound to the freshly parsed records. There is no cache shortcut:
every explicit Download fetches the indexes and selected closure afresh.

Recipient files are preserved unchanged under
`/.crosspoint/asset-sources/<payload-sha256>/<revision>/`, with their `source/`
paths retained alongside both indexes, a bounded reference spool, and a
`complete.txt` marker recording identity, index sizes/hashes and spool hash.
Opaque source ZIP archives are not extracted. All files are read back before
writing and reading back the marker, then the staged directory is published
before the original CPHY/YAML payload transaction begins. Failed or interrupted
work keeps the previous payload; explicit retry restores `.old` if needed and
clears incomplete `.part` work. Successful replacement removes its temporary
backup. Other payload/revision directories are retained; no deletion UI is added.

Hosts and base prefixes are separately compiled official endpoint configuration,
pending parent approval and real endpoint qualification. Neither snapshots nor
digests authenticate a catalog: firmware requires ESP certificate-bundle HTTPS
and hostname checking for the catalog and every approved redirect hop. Payload
acceptance requires catalog-pinned size/SHA-256 plus full format validation.
No URL here is a deployed address.

Build configuration uses `ASSET_DOWNLOAD_BASE_URL` for immutable payloads,
`ASSET_CATALOG_BASE_URL` for catalog discovery (defaults to the payload prefix),
and optional `ASSET_DOWNLOAD_REDIRECT_BASE_URL` for one additional approved
prefix. Prefixes must be HTTPS URLs ending in `/`; both catalog and payload
requests allow at most two redirect hops. Empty defaults disable online service.
The URLs cannot be supplied by a downloaded catalog or a user-edited setting.

### Transport security limits

Catalog requests explicitly select `HttpDownloader::Transport::ESP_HTTP`, which
attaches `esp_crt_bundle_attach`; the zero-initialized ESP client configuration
keeps hostname checking enabled. Each redirect is checked against compiled HTTPS
prefixes before opening the next request. The existing insecure wolfSSL option
is not used to authenticate this catalog. SHA-256 authenticates neither a server
nor an untrusted catalog: it binds payload bytes to an already authenticated
catalog.

The inspected C3 SDK configuration enables `CONFIG_MBEDTLS_HAVE_TIME` but leaves
`CONFIG_MBEDTLS_HAVE_TIME_DATE` unset. Certificate validity-date enforcement and
catalog freshness/replay protection must therefore not be claimed. This feature
does not change certificate settings, add mandatory NTP, or persist a replay
counter. Qualify the exact release SDK and approved endpoints on hardware before
enabling distribution. Local simulator HTTP mocks test consumer behavior only;
they do not qualify TLS, redirects, certificate expiry, or live availability.

## Storage and budgets

Managed YAML filename is `<code>.yaml` under `/.crosspoint/languages`.
Replace a sole approved same-code manual source in place; never create a second
source. Hyphenation destination remains `hyph-<code>.cphyph` under
`/.crosspoint/hyphenation`. Block same-code or FAT case collisions.

One catalog resident, no retained JSON, one transfer. Fixed record/parser memory
must be below 16384 bytes (static assertion plus device measurements at release).
Each body must meet the total 8192-byte limit even when individual field/count
ceilings would permit a larger document. YAML compiled feasibility remains
65536 bytes; CPHY also requires inactive-bank capacity with retained packs.

Source status compares actual bytes against current and optional previous hash;
it is independent of active runtime state. Unknown bytes require explicit
overwrite confirmation with Cancel default. `.old` is transaction recovery only.
Download remains available for a Current source to repair missing or damaged
recipient companions. Current describes payload identity, not companion health.

Each companion index and referenced file is at most 2 MiB. Each index selects
at most 16 references; both together select at most 32 and 8 MiB before
deduplication. Paths are at most 240 ASCII bytes. Index parsing bounds depth to
16, keys to 64 decoded bytes, skipped strings to 64 KiB, and tokens to 131072.
Files stream through a 1024-byte buffer; references are spooled to SD. The host
parser is 720 bytes and the helper workspace is 3568 bytes (4288 bytes combined
during parsing), allocated only for the operation. The language discovery
inspector is released first; its enclosing 2768-byte service workspace remains
allocated. Simultaneous known host storage is catalog 12856 + activity 1768 +
service workspace 2768 + companion workspace 3568 + companion parser 720 =
21680 bytes. This subtotal excludes network allocations, stacks, other objects,
and allocator overhead; 4288 bytes is only the companion helper's contribution.
The scoped catalog/activity/companion-parser total is 15344 bytes, below the
16 KiB catalog ceiling. Actual device peak heap and stack remain unmeasured.

Before index transfer the device reserves their full bounded size plus payload
and 1 MiB metadata allowance; after parsing it checks remaining unique files,
payload and that allowance. This conservative check is not an exact FAT cluster
estimate. Write/sync/close failures still abort safely. Cleanup uses a bounded
path and at most 8192 traversal operations, with cancellation checks.

## Local fixtures

`test/asset_catalog/fixtures/hyphenation.json` and `translation.json` are local
producer-shape examples generated from this checkout, not advertised releases.
Their revision is a source snapshot, not proof of publication or legal clearance.
Assets/publication tooling must reproduce and validate its own complete snapshot.
