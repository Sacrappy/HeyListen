# Making a release

A HeyListen release holds the **HeyListen Builder** and nothing of the game: the Windows setup (`HeyListenBuilder.exe`) and
its list (`heylisten-setup.txt`), the RecompLauncher zip (`HeyListen-<tag>-Windows-x64.zip`), the Linux AppImage and its list
(`heylisten-linux.txt`), the user guide and `SHA256SUMS.txt`. The builder's update parts live on two releases of their own,
`builder-parts` (Windows) and `linux-parts` (Linux), named by their checksum, so a part that did not change is never uploaded
twice.

Releases are made on GitHub's runners by [`.github/workflows/release.yml`](../.github/workflows/release.yml), from the tag
and nothing else, and end as a **draft**: the maintainer reads it and publishes it by hand. Nothing in the workflow publishes
a release, pushes a tag or posts anything. (The Windows files are not code-signed.)

## The steps

1. Write `docs/release/RELEASE_NOTES.md` and merge everything into `main`. In the notes, `{TAG}` becomes the tag and `{SINCE}`
   the previous full release's tag (the release the new one reuses its parts from; `make_release.py --since vX.Y.Z` sets it).
2. Tag and push: `git tag v0.4.1 && git push origin v0.4.1`. That starts the workflow:

   | Job | Runner | What it does |
   |---|---|---|
   | Version and mode | Ubuntu | Reads the tag (a push makes the draft; a manual run never does). |
   | Windows files | Windows | Sets up Python 3.12, the .NET 8 SDK and llvm-mingw (cached; its SHA-256 is checked either way), then `make_release.py`: the setup, the builder, the parts (those an earlier release made are downloaded and checked, not rebuilt), `heylisten-setup.txt`, the notes, the guide, the RecompLauncher zip, the FORBIDDEN check. |
   | Linux | Ubuntu 22.04 container | [`linux.yml`](../.github/workflows/linux.yml), called: the game-free checks and the AppImage with its parts. Publishes nothing when called. |
   | Checks and draft release | Ubuntu | `make_release_linux.sh` (the Linux gate, with the `recomplauncher.json` check), `collect_release.py` (FORBIDDEN over every file and zip again, the Windows list against its parts, the zip's `recomplauncher.json`, `SHA256SUMS.txt` over every asset and new part), then the draft. |

3. Upload the new parts, unless `RELEASE_UPLOAD_PARTS` does it (below). The run's `release-files` artifact holds them:

   ```sh
   gh run download <run id> -R Sacrappy/HeyListen -n release-files -D release-files
   gh release upload builder-parts -R Sacrappy/HeyListen release-files/builder-parts/*.zip   # (skip names already there)
   GITHUB_REPOSITORY=Sacrappy/HeyListen GITHUB_SHA=$(git rev-parse v0.4.1) GH_TOKEN=$(gh auth token) \
     engine/scripts/linux/publish_parts.sh "$PWD/release-files/linux-parts"
   ```

4. Read the draft (Releases page): the files, the notes, a test install from `HeyListenBuilder.exe` and the AppImage. Then
   **publish it by hand**. Publishing does not start anything else: `linux.yml` sees that the release already has
   `heylisten-linux.txt` and does nothing.

An existing release or draft for the tag is never replaced: delete the draft by hand to make it again (re-run the run's
failed jobs).

| Setting | Kind | Value |
|---|---|---|
| `RELEASE_UPLOAD_PARTS` | repository variable, optional | `true`: the last job uploads the new parts to `builder-parts` and `linux-parts` (public as soon as uploaded) |

`tools/release/make_release.py` still makes the Windows files on a PC (`python -I tools/release/make_release.py --tag vX.Y.Z`;
it needs git, Python 3.10+, the .NET 8 SDK on PATH or `--dotnet`, and internet), and `tools/release/make_release_linux.sh`
checks downloaded Linux files. A release made by hand the old way still gets its Linux files from `linux.yml` when it is
published.

## No game data, ever

Nothing of the game is in the repository, in CI or in a release. The workflow builds only from `engine/`, the official tool
downloads (SHA-256 checked) and earlier HeyListen parts, and refuses, twice, anything whose name looks like game data, keys or a
cache (`FORBIDDEN` in `make_release.py`, in every file and inside every zip): once when the Windows files are made, once over
all files before the draft. The Linux files go through `check_release_linux.py` as well.

## A dry run

A dry run makes and checks everything and leaves the files as the run's artifacts (`release-files` holds the would-be
release); no draft, no tag, no part uploaded. Actions, *Release*, *Run workflow*, from the branch to try, with a version that is
not a release (`v0.4.1-dry.1`):

```sh
gh workflow run release.yml -R Sacrappy/HeyListen --ref <branch> -f tag=v0.4.1-dry.1
```

GitHub offers *Run workflow* for a workflow only once its file is on the default branch; before that, a branch can start a dry
run by a temporary `push` trigger in `release.yml` naming it (never a tag).
