# Fuzz campaign triage

## Units

| Unit | Target | CPU s | Executions | Corpus | Edges (cov) | Engine patched | Result |
|---|---|---|---|---|---|---|---|
| ctl-s0 | ctl | 2881 | 395637174 | 2544 | 403 | yes | no finding in 2880 s |
| display-s0 | display | 2881 | 28134282 | 855 | 172 | yes | no finding in 2880 s |
| label-s0 | label | 2881 | 27782513 | 1112 | 368 | yes | no finding in 2880 s |
| mountwalk-s0 | mountwalk | 2881 | 1486035 | 5448 | 4660 | yes | FINDING (1 distinct, see findings.tsv) |
| mountwalk-s1 | mountwalk | 2881 | 1564996 | 5586 | 4704 | yes | FINDING (1 distinct, see findings.tsv) |
| mountwalk-s2 | mountwalk | 2881 | 1786020 | 5577 | 4742 | yes | FINDING (1 distinct, see findings.tsv) |
| mountwalk-s3 | mountwalk | 2881 | 1577097 | 5676 | 4582 | yes | FINDING (1 distinct, see findings.tsv) |
| probe-s0 | probe | 2881 | 2878862 | 5386 | 3465 | yes | FINDING (1 distinct, see findings.tsv) |
| probe-s1 | probe | 2881 | 2392091 | 5250 | 3213 | yes | FINDING (1 distinct, see findings.tsv) |
| verdict-s0 | verdict | 2881 | 241886562 | 2458 | 584 | yes | no finding in 2880 s |

Total fuzz CPU time: 28810 s (8.00 h).

## Signatures

| Id | Status | Component | Class | Exploitable | Signature | Count | Units | Input |
|---|---|---|---|---|---|---|---|---|
| F7 | known | engine | L | no (bounded per process) | `leak ntfs_attr_open < ntfs_device_mount @attrib.c:426` | 1 | mountwalk-s3 | - |
| F5 | known | engine | L | no (bounded per process) | `leak ntfs_attr_readall < ntfs_make_symlink @attrib.c:7025` | 1 | mountwalk-s0 | - |
| F7 | known | engine | L | no (bounded per process) | `leak ntfs_inode_open < ntfs_device_mount < mount_view @inode.c:95` | 2 | probe-s0,probe-s1 | - |
| F7 | known | engine | L | no (bounded per process) | `leak ntfs_inode_open < ntfs_device_mount @inode.c:95` | 2 | mountwalk-s1,mountwalk-s2 | - |

## UBSan locations in the engine (recoverable, once per location and process)

| Location and message | Reports | Units | Known | Status |
|---|---|---|---|---|
| `attrib.c:385 shift exponent N is too large for N-bit type 'T'` | 3 | 3 | U | known |
| `attrib.c:386 shift exponent N is too large for N-bit type 'T'` | 3 | 3 | U | known |
| `bootsect.c:236 left shift of N by N places cannot be represented in type 'T' (aka 'T')` | 6 | 6 | U | known |
| `bootsect.c:236 signed integer overflow: N - N cannot be represented in type 'T' (aka 'T')` | 4 | 4 | U | known |
| `bootsect.c:238 signed integer overflow: N - N cannot be represented in type 'T' (aka 'T')` | 4 | 4 | U | known |
| `dir.c:399 left shift of N by N places cannot be represented in type 'T' (aka 'T')` | 3 | 3 | U | known |
| `reparse.c:156 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:156 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:167 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:167 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:327 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:327 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:333 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:333 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:347 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:347 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:841 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:841 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:848 load of misaligned address NxNcNfbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `reparse.c:848 load of misaligned address NxNeNfdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `runlist.c:862 left shift of N by N places cannot be represented in type 'T' (aka 'T')` | 1 | 1 | U | known |
| `runlist.c:862 left shift of negative value N` | 5 | 5 | U | known |
| `runlist.c:882 signed integer overflow: N + N cannot be represented in type 'T' (aka 'T')` | 3 | 3 | U | known |
| `runlist.c:897 left shift of negative value N` | 6 | 6 | U | known |
| `runlist.c:899 signed integer overflow: N + N cannot be represented in type 'T' (aka 'T')` | 2 | 2 | U | known |
| `unistr.c:183 load of misaligned address NxNaNf for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:183 load of misaligned address NxNbNcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:183 load of misaligned address NxNdNfN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:183 load of misaligned address NxNdcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:183 load of misaligned address NxNddNdfN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:183 load of misaligned address NxNeNf for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:188 load of misaligned address NxNb for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:188 load of misaligned address NxNbNcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:188 load of misaligned address NxNdNebcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:188 load of misaligned address NxNdNfN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:188 load of misaligned address NxNdcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:188 load of misaligned address NxNddNdfN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:345 load of misaligned address NxN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:345 load of misaligned address NxNcdN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:345 load of misaligned address NxNdNebcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:345 load of misaligned address NxNedN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:345 load of misaligned address NxNfcNbN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:345 load of misaligned address NxNfebN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:376 load of misaligned address NxNbeaNaN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:376 load of misaligned address NxNcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:376 load of misaligned address NxNdNebcN for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `unistr.c:376 load of misaligned address NxNfNfabf for type 'T' (aka 'T'), which requires N byte alignment` | 1 | 1 | U | known |
| `volume.c:1094 signed integer overflow: N - N cannot be represented in type 'T' (aka 'T')` | 1 | 1 | U | known |
| `volume.c:273 left shift of N by N places cannot be represented in type 'T' (aka 'T')` | 6 | 6 | U | known |
| `volume.c:624 signed integer overflow: N + N cannot be represented in type 'T' (aka 'T')` | 4 | 4 | U | known |
