# Contributing

Thanks for helping with the SDK for the F\*CK module (KDFCK001) on the Kode Dot.

## Talk first

Open an issue before a large change; the templates in `.github/ISSUE_TEMPLATE` cover bugs, build problems, API proposals, documentation and tasks. A question or an idea that is not a proposal yet goes to [Discussions](https://github.com/kodediy/kode-fck-sdk/discussions).

## Branches and pull requests

- `main` is what works. A change goes in a branch, one branch per issue, and reaches `main` through a pull request.
- Name the branch with the kind of change, then the issue number and what it does, in lower case with hyphens: `feat/`, `fix/`, `hotfix/` (on something already released, from its tag), `docs/`, `refactor/`, `test/`, `ci/`, `chore/` or `release/`. For example `fix/12-tx-after-stop`.
- Commit by path, not with `git add -A`. A release is an annotated tag on `main`, `vX.Y.Z`.
- Fill in the pull request template. Say what was **measured** on hardware and what was only **read in the code**, and on which board.
- Anything that touches hardware is not done until it has run on a board. Nothing transmits without the permission of the person running the test: sub-GHz tests use a fixed test signal, never somebody's real remote.

## Style

- Code, comments, README and documentation in English.
- The layers are kode-sdk's: `fck.h` (tasks: calls block and return `bool`, `fck_error()` says why) and `fck_sys.h` (Active Objects: requests with `QActive *me` first, answers in `FCK_*_RSP_SIG`). Names are whole words, no abbreviations.
- No unreachable `assert` from the API and no `portMAX_DELAY` in the facade.
- Every function of `fck.h` and `fck_sys.h` is in `ALL[]` of `tests/target/main/main.c`; `tools/check_layers.sh` checks it.

## Before you open a pull request

With ESP-IDF v6.1 exported:

```sh
tools/host_tests.sh                    # eight host suites; --sanitize adds ASan and UBSan
tools/check_layers.sh                  # the layer rules
tools/check_example_pins.sh
idf.py -C tests/target build           # when components/ changed
tools/check_ram.sh tests/target/build tests/target/ram_budget.txt
for p in module console_actions wifi radio subghz gnss wardriving; do idf.py -C examples/$p build; done
```

Test cases are moved between suites, never deleted: the total does not go down. `tools/check_misra.sh` (cppcheck on `PATH`, see `tools/build_cppcheck.sh`) takes a baseline file that is not in the repository.
