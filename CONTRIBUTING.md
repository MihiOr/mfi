# Development

Run the host suite before changing packet grammar or handshake behavior:

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

For transport changes, also compile the Cortex-M3 backend and run `tests/arm_transport_tests.py` as described in [docs/porting.md](docs/porting.md). Add independent wire vectors or observable peer behavior for changes to CRC, field widths, GPIO ordering, fault handling, or direction reversal.

Keep application commands, board-specific services, serial transport, printing, and delay calls out of the protocol. Describe any wire-incompatible change explicitly. The repository's copyright terms apply to all contributions; contact the owner before submitting code.
