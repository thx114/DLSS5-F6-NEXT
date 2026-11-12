"""Stage an isolated F6 build with per-game profiles, then select it via Hub CLI.

The plan supplies existing, verified payloads. Existing installations are never
rewritten, and no game process is queried or controlled. CLI selection changes
persistent settings; a running Hub may still need a full restart. ASI plugins
remain in explicitly referenced user-owned directories and are never packaged.
"""
from __future__ import annotations

import argparse
import configparser
from contextlib import closing
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess


def save(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


class Deployment:
    def __init__(self, plan_path: Path):
        self.plan_path = plan_path.resolve()
        self.plan = load(plan_path)
        self.evidence = plan_path.resolve().parent
        self.prefix = self.plan.get("EvidencePrefix", "three-game-asi")
        if not re.fullmatch(r"[A-Za-z0-9_-]+", self.prefix):
            raise ValueError("Invalid evidence prefix")
        self.hub = Path(self.plan["InstallationRoot"]).resolve()
        self.build_id = self.plan["BuildId"]
        if not re.fullmatch(r"local/[A-Za-z0-9_-]+", self.build_id):
            raise ValueError("BuildId must name one local build")
        self.package = self.hub / "OptiScaler" / self.build_id
        self.runtime_id = hashlib.sha256(self.build_id.encode()).hexdigest().upper()[:16]
        self.cache = {}
        games = self.plan["Games"]
        if not games:
            raise ValueError("Plan must contain at least one game")
        if len({game["Biz"] for game in games}) != len(games):
            raise ValueError("Duplicate game")
        for game in games:
            if not re.fullmatch(r"[a-z0-9_]+", game["Biz"]):
                raise ValueError("Invalid game key")

    def hash(self, path: Path) -> str:
        stat = path.stat()
        key = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
        if key not in self.cache:
            with path.open("rb") as stream:
                self.cache[key] = hashlib.file_digest(stream, "sha256").hexdigest().upper()
        return self.cache[key]

    def settings(self):
        path = self.hub / "HoYoShadeHubDatabase.db"
        with closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as database:
            return dict(database.execute("SELECT Key,Value FROM Setting"))

    def evidence_file(self, suffix):
        return self.evidence / (self.prefix + "-" + suffix)

    def runtime(self, game):
        return self.hub / "cache/games" / game["Biz"] / "OptiRuntime" / self.runtime_id

    def user_plugins(self, game):
        value = game.get("UserPluginsPath", self.plan.get("UserPluginsPath"))
        if not isinstance(value, str) or not value.strip():
            raise ValueError("Plan requires an explicit UserPluginsPath (global or per game); "
                             "legacy bundled-plugin plans require a new plan and build directory")
        path = Path(value)
        if not path.is_absolute() or not path.is_dir():
            raise ValueError("UserPluginsPath must name an existing absolute user-owned directory")
        path = path.resolve()
        for target in [self.package, *(self.runtime(g) for g in self.plan["Games"])]:
            if path.is_relative_to(target.resolve()) or target.resolve().is_relative_to(path):
                raise ValueError("UserPluginsPath must be separate from the generated package and runtimes")
        return path

    @staticmethod
    def require_no_bundled_plugins(root):
        for path in root.rglob("*"):
            if path.is_file() and path.suffix.lower() == ".asi":
                raise ValueError(f"Bundled ASI is unsupported: {path}; keep user plugins outside "
                                 "the generated package/runtimes and create a new plan/build directory")

    @staticmethod
    def config(data: bytes):
        parser = configparser.ConfigParser(interpolation=None, strict=False)
        parser.read_string(data.decode("utf-8-sig"))
        return {(section.lower(), key.lower()): value for section in parser.sections()
                for key, value in parser[section].items()}

    @staticmethod
    def patch(data: bytes, changes):
        text = data.decode("utf-8-sig")
        lines = text.splitlines(keepends=True)
        section = ""
        seen = set()
        for index, line in enumerate(lines):
            match = re.match(r"\s*\[([^\]]+)\]", line)
            if match:
                section = match[1].lower()
            match = re.match(r"(\s*([^;#=]+?)\s*=\s*)(.*?)(\r?\n)?$", line)
            if not match:
                continue
            key = (section, match[2].strip().lower())
            if key in changes:
                if key in seen:
                    raise ValueError(f"Duplicate configuration key: {key}")
                lines[index] = match[1] + changes[key] + (match[4] or "")
                seen.add(key)
        if seen != changes.keys():
            raise ValueError(f"Missing configuration keys: {changes.keys() - seen}")
        result = "".join(lines).encode("utf-8")
        before, after = Deployment.config(data), Deployment.config(result)
        unexpected = {key for key in before.keys() | after.keys()
                      if before.get(key) != after.get(key) and key not in changes}
        if unexpected:
            raise ValueError(f"Unexpected configuration edits: {unexpected}")
        return result

    def copy_new(self, source: Path, target: Path, *, link=False):
        if source.suffix.lower() == ".asi" or target.suffix.lower() == ".asi":
            raise ValueError("ASI plugins must remain in the user-owned directory")
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise FileExistsError(target)
        if link and source.stat().st_dev == target.parent.stat().st_dev:
            os.link(source, target)
        else:
            with source.open("rb") as reader, target.open("xb") as writer:
                shutil.copyfileobj(reader, writer)
                writer.flush()
                os.fsync(writer.fileno())
            shutil.copystat(source, target)
        if self.hash(source) != self.hash(target):
            raise ValueError(f"Copy differs: {target}")

    def protected(self, record):
        for file in record["ProtectedFiles"]:
            path = Path(file["Path"])
            if path.stat().st_ino != file["FileId"] or self.hash(path) != file["SHA256"]:
                raise ValueError(f"Existing protected file changed: {path}")

    def prepare(self):
        plugins = {g["Biz"]: self.user_plugins(g) for g in self.plan["Games"]}
        if self.package.exists() or any(self.runtime(g).exists() for g in self.plan["Games"]):
            raise FileExistsError("New package or runtime already exists")
        artifact = Path(self.plan["Artifact"])
        if self.hash(artifact) != self.plan["SHA256"]:
            raise ValueError("Artifact differs from reviewed build")
        manifest = load(Path(self.plan["SourceManifest"]))
        if manifest["SourceCommit"] != self.plan["SourceCommit"]:
            raise ValueError("Source identity mismatch")
        root = Path(self.plan["ProductRoot"])
        for file in manifest["Files"]:
            if self.hash(root / file["Path"]) != file["SHA256"]:
                raise ValueError("Reviewed source changed: " + file["Path"])
        tests = load(Path(self.plan["TestsReceipt"]))
        if tests["Status"] != "PASSED" or tests["ArtifactSHA256"] != self.plan["SHA256"]:
            raise ValueError("Tests do not identify this artifact")
        files = {}
        for folder in self.plan["ProtectedRoots"]:
            for path in Path(folder).rglob("*"):
                if path.is_file() and path.suffix.lower() in {".dll", ".asi", ".ini", ".json", ".txt"}:
                    if path.is_symlink() or path.is_junction():
                        raise ValueError(f"Unsupported reparse point: {path}")
                    files[str(path.resolve())] = {"Path": str(path.resolve()),
                        "SHA256": self.hash(path), "FileId": path.stat().st_ino}
        for filename in self.plan["ProtectedFiles"]:
            path = Path(filename).resolve()
            files[str(path)] = {"Path": str(path), "SHA256": self.hash(path), "FileId": path.stat().st_ino}
        pre = {"Settings": self.settings(), "ProtectedFiles": list(files.values()), "Profiles": {}}
        for game in self.plan["Games"]:
            key = game["SelectionKey"]
            if pre["Settings"].get(key) != game["PreviousBuildId"]:
                raise ValueError("Selection changed since planning: " + game["Biz"])
            if game.get("NrSHA256") and self.hash(Path(game["NrSource"])) != game["NrSHA256"]:
                raise ValueError("Pinned NR source differs: " + game["Biz"])
            profile = Path(game["Profile"])
            data = profile.read_bytes()
            self.copy_new(profile, self.evidence_file("before-" + game["Biz"] + ".ini"))
            pre["Profiles"][game["Biz"]] = {"Path": str(profile), "SHA256": self.hash(profile)}
            values = self.config(data)
            for key, expected in {("framegen", "external"): "false", ("plugins", "loadasiplugins"): "true"}.items():
                if values.get(key, "").lower() != expected:
                    raise ValueError(f"Unexpected existing route: {game['Biz']} {key}")
        save(self.evidence_file("pre.json"), pre)
        source = Path(self.plan["Payload"])
        for path in source.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in {".dll", ".txt"}:
                continue
            relative = path.relative_to(source)
            if relative.parts[0].lower() in {"profiles", "plugins"}:
                continue
            if path.is_symlink() or path.is_junction():
                raise ValueError(f"Unsupported payload reparse point: {path}")
            self.copy_new(artifact if relative.as_posix() == "OptiScaler.dll" else path,
                          self.package / relative, link=relative.as_posix() != "OptiScaler.dll")
        for relative in ["LICENSE", *self.plan.get("Licenses", [])]:
            self.copy_new(root / relative, self.package / relative)
        save(self.package / "build.json", {"sourceId": "local", "version": self.build_id.split("/")[1],
             "assetName": self.plan["DisplayName"], "installedAt": datetime.datetime.now(datetime.timezone.utc).isoformat()})
        for game in self.plan["Games"]:
            biz = game["Biz"]
            log = self.package / "logs" / biz / "OptiScaler.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            changes = {("plugins", "path"): str(plugins[biz]),
                       ("libraries", "optidllpath"): str(self.package / "OptiScaler"),
                       ("log", "logfilename"): str(log)}
            authority = self.patch(Path(game["Profile"]).read_bytes(), changes)
            authority_path = self.package / "profiles" / (biz + ".ini")
            authority_path.parent.mkdir(parents=True, exist_ok=True)
            authority_path.write_bytes(authority)
            runtime = self.runtime(game)
            for path in self.package.rglob("*"):
                if not path.is_file():
                    continue
                relative = path.relative_to(self.package)
                nr = path.name.lower() == "nvngx_dlssnr.dll"
                self.copy_new(Path(game["NrSource"]) if nr else path, runtime / relative,
                              link=path.suffix.lower() == ".dll")
            projected = self.patch(authority, {("libraries", "optidllpath"): str(runtime / "OptiScaler")})
            for relative in ["OptiScaler.ini", "profiles/" + biz + ".ini", ".hub-runtime-baseline"]:
                (runtime / relative).write_bytes(projected)
            (runtime / ".hub-authority-baseline").write_bytes(authority)
            (runtime / ".hub-runtime-owner").write_text(biz, encoding="utf-8")
            save(runtime / ".hub-profile-authority.json", {"BuildDirectory": str(self.package), "GameKey": biz})
        # A root default is required by older Hub builds; each game activates its
        # own authority profile before injection. It is never copied across games.
        first = self.plan["Games"][0]["Biz"]
        (self.package / "OptiScaler.ini").write_bytes((self.package / "profiles" / (first + ".ini")).read_bytes())
        self.protected(pre)
        self.verify(require_selected=False)

    def select(self):
        self.verify(require_selected=False)
        action = self.evidence_file("select.json")
        save(action, {"steps": [{"action": "set_opt_build", "build": self.build_id}]})
        exe = self.hub / (self.hub / "version.ini").read_text(encoding="utf-8-sig").strip().split("=", 1)[1]
        for game in self.plan["Games"]:
            output = self.evidence_file("select-" + game["Biz"] + ".log")
            with output.open("wb") as log:
                process = subprocess.Popen([str(exe), "run", "--biz", game["Biz"], "--file", str(action)],
                    cwd=self.hub, stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                # Do not terminate a timed-out launcher process: persist its id.
                try:
                    code = process.wait(timeout=50)
                except subprocess.TimeoutExpired:
                    save(self.evidence_file("selection-pending.json"), {"Biz": game["Biz"], "Pid": process.pid})
                    raise
                if code:
                    raise RuntimeError(f"Hub selection failed for {game['Biz']}: {code}; see {output}")
        self.verify(require_selected=True)

    def verify(self, *, require_selected):
        plugins = {g["Biz"]: self.user_plugins(g) for g in self.plan["Games"]}
        for root in [self.package, *(self.runtime(g) for g in self.plan["Games"])]:
            self.require_no_bundled_plugins(root)
        pre = load(self.evidence_file("pre.json"))
        self.protected(pre)
        settings = self.settings()
        allowed = {game["SelectionKey"] for game in self.plan["Games"]}
        changes = {key: {"Before": pre["Settings"].get(key), "After": settings.get(key)}
                   for key in pre["Settings"].keys() | settings.keys() if pre["Settings"].get(key) != settings.get(key)}
        if changes.keys() - allowed:
            raise ValueError("Unrelated launcher settings changed: " + str(changes.keys() - allowed))
        games = []
        for game in self.plan["Games"]:
            runtime = self.runtime(game)
            if game.get("NrSHA256") and self.hash(Path(game["NrSource"])) != game["NrSHA256"]:
                raise ValueError("Pinned NR source differs: " + game["Biz"])
            for path in self.package.rglob("*"):
                if path.is_file() and path.suffix.lower() == ".dll":
                    target = runtime / path.relative_to(self.package)
                    expected = self.hash(Path(game["NrSource"])) if path.name.lower() == "nvngx_dlssnr.dll" else self.hash(path)
                    if self.hash(target) != expected:
                        raise ValueError("Runtime payload differs: " + str(target))
                    source = Path(game["NrSource"]) if path.name.lower() == "nvngx_dlssnr.dll" else path
                    if source.stat().st_dev == target.stat().st_dev and not source.samefile(target):
                        raise ValueError("Runtime DLL hardlink differs: " + str(target))
            if self.hash(self.package / "OptiScaler.dll") != self.plan["SHA256"]:
                raise ValueError("Package artifact differs")
            if self.hash(runtime / "OptiScaler.dll") != self.plan["SHA256"]:
                raise ValueError("Runtime artifact differs")
            authority = (self.package / "profiles" / (game["Biz"] + ".ini")).read_bytes()
            old = self.evidence_file("before-" + game["Biz"] + ".ini").read_bytes()
            before, values = self.config(old), self.config(authority)
            permitted = {("plugins", "path"), ("libraries", "optidllpath"), ("log", "logfilename")}
            if any(before.get(key) != values.get(key) for key in before.keys() | values.keys() if key not in permitted):
                raise ValueError("User parameters changed: " + game["Biz"])
            for key, expected in {("framegen", "external"): "false", ("plugins", "loadasiplugins"): "true",
                                  ("plugins", "path"): str(plugins[game["Biz"]])}.items():
                actual = values.get(key, "")
                if (actual if key == ("plugins", "path") else actual.lower()) != expected:
                    raise ValueError("Profile configuration differs: " + str(key))
            projected = self.patch(authority, {("libraries", "optidllpath"): str(runtime / "OptiScaler")})
            if (runtime / "OptiScaler.ini").read_bytes() != projected:
                raise ValueError("Runtime profile projection differs: " + game["Biz"])
            selected = settings.get(game["SelectionKey"]) == self.build_id
            if require_selected and not selected:
                raise ValueError("Game does not select new build: " + game["Biz"])
            games.append({"Biz": game["Biz"], "Runtime": str(runtime), "DiskSelectionVerified": selected,
                          "NrSHA256": self.hash(runtime / "nvngx_dlssnr.dll"),
                          "UserPluginsPath": str(plugins[game["Biz"]]),
                          "UserPluginsCopied": False, "UserPluginsExecutionVerified": False,
                          "UserGeneratedFrames": values.get(("dlssg", "interpolationcount")),
                          "RuntimeLoadingVerified": False, "PerformanceAcceptance": "UNKNOWN"})
        result = {"VerifiedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "Status": "SELECTED_ON_DISK_RESTART_HUB_REQUIRED" if require_selected else "PREPARED_DISK_VERIFIED",
                  "Plan": str(self.plan_path),
                  "BuildId": self.build_id, "Package": str(self.package), "SHA256": self.plan["SHA256"],
                  "SourceCommit": self.plan["SourceCommit"],
                  "UserPluginsDistributed": False,
                  "ProtectedFilesVerified": len(pre["ProtectedFiles"]), "ChangedLauncherSettings": changes,
                  "Activation": "Fully exit and restart Hub before starting each game; persistent selection is not proof of the live GUI cache.",
                  "Games": games, "GameProcessQueryPerformed": False, "GameControlPerformed": False}
        save(self.evidence_file("deployment.json"), result)
        if require_selected:
            root = Path(self.plan["ProductRoot"])
            save(root / "build/current-asi-deployment.json", result)
            for game in games:
                save(root / "build" / ("current-game-deployment-" + game["Biz"] + ".json"),
                     {**{key: value for key, value in result.items() if key != "Games"}, **game})
        print(json.dumps({key: result[key] for key in ["Status", "BuildId", "ProtectedFilesVerified", "ChangedLauncherSettings", "Games"]}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=["prepare", "select", "verify"])
    parser.add_argument("--plan", type=Path, required=True)
    args = parser.parse_args()
    deployment = Deployment(args.plan)
    if args.phase == "verify":
        deployment.verify(require_selected=True)
    else:
        getattr(deployment, args.phase)()
