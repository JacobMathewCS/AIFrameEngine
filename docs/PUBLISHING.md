# Publishing checklist

The local repository is source-only. Build outputs, production models, downloaded
SDKs, personal traces and the internal handoff are deliberately excluded.

Before the first upload:

1. Review `git status --short`, `git diff --cached --stat` and the staged contents.
2. Authenticate locally using `gh auth login --hostname github.com --git-protocol https --web`.
   Complete the browser flow yourself. Never paste tokens or passwords into issues or chat.
3. Set a repository-local commit name and email with `git config user.name` and
   `git config user.email`. Use your GitHub-provided no-reply email if desired; do not
   guess its value. No identity is assigned automatically.
4. Commit the reviewed files with `git commit -m "Initial source release"`.
5. Choose repository name and visibility explicitly. Running `gh repo create` interactively
   allows you to select an existing local repository and choose those settings.
6. Confirm the remote with `git remote -v`, then verify the uploaded files on GitHub.

No license has been selected for the original project. Resolve that separately before
advertising it as open source or accepting substantial external contributions. See
`THIRD_PARTY_NOTICES.md`; a project license cannot replace dependency/weight terms.

## Fresh-clone limitations

The dependency and model setup is currently manual. Follow `external/README.md` and
`models/README.md`; a source upload is not a standalone playable distribution.
The GitHub workflow runs only standard-library Python summary tests on Windows.
Native builds and GPU tests must still be run on a provisioned Windows machine.

## Local cleanup policy

Confirmed-unused backups are moved to an ignored, timestamped folder under `scratch`,
with hashes checked before and after. Working models, dependencies and historical
measurement evidence are retained. Ignoring a file is not the same as deleting it.