"""Exercise the production FG pacing admission with fresh and absent SR input.

The CPU fixture extracts the actual lambda and uses the real pacing policy.
No game, graphics API or SDK is loaded. --legacy-gap-pacing must fail.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--legacy-gap-pacing", action="store_true")
args = parser.parse_args()
source = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text(encoding="utf-8-sig")
begin = source.index("const auto fgPacingActive = [&]() {")
end = source.index("\n    };", begin) + len("\n    };")
admission = source[begin:end]
if args.legacy_gap_pacing:
    admission = admission.replace(
        "const bool runtimePacing = !starRailSourceFallback;",
        "const bool runtimePacing = !starRailSourceFallback || starRailF5DsScheduling;",
    )
fixture = r'''
#include <framegen/FgSourcePacing.h>
#include <cstdio>
#include <stdexcept>
struct Setting { bool value = true; bool value_or_default() const { return value; } };
struct Config { Setting FGEnabled; };
struct Feature {
    bool active = true, paused = false;
    bool IsActive() const { return active; }
    bool IsPaused() const { return paused; }
};
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
int main() try {
    Config configuration; auto* config = &configuration;
    Feature feature; auto* fg = &feature;
    struct { Feature* currentFG; } state{fg};
    bool starRailSourceFallback = false, starRailSourceFailed = false;
    [[maybe_unused]] bool starRailF5DsScheduling = true;
    // INSERT_ADMISSION
    const auto decision = [&] {
        return framegen::SelectSourcePacing({framegen::PacingRole::Source,
            true, true, true, fgPacingActive(), true, true,
            false, false, false, true, false, 5});
    };
    Check(decision().reflexSleep && decision().generatedFrames == 5,
          "fresh real input retains the requested FG pacing");
    starRailSourceFallback = true;
    for (unsigned i = 0; i != 300; ++i) {
        const auto d = decision();
        Check(!d.fgActive && !d.reflexSleep && d.generatedFrames == 0,
              "retained provider cannot pace a missing source on the frozen SR token");
        Check(d.frameLimit, "an explicit base-frame cap remains available during a gap");
    }
    Check(feature.active && configuration.FGEnabled.value,
          "input gaps preserve provider residency and the user's FG selection");
    starRailSourceFallback = false;
    Check(decision().reflexSleep && decision().generatedFrames == 5,
          "a fresh source restores normal pacing immediately");
    starRailSourceFailed = true;
    Check(!decision().reflexSleep, "a failed source cannot sleep");
    starRailSourceFailed = false;
    feature.paused = true;
    Check(!decision().fgActive, "paused provider remains inactive");
    feature.paused = false;
    configuration.FGEnabled.value = false;
    Check(!decision().fgActive, "user Off remains inactive");
    configuration.FGEnabled.value = true;
    Feature other; state.currentFG = &other;
    Check(!decision().fgActive, "a replaced provider cannot pace");
    state.currentFG = fg;
    starRailF5DsScheduling = false;
    Check(decision().reflexSleep, "ordinary non-gap providers retain their pacing");
    feature.active = false;
    Check(!decision().fgActive, "inactive provider remains inactive");
    fg = nullptr;
    Check(!decision().fgActive, "absent provider remains inactive");
    std::printf("PASS %u source-gap pacing CPU checks\n", checks);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
}
'''.replace("// INSERT_ADMISSION", admission)
with tempfile.TemporaryDirectory(prefix=".gap-pacing-", dir=HERE, ignore_cleanup_errors=True) as temporary:
    directory = Path(temporary)
    cpp = directory / "fixture.cpp"
    cpp.write_text(fixture, encoding="utf-8")
    exe = directory / "fixture.exe"
    batch = directory / "compile.cmd"
    batch.write_text(
        '@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'"{exe}"\nexit /b %errorlevel%\n', encoding="utf-8", newline="\r\n",
    )
    completed = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory)
raise SystemExit(completed.returncode)
