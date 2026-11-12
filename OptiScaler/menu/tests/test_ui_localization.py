"""Check production UI translations without building/loading the F6 DLL.

Run with Python. Pass --runtime to compile and run UiLocalizationTests.cpp
with MSVC (uses vswhere, or --vcvars for an explicit vcvars64.bat).
"""
import argparse
import ast
from collections import Counter
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
MENU = ROOT / "OptiScaler/menu"
LITERAL = r'"(?:\\.|[^"\\])*"'
TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|\'(?:\\.|[^\'\\])*\'|' + LITERAL)
PAIR = re.compile(r'\{\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r')\s*\}')
HAN = re.compile(r'[\u3400-\u9fff]')
PRINTF = re.compile(r'%(?:%|[-+#0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|I64|I32|[hljztL])?[diuoxXfFeEgGaAcspn])')


def read(path):
    return path.read_text(encoding="utf-8-sig")


def literals(source):
    """Ignore comments/character literals and join adjacent C++ strings."""
    previous = None
    for token in TOKEN.finditer(source):
        if not token.group().startswith('"'):
            continue
        value = ast.literal_eval(token.group())
        if previous and not source[previous[1]:token.start()].strip():
            previous = (previous[0], token.end(), previous[2] + value)
        else:
            if previous:
                yield previous
            previous = (token.start(), token.end(), value)
    if previous:
        yield previous


def check_sources(dictionary):
    # Inventory files whose Chinese literals flow through MenuText. Inline
    # bilingual helpers are validated separately instead of adding dead entries.
    paths = [MENU / name for name in (
        "menu_common.cpp", "GuideDiagnosticsMenu.cpp", "CriticalErrorWindow.h",
        "GenshinFsrSubstageUi.h", "NextgenNrLayers.h", "HoYoShadeControls.h",
        "NextgenLookControls.h", "NextgenF5Panels.h", "NextgenUi.cpp", "MenuPages.h")]
    paths += [ROOT / "OptiScaler/dlssnr/DlssNr_Menu.cpp", ROOT / "OptiScaler/uid/DrawUidPanel.h"]
    failures = []
    count = 0
    for path in paths:
        source = read(path)
        bilingual = {}
        for pair in re.finditer(r'\b(?:tr|text|Label)\(\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r')', source):
            bilingual[ast.literal_eval(pair[1])] = ast.literal_eval(pair[2])
        # Navigation metadata and Look parameter tables store adjacent CN/EN
        # columns after their stable machine name. Do not translate that name.
        # Only these typed rows actually consume explicit CN/EN columns.
        # An ordinary option array such as {"GPU完成", "Reset"} is not bilingual.
        metadata = []
        if path.name == "MenuPages.h":
            metadata = re.findall(r'\{\s*' + LITERAL + r'\s*,\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r')', source)
        elif path.name == "NextgenLookControls.h":
            metadata = re.findall(r'\{\s*' + LITERAL + r'\s*,\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r')\s*,\s*&', source)
        for cn, en in metadata:
            bilingual[ast.literal_eval(cn)] = ast.literal_eval(en)
        # NR's existing display-only reverse map keeps backend reason tokens
        # intact in English mode and translates them only for Chinese display.
        reverse = re.search(r'words\[\]\s*=\s*\{([\s\S]*?)\n\s*\};', source)
        if reverse:
            for en, cn in PAIR.findall(reverse[1]):
                bilingual[ast.literal_eval(cn)] = ast.literal_eval(en)
        for match in re.finditer(r'(?:NextgenUi::English\(\)|\benglish)\s*\?\s*(' + LITERAL + r')\s*:\s*(' + LITERAL + r')', source):
            bilingual[ast.literal_eval(match[2])] = ast.literal_eval(match[1])
        for start, end, value in literals(source):
            visible = value.split("##", 1)[0]
            if not HAN.search(visible):
                continue
            # Existing GIMI input identities are hidden with ### by its input()
            # helper; their adjacent captions already use explicit EN/CN pairs.
            if path.name == "menu_common.cpp" and value in {
                "INI 绝对路径##gimi", "组件节名##gimi", "精确绑定##gimi", "依赖 INI 路径##gimi"}:
                continue
            if path.name == "NextgenLookControls.h" and value in {"第 ", " 层 Look"}:
                assert 'NextgenUi::English()?"Layer "+std::to_string(layer+1)+" Look"' in source
                continue
            english = dictionary.get(value, dictionary.get(visible, bilingual.get(value)))
            if english is None or HAN.search(english.split("##", 1)[0]):
                failures.append(f"{path.relative_to(ROOT)}:{source.count(chr(10), 0, start) + 1}: {value}")
            count += 1
        # Direct text/label draws must use MenuText or explicit bilingual helpers.
        for match in re.finditer(r'ImGui::(?:Text(?:Wrapped|Disabled|Unformatted)?|SetTooltip|TreeNode|CollapsingHeader|Begin|TableSetupColumn)\(\s*(' + LITERAL + r')', source):
            visible = ast.literal_eval(match[1]).split("##", 1)[0]
            if HAN.search(visible):
                failures.append(f"{path.relative_to(ROOT)}: raw ImGui Chinese label: {visible}")
    assert not failures, "Missing UI translations:\n" + "\n".join(failures)
    return count


