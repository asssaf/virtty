# Agent Instructions

## GitHub Actions

When adding or modifying GitHub Actions workflows in this repository, you must use a specific commit hash for all actions instead of version tags. This ensures reproducibility and security.

Example:
```yaml
uses: actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683 # v4.2.2
```

To find the latest commit hash for an action, use `git ls-remote` or consult the GitHub action's release page.
