# Security Policy

## Supported Versions

Security fixes are provided on a best-effort basis for actively maintained code.

| Version/Branch | Security Support |
| --- | --- |
| Latest stable release | Supported |
| `master` | Supported |
| `develop` | Best effort |
| Older releases | Not supported |

## Reporting a Vulnerability

Please report security issues responsibly and do not publish exploit details
before a fix is available.

Fallback channel:

- Open a GitHub issue with minimal details and request a private follow-up:
  https://github.com/Leafuke/MineBackup/issues

## What to Include

To help triage quickly, include:

- Affected version, commit, or branch
- Operating system and environment
- Clear reproduction steps
- Security impact (data loss, code execution, privilege issue, etc.)
- Proof of concept (optional but helpful)
- Suggested fix (optional)

## Response Process

Maintainers will try to:

- Acknowledge reports within 7 days
- Provide an initial assessment within 14 days
- Coordinate a fix and disclosure timeline based on severity and complexity

## Disclosure and Credit

Please use coordinated disclosure. After a fix is released, maintainers may
publish a security note and credit reporters unless anonymity is requested.

## Project-Specific Notes

MineBackup interacts with local file systems, backup archives, and external tools
such as 7-Zip, and also performs network requests for update/notice checks.
When testing security issues, use non-production test data and avoid using real
player saves as your only copy.
