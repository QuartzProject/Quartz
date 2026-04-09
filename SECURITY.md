# Security

Quartz exists to run code its host does not trust, so a way out of the sandbox is the most serious kind of bug it can
have.

## What counts as a vulnerability

- A Quartz program, or a `.qzb` module, that reads or writes memory outside what the validator proved, calls a host
  function it was not given, or crashes, hangs or exhausts the host process (as opposed to trapping in its sandbox).
- Fuel, memory quota or recursion limits that can be bypassed, in either the interpreter or the native tier.
- A module accepted with a bad hash, or a signed module accepted with a signature from a key the host did not trust.
- A difference between the interpreter and the native tier that a program can observe and exploit.

Bugs in the compiler front end that produce wrong diagnostics are ordinary bugs: the validator, not the front end, is
the security boundary.

## Reporting

Please report privately through the repository's security advisories ("Report a vulnerability" on GitHub) rather than
in a public issue, with a program or module that shows the problem if you have one. You will get an answer, and a fix
will be released before the details are made public.

## Supported versions

Before 1.0 only the latest release receives security fixes.
