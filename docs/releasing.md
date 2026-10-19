# Releasing (maintainers)

How a version of ck3agent reaches the releases page.

## Prerequisites

- Write access to this repository and to the upstream research tree.
- The GitHub CLI, signed in.

## Steps

1. Upstream: stamp `VERSION`, `ck3agent/_version.py`, `ck3agent/_bridge_meta.py`,
   the `CHANGELOG.md` heading (`## YYYY.MM.N — date`, the tag's UTC date) and
   `CITATION.cff` (`version`, `date-released`). A test fails if they disagree.
2. Export, build and test; sync the export here as one commit on `main`
   through a pull request merged with a merge commit (a squash breaks the
   next sync).
3. With the repository public (attestation is skipped while it is private),
   tag the merge and push the tag:

   ```bash
   git tag -a v2026.10.0 -m "ck3agent 2026.10.0" && git push origin v2026.10.0
   ```

   The `Release` workflow builds the bridge from a clean checkout, refuses a
   provenance that is not clean at that commit, bundles the wheels, and opens
   a draft release with notes taken from the CHANGELOG section.
4. On the draft: check the assets (tarball, wheel, sdist, `.so.debug`,
   `SHA256SUMS`), then from a download directory:

   ```bash
   sha256sum -c SHA256SUMS --ignore-missing
   gh attestation verify ck3agent-2026.10.0-linux-x86_64.tar.gz --repo skullbloc/ck3agent
   ```

   Do not press "Generate release notes": it replaces the CHANGELOG notes with
   a list of pull requests. Tick "Set as the latest release" and publish.
5. Afterwards: a Zenodo DOI, if the integration is enabled, goes into
   `CITATION.cff` under `identifiers` in the next sync.

## Check it worked

- The releases page shows the version with five assets and the CHANGELOG notes.
- `ck3agent version` on a fresh install prints the version and the supported BuildID.

## If it fails

- The workflow refuses the tag: the version files disagree (step 1) or the
  CHANGELOG heading still says "unreleased".
- `gh attestation verify` fails: the tag was pushed while the repository was
  private. Delete the draft and the tag, and push the tag again.
