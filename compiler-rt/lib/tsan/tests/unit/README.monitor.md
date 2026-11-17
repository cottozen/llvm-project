# Monitor trace tests

The C++ trace tests read `data/cycles.txt` and `data/no_cycles.txt` directly,
using the same format and cycle expectations as the Python monitor tests.
No generated C++ fixtures or Python installation are needed to run them.
Changes to the trace files take effect on the next test run without rebuilding.

Supported notation matches `monitor.parser`: `<START [tag]>` / `<END [tag]>`
blocks, `#` comments, numeric or `t`-prefixed thread IDs, numeric or `l`-prefixed
lock IDs, `alias p t1`, `begin`/`b`, `end`/`e`, `acq`/`a`, `rel`/`r`,
`acq-rel`, and `<` handoffs. Aliases and synthetic lock IDs reset per block.
A block whose tag contains `CYCLE` (case insensitive) must detect a cycle;
all other blocks must finish without a conflict. Replay stops at the first
conflict, matching the Python simulator. Missing, empty, or malformed inputs
fail the tests.

The input directory defaults to this repository's `data` directory, resolved
at CMake configuration time so the tests run from any working directory.
Override it with `-DTSAN_MONITOR_TRACE_DIR=/absolute/path/to/data` when configuring
CMake to use a different checkout's inputs.

From the repository root, build and run all monitor tests:

```sh
bash src/tsan/test.sh
```

Or run just the shared traces:

```sh
bash src/tsan/test.sh 'SharedPythonTraces/*'
```

These shared traces check cycle detection, like `tests/test_monitor.py`.
Monitor unit tests separately check specific conflict kinds. Standalone tests
link the runtime built in this checkout and rebuild that runtime first.
