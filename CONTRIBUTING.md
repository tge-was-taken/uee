# Goal
The goal is to provide a minimalist runtime for running PS2 games. It is not meant to be generalist, but meant to be used as an aid in making PS2 games run on multiple platforms.

# Coding style
Documentation should not live in code. The code should be simple, boring and self-explanatory. A comment should never describe how or what something is doing. The only comments important to add are:
- Why this code does what it does, which can not be inferred from the code alone?
- Source attribution. For reverse engineered & reimplemented code, it is essential that attribution is provided in the form of module name, hash, address and symbol name.
- Functions should be verbs (isActive(), getStateName())

# Tests
To ensure correctness, we make use of tests. The most important tests are:
- Autotests. These are behavioural tests that have published results as produced on real hardware, and are an authoritative source for how the simulated hardware should perform.
- "Vector" tests. These are synthetic behavioural tests, similar to autotests, but these is based on behavior observed high-accuracy emulators such as PCSX2's interpreter modes.
