"""Compile the production Look renderer with deterministic headless UI actions.

Only the presentation include is substituted; control/commit logic stays exact.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / "OptiScaler/menu/NextgenLookControls.h").read_text(encoding="utf-8-sig")
include = '#include "UiLocalization.h"'
assert source.count(include) == 1
out = ROOT / "build/common-core-tests/LookControlsProduction.h"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(source.replace(include, "// Headless UI supplied by fixture."), encoding="utf-8")

