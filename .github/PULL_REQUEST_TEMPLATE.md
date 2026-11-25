*By completing this PR sufficiently, you help us review it quicker and keep the release notes honest.*

### Checklist
1. [ ] A title in the commit format, `type(scope): Summary` (*eg. "fix(fck_aos): Give the I2C bus back after the receiver's reset"*)
2. [ ] Related links below (*eg. the issue this PR closes*)
3. [ ] Relevant documentation updated - header comments, README - if the change touches what they say
4. [ ] `tools/host_tests.sh` and `tools/check_layers.sh` run, and green; `tests/target` built when `components/` changed
5. [ ] The branch is named for the change, as CONTRIBUTING.md says: `feat/`, `fix/`, `hotfix/`, `docs/`, ...
6. [ ] If a module was used: its Dot's MAC, the firmware on each chip, and what was written to them, said in the issue first
7. [ ] Nothing new transmits from an example or a test; what does, says which band, what power and which limit covers it

*This entire section above can be deleted if all items are checked.*

-----------
## Description of Change
Please describe the proposed change and its impact.

## Test Scenarios
Please describe on what hardware and software this was tested, and how. Tell "measured" from "read in the code".

(*eg. host suites, 97 cases; tests/target on v6.1; module KDFCK001 rev1.0 on a Dot (the Dot's MAC), USB cable, the firmwares the module ships with, a 433 MHz remote pressed during fck_subghz_find()*)

## Related links
Please provide links to related issues, PRs etc.

(*eg. Closes #number of issue*)
