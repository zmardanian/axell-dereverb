# Security policy

## Supported versions

Only the latest release of Axell De-Reverb gets security fixes. Please update before you report: the newest version is always
on the [releases page](https://github.com/zmardanian/axell-dereverb/releases/latest) and on [axellsdojo.com](https://www.axellsdojo.com).

## Reporting a vulnerability

Please **do not open a public issue** for a security problem. Report it privately instead:

- on GitHub: [Report a vulnerability](https://github.com/zmardanian/axell-dereverb/security/advisories/new) (private, only the maintainer can see it), or
- by email: contact@axellsdojo.com

Please include what you found, how to reproduce it, and the version and operating system you used. You will get an
answer within 7 days. Fixes for confirmed problems are released as soon as they are ready, and the advisory is
published with credit to you, unless you would rather stay anonymous.

## What counts

- crashes or memory errors a crafted preset, session file or audio input can trigger in the plugin
- the release files: a download whose SHA256 checksum does not match `SHA256SUMS.txt` on the release page
- the Axell Signature Dojo's updater (it downloads releases from GitHub and checks their SHA256 checksums)

Sound or usability bugs are not security problems: please open a normal issue for those.

## Checking a download

Every release lists the SHA256 checksum of each file in `SHA256SUMS.txt`. On Linux: `sha256sum -c SHA256SUMS.txt`;
on Windows (PowerShell): `Get-FileHash <file> -Algorithm SHA256`.
From the November 2026 releases on, `SHA256SUMS.txt` is also signed with the suite's release key (minisign, `SHA256SUMS.txt.minisig`):
`minisign -Vm SHA256SUMS.txt -P RWQJLVJSETsP7RRz1rdiugX9w09wtGCtwEc5hHU8k9S+crL/ekfTkQKG`. A file that fails this check was not published by the maintainer - please report it.
