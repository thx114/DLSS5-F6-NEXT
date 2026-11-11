# Runtime observation tools

`Collect-F6DisplayTiming.ps1` optionally observes a selected process using
an explicitly supplied PresentMon executable, or analyzes an existing CSV.
It reports display timing separately from source rate and SDK counters;
missing display evidence remains unknown.

`Analyze-F6GuidePair.py` reads an exported `f6.guide-pair.v1` snapshot and
can emit offline reprojection/error reports. It does not change live settings.
Use each tool's help for arguments. Store local outputs under `build/`.
