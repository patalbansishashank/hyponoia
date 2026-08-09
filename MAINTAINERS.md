# Maintainers

Hyponoia has one maintainer.

| Handle | Contact | Scope |
| --- | --- | --- |
| [`@patalbansishashank`](https://github.com/patalbansishashank) | mail@creative.desi | Everything: code, review, security, releases, and repository settings. |

Every pull request is reviewed and merged by the maintainer. There is no review
quorum, no delegation, and no escalation path, because with one person those
would be fiction. `.github/CODEOWNERS` is the binding version of this file;
this document explains it.

Security reports go through [SECURITY.md](SECURITY.md), not the issue tracker.
Conduct reports go to the address above — see
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Release discipline

Release process is a checklist rather than an approval chain:

- `dry-run.yml` completes successfully on the release-candidate commit.
- Indexing benchmarks are run with the candidate binary on real repositories,
  not test-only shortcuts, and compared against the previous release on the
  same machine class and same repository revisions.
- An unexplained indexing slowdown over 15%, or an unexplained shift in node
  and edge counts, blocks the release until it is understood.
- Benchmark logs, repository revisions, binary version, and machine details are
  kept with the release notes.

## Adding maintainers

When someone else starts maintaining part of this project, add a row to the
table above and give them the paths they own in `.github/CODEOWNERS`:

```
/src/mcp/  @their-handle
```

That is the whole process. Roles and review policy can be written down when
there are enough people for them to mean something.