def check_dictionary(source=None):
    if source is None:
        source = read(MENU / "UiLocalization.h")
    section = source.split("Dictionary[] = {", 1)[1].split("\n};", 1)[0]
    pairs = [(ast.literal_eval(cn), ast.literal_eval(en)) for cn, en in PAIR.findall(section)]
    duplicates = [key for key, n in Counter(cn for cn, en in pairs).items() if n > 1]
    assert not duplicates, f"Duplicate translation keys: {duplicates}"
    for cn, en in pairs:
        assert PRINTF.findall(cn) == PRINTF.findall(en), f"printf argument/percent mismatch: {cn} -> {en}"
        assert not HAN.search(en.split("##", 1)[0]), f"Chinese remains in English display: {en}"
        if "##" in cn:
            assert cn.split("##", 1)[1] == en.split("##", 1)[1], f"ImGui ID changed: {cn} -> {en}"
    return dict(pairs)


def check_negative_controls(dictionary):
    source = read(MENU / "UiLocalization.h")
    mutations = (
        source.replace('"Source: %.1f Hz"', '"Source: %u Hz"', 1),
        source.replace('"NR per-frame detail retention debug###NrHistoryPerFrame"',
                       '"NR per-frame detail retention debug###ChangedIdentity"', 1),
        source.replace('"NR per-frame detail retention debug###NrHistoryPerFrame"',
                       '"未翻译###NrHistoryPerFrame"', 1),
    )
    for changed in mutations:
        assert changed != source, "Negative-control target missing"
        try:
            check_dictionary(changed)
        except AssertionError:
            continue
        raise AssertionError("Dictionary checker accepted a broken translation/format/identity")
    for missing in ("持续保留：深度容差", "GPU完成", "细节持续反馈"):
        incomplete = dict(dictionary)
        del incomplete[missing]
        try:
            check_sources(incomplete)
        except AssertionError:
            continue
        raise AssertionError("UI inventory accepted a missing translation: " + missing)


def check_wiring():
    guide = read(MENU / "GuideDiagnosticsMenu.cpp")
    for text in ('MenuText::Begin("NR 每帧细节保留调试###NrHistoryPerFrame"',
                 'MenuText::TableSetupColumn(name)', 'MenuText::TreeNode("消费者详情与拒绝原因")'):
        assert text in guide, f"Localised history/guide draw missing: {text}"
    common = read(MENU / "menu_common.cpp")
    assert 'cache.english!=NextgenUi::English()' in common, "Monitor cache does not respond to a language change"
    assert 'notification.setContentProvider([filename]' in common, "Startup toast does not retain its formatting source"
    assert '+[](const char* text) { return MenuText::Translate(text ? text : ""); }' in common, "Toast render bypasses current language"
    notify = read(ROOT / "OptiScaler/include/imgui/ImGuiNotify.hpp")
    assert 'const auto titleText = currentToast->resolveTitle(translate);' in notify
    assert 'const auto contentText = currentToast->resolveContent(translate);' in notify
    nr = read(ROOT / "OptiScaler/dlssnr/DlssNr_Menu.cpp")
    assert 'ImGuiToastType::Info, 15000, "%s",' in nr and 'MenuText::Translate("F6 当前未接入' in nr, "F5Low toast bypasses localisation"
    # Keep exact technical/model choices and their original config mapping.
    assert 'static const char* srModelLabels[] = { "默认", "J", "K", "L", "M" };' in common
    assert 'static const char* modelPresets[] = { "驱动默认", "A", "B", "C" };' in common
    assert 'config->FGDLSSGModelPreset = static_cast<uint32_t>(modelPreset)' in common


def run_runtime(vcvars):
    if not vcvars:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        assert vswhere.is_file(), "Use --vcvars to specify vcvars64.bat"
        installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
        vcvars = str(Path(installation) / "VC/Auxiliary/Build/vcvars64.bat")
    assert Path(vcvars).is_file(), f"Missing vcvars: {vcvars}"
    # cmd's /c grammar needs its own outer quotes; list2cmdline's backslash
    # quoting is for C argv and is not a substitute for cmd quoting here.
    output = subprocess.check_output(
        f'cmd.exe /d /s /c "chcp 65001 >nul && call "{Path(vcvars)}" >nul && set"', encoding="utf-8")
    env = dict(os.environ)
    for line in output.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            env[key] = value
    compiler = shutil.which("cl.exe", path=env.get("Path", env.get("PATH", "")))
    assert compiler, "MSVC compiler unavailable after vcvars"
    # A temporary directory gives this worker its own objects/executable. It
    # never touches the DLL build, common suite, deployed files or game config.
    with tempfile.TemporaryDirectory(prefix="f6-ui-localization-") as directory:
        target = Path(directory) / "UiLocalizationTests.exe"
        subprocess.run([compiler, "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/W4", "/O2", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
                        "/I" + str(ROOT / "OptiScaler"), "/I" + str(ROOT / "OptiScaler/include"),
                        str(MENU / "tests/UiLocalizationTests.cpp"), "/Fe:" + str(target),
                        "/Fo:" + str(Path(directory) / "UiLocalizationTests.obj")],
                       cwd=directory, env=env, check=True)
        subprocess.run([str(target)], cwd=directory, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--vcvars")
    args = parser.parse_args()
    dictionary = check_dictionary()
    count = check_sources(dictionary)
    check_wiring()
    check_negative_controls(dictionary)
    print(f"PASS: {len(dictionary)} dictionary entries; {count} Chinese UI literals covered; monitor/toast/model wiring preserved; 6 negative controls detected", flush=True)
    if args.runtime:
        run_runtime(args.vcvars)
