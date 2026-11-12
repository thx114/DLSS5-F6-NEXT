"""Run isolated F6 input regressions against repository ImGui and production code.

python OptiScaler/menu/tests/test_menu_input.py [--vcvars PATH]
Only OS acquisition/window services are seams. Extracted C++ is compiled, never
reimplemented here. No product build, window creation, game access or ASI loading.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
IMGUI = ROOT / "OptiScaler/include/imgui"
INPUT = ROOT / "OptiScaler/menu/input"


def mask_cpp(source):
    return re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: re.sub(r'[^\n]', ' ', m[0]), source)


def block(path, signature, semicolon=False):
    """Extract a unique definition with balanced braces, ignoring C++ literals/comments."""
    source = path.read_text(encoding="utf-8-sig")
    masked = mask_cpp(source)
    matches = list(re.finditer(signature, masked, re.M))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one definition of {signature!r} in {path}, got {len(matches)}")
    start = matches[0].start()
    opening = masked.index("{", matches[0].end())
    depth = 1
    end = opening + 1
    while depth and end < len(masked):
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if depth or (semicolon and masked[end:end + 1] != ";"):
        raise RuntimeError(f"Incomplete definition in {path}")
    end += int(semicolon)
    line = source.count("\n", 0, start) + 1
    return f'#line {line} "{path.as_posix()}"\n' + source[start:end] + "\n"


def extract(directory):
    backend = IMGUI / "imgui_impl_win32.cpp"
    parts = [block(backend, r'^struct ImGui_ImplWin32_Data\b', True)]
    for signature in (
        r'^static ImGui_ImplWin32_Data\* ImGui_ImplWin32_GetBackendData\(\)',
        r'^static ImGui_ImplWin32_Data\* ImGui_ImplWin32_GetBackendData\(ImGuiIO& io\)',
        r'^static bool IsVkDown\(int vk\)',
        r'^static void ImGui_ImplWin32_AddKeyEvent\(',
        r'^static void ImGui_ImplWin32_ProcessKeyEventsWorkarounds\(',
        r'^static ImGuiViewport\* ImGui_ImplWin32_FindViewportByPlatformHandle\(',
        r'^static void ImGui_ImplWin32_UpdateMouseData\(',
        r'^static void ImGui_ImplWin32_UpdateGamepads\(',
        r'^void\s+ImGui_ImplWin32_NewFrame\(\)',
    ):
        parts.append(block(backend, signature))
    (directory / "production_backend.inc").write_text("\n".join(parts), encoding="utf-8")
    cpp = INPUT / "input_system.cpp"
    messages = INPUT / "input_system_messages.cpp"
    xinput = INPUT / "input_system_xinput.cpp"
    definitions = [(INPUT / "input_system_internal.h", r'^class ScopedHookBypass\b', True)]
    definitions += [(cpp, rf'^bool {name}\(', False) for name in (
        "ShouldApplyBlockingPolicyLocked", "ShouldBlockKeyboardInputLocked", "ShouldBlockMouseInputLocked")]
    definitions += [
        (messages, r'^bool IsMouseVirtualKey\(', False),
        (cpp, r'^bool ShouldBlockVirtualKey\(', False),
        (messages, r'^SHORT WINAPI hkGetKeyState\(', False),
        (xinput, r'^bool ShouldBlockXInputLocked\(', False),
        (xinput, r'^DWORD WINAPI hkXInputGetState\(', False),
        (cpp, r'^void NewImGuiWin32Frame\(', False),
        (cpp, r'^void FeedImGui\(', False),
    ]
    code = "\n".join(block(path, signature, semi) for path, signature, semi in definitions)
    (directory / "production_input.inc").write_text(code, encoding="utf-8")
    # Mutation of the extracted *production* fast-click expression, not a
    # separate test implementation. Used by a runtime negative-control switch.
    old = "_state.MouseButtons[mb].Down || _state.MouseButtons[mb].Pressed"
    if code.count(old) != 1:
        raise RuntimeError("Fast-click mutation target changed; review this negative control")
    mutated = code.replace("void FeedImGui(bool menuVisible)", "void LostFastClickFeed(bool menuVisible)")
    start = mutated.index("void LostFastClickFeed(")
    mutated = mutated[start:].replace(old, "_state.MouseButtons[mb].Down")
    (directory / "lost_fast_click.inc").write_text(mutated, encoding="utf-8")
    # Execute the actual overlay function with inert D3D/renderer services.
    # Its old shape is reconstructed by the single deleted bare platform poll.
    overlay = block(ROOT / "OptiScaler/menu/menu_overlay_dx.cpp", r'^static void RenderImGui_DX11\(')
    renderer = "ImGui_ImplDX11_NewFrame();"
    if overlay.count(renderer) != 1:
        raise RuntimeError("DX11 renderer frame boundary changed; review extraction")
    old_overlay = overlay.replace("RenderImGui_DX11(", "OldRenderImGui_DX11(", 1)
    old_overlay = old_overlay.replace(renderer, renderer + "\n            ImGui_ImplWin32_NewFrame();", 1)
    (directory / "production_dx11_overlay.inc").write_text(overlay + "\n" + old_overlay, encoding="utf-8")
    digest = hashlib.sha256(("".join(parts) + code + overlay).encode()).hexdigest()[:16]
    print(f"Extracted real backend/input functions: SHA256 {digest}", flush=True)


def platform_inventory(overrides=None):
    """Inventory live production calls, excluding vendor ImGui and test fixtures."""
    overrides = overrides or {}
    calls = []
    pattern = re.compile(r'\b(ImGui_ImplWin32_NewFrame|NewImGuiWin32Frame|ImGui_ImplUwp_NewFrame)\s*\(')
    product = ROOT / "OptiScaler"
    for path in product.rglob("*"):
        relative = path.relative_to(product)
        if path.suffix not in {".cpp", ".h", ".hpp"} or "tests" in relative.parts or relative.parts[:2] == ("include", "imgui"):
            continue
        source = overrides.get(path)
        if source is None:
            # Some bundled headers use legacy comment encodings. Target C++
            # identifiers/punctuation are ASCII; preserve undecodable bytes.
            source = path.read_text(encoding="utf-8-sig", errors="surrogateescape")
        if not any(name in source for name in ("NewImGuiWin32Frame", "ImGui_ImplWin32_NewFrame", "ImGui_ImplUwp_NewFrame")):
            continue
        masked = mask_cpp(source)
        for match in pattern.finditer(masked):
            prefix = masked[masked.rfind("\n", 0, match.start()) + 1:match.start()]
            # Skip C++ declarations/definitions, but count qualified calls.
            if re.search(r'\bvoid\s*$', prefix):
                continue
            calls.append((relative.as_posix(), match[1]))
    expected = [
        ("menu/input/input_system.cpp", "ImGui_ImplWin32_NewFrame"),
        ("menu/menu_common.cpp", "ImGui_ImplUwp_NewFrame"),
        ("menu/menu_common.cpp", "NewImGuiWin32Frame"),
    ]
    if sorted(calls) != expected:
        raise RuntimeError(f"Platform call inventory has extra/missing owner calls: {sorted(calls)}")
    return calls


def check_wiring():
    """Supplement executable coverage with production caller-boundary checks."""
    common = ROOT / "OptiScaler/menu/menu_common.cpp"
    render = block(common, r'^bool MenuCommon::RenderMenu\(\)')
    calls = re.findall(r'\b(HandleMenuShortcuts|UpdateMenuInputMode|BeginMenuFrameIfNeeded)\(ctx\)', render)
    if calls != ["HandleMenuShortcuts", "UpdateMenuInputMode", "BeginMenuFrameIfNeeded"]:
        raise RuntimeError(f"Menu opening policy/frame order changed: {calls}")
    frame = block(common, r'^void MenuCommon::BeginMenuFrameIfNeeded\(')
    if not re.search(r'if\s*\(\s*!_isUWP\s*\)\s*\{\s*OptiInput::NewImGuiWin32Frame\(\);\s*\}\s*else', frame):
        raise RuntimeError("Non-UWP backend must run through the production bypass wrapper")
    if not (frame.index("OptiInput::NewImGuiWin32Frame();") < frame.index("OptiInput::FeedImGui(_isVisible);")
            < frame.index("ImGui::NewFrame();")):
        raise RuntimeError("Backend/FeedImGui/core order changed")
    policy = block(common, r'^void MenuCommon::UpdateMenuInputMode\(')
    if "MenuInputPolicy::SetVisible(io, _isVisible);" not in policy or "BackendFlags" in policy:
        raise RuntimeError("Visibility caller bypasses policy or overwrites backend capabilities")
    source = common.read_text(encoding="utf-8-sig")
    if re.search(r'\bWantSetMousePos\s*=|\bhasGamepad\b', source):
        raise RuntimeError("Application cursor request or cached gamepad ownership reintroduced")
    calls = platform_inventory()
    for path, name in sorted(calls):
        print(f"Platform call inventory: {path}: {name}", flush=True)
    overlay_path = ROOT / "OptiScaler/menu/menu_overlay_dx.cpp"
    overlay = overlay_path.read_text(encoding="utf-8-sig")
    function = block(overlay_path, r'^static void RenderImGui_DX11\(')
    if not (function.index("ImGui_ImplDX11_NewFrame();") < function.index("MenuOverlayBase::RenderMenu();")):
        raise RuntimeError("DX11 renderer/core frame order changed")
    mutated = overlay.replace("ImGui_ImplDX11_NewFrame();", "ImGui_ImplDX11_NewFrame();\nImGui_ImplWin32_NewFrame();", 1)
    if mutated == overlay:
        raise RuntimeError("DX11 call inventory mutation target missing")
    try:
        platform_inventory({overlay_path: mutated})
    except RuntimeError as error:
        if not str(error).startswith("Platform call inventory has extra/missing owner calls:"):
            raise
    else:
        raise RuntimeError("Platform inventory accepted the old DX11 bare pre-poll")
    print("PASS: production platform call inventory; old DX11 bare pre-poll rejected", flush=True)
    print("PASS: production shortcut -> visibility policy -> backend -> FeedImGui -> core wiring; UWP kept separate", flush=True)


def run(vcvars):
    # Same vswhere/vcvars discovery convention as test_ui_localization.py.
    if not vcvars:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True, timeout=30).strip()
        vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
    vcvars = Path(vcvars).resolve()
    if not vcvars.is_file() or any(char in str(vcvars) for char in '\"\r\n%'):
        raise RuntimeError(f"Invalid vcvars path: {vcvars}")
    output = subprocess.check_output(
        f'cmd.exe /d /s /c "chcp 65001 >nul && call "{vcvars}" >nul && set"', encoding="utf-8", timeout=30)
    env = dict(os.environ)
    for line in output.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            env[key] = value
    compiler = shutil.which("cl.exe", path=env.get("Path", env.get("PATH", "")))
    if not compiler:
        raise RuntimeError("MSVC compiler unavailable after vcvars")
    with tempfile.TemporaryDirectory(prefix="f6-menu-input-") as temp:
        directory = Path(temp)
        extract(directory)
        exe = directory / "MenuInputTests.exe"
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/W4", "/O2", "/MT",
                   "/DIMGUI_DISABLE_WIN32_FUNCTIONS", "/DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS",
                   "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/I" + str(ROOT / "OptiScaler"),
                   "/I" + str(ROOT / "OptiScaler/include"), "/I" + str(ROOT / "external/freetype"),
                   "/I" + str(IMGUI), "/I" + str(directory),
                   str(HERE / "MenuInputTests.cpp")]
        command += [str(IMGUI / name) for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp")]
        command += [str(IMGUI / "misc/freetype/imgui_freetype.cpp"), str(ROOT / "external/freetype/freetype.lib")]
        command += ["/Fe:" + str(exe)]
        subprocess.run(command, cwd=directory, env=env, check=True, timeout=180)
        subprocess.run([str(exe)], cwd=directory, check=True, timeout=30)
        controls = {
            "unbypassed-shift": "SHIFT_STABLE",
            "unbypassed-gamepad": "PAD_STATE",
            "lost-fast-click": "FAST_CLICK",
            "old-visibility": "CONFIG_PRESERVED",
            "dx11-double-sample": "ONE_PLATFORM_SAMPLE",
            "dx11-short-delta": "FRAME_DELTA",
            "dx11-unbypassed-shift": "DX11_SHIFT_STABLE",
            "dx11-unbypassed-gamepad": "DX11_PAD_STABLE",
        }
        for mode, failure in controls.items():
            result = subprocess.run([str(exe), mode], cwd=directory, capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            if result.returncode != 1 or f"FAIL [{failure}]" not in output:
                raise RuntimeError(f"Negative control {mode} failed for an unexpected reason: {output}")
            print(f"Negative control detected: {mode}: {output.strip()}", flush=True)
        print(f"PASS: {len(controls)} executable negative controls rejected", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars", help="Explicit vcvars64.bat; otherwise use repository discovery")
    # Consistent with adjacent test entrypoints, but runtime is always enabled.
    parser.add_argument("--runtime", action="store_true", help=argparse.SUPPRESS)
    check_wiring()
    run(parser.parse_args().vcvars)
