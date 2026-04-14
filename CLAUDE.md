# libcg

## Git Workflow

Use Graphite (`gt`) for stacked PRs. Each branch in the stack is one PR.

```bash
gt auth --token <token>              # one-time: token from app.graphite.com/settings/cli
gt create <branch-name>              # create a stacked branch from staged changes
gt modify                            # amend current branch, restack descendants
gt submit --stack --no-interactive --publish   # push stack & create/update PRs (headless, ready for review)
gt sync                              # rebase on trunk, clean merged branches
gt log                               # show stack graph
```

Always pass `--no-interactive` to `gt submit` to skip prompts. Add `--publish` too — without it, `--no-interactive` creates PRs in draft mode by default.
