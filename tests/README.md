# Tests

The tests are in Python, on the shipped C code through its bindings: a server and a client, in one
process, on a simulated link with a clock of its own (`simulator.py`). Their tools are in the `dev` group.

```bash
uv run pytest
```

An editable install is not rebuilt by itself: install again after changing the C code. `ctest` tests
its own build, and stops if an editable install is found first.

`test_js_client.py` runs `js/dfukit.js` in Node against the C server, and is skipped where there is
no Node.

`pytest --cov` measures only Python. To measure the C code, build with coverage on, run the tests
without rebuilding, and read the counters with `gcovr`:

```bash
uv pip install -e . -C cmake.define.DFUKIT_COVERAGE=ON -C build-dir=build
uv run --no-sync pytest
uv run --no-sync gcovr --root . build --filter 'src/' --filter 'backends/ram/' --filter 'lib/crc32/' \
    --gcov-ignore-errors=no_working_dir_found
```
