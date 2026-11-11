# Look shader generation

Normal DLL builds use the embedded shader headers already in the source tree.
Regeneration is only needed when changing the corresponding production HLSL.

Run `python ./tools/shaders/regenerate-look-shaders.py --help` for arguments.
`--source` must be the absolute path to this checkout's `OptiScaler` directory.
Supply fresh output directories under `build/shaders/evidence/` and
`build/shaders/backups/` for `--evidence` and `--cleanup` respectively.
The generator uses the source-local DXC by default, validates both compiled
formats and preserves preexisting headers before replacement.
Generated validation outputs and backups are local build artifacts.
