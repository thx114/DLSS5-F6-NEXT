#!/usr/bin/env python3
"""Real prepare/select/verify in disposable directories; mocked launcher, no game."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE = Path(__file__).resolve().parents[1] / "runtime" / "deploy_f6_asi.py"
SPEC = importlib.util.spec_from_file_location("f6_user_plugin_deployment", MODULE)
DEPLOY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPLOY)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def snapshot(root):
    return {str(path.relative_to(root)): (path.read_bytes(), path.stat().st_ino)
            for path in root.rglob("*") if path.is_file()}


class UserPluginDeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="f6-user-plugins-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        # Fail closed if any tested route accidentally tries to launch a process.
        self.process_guard = mock.patch.object(DEPLOY.subprocess, "Popen",
                                               side_effect=AssertionError("No process allowed"))
        self.process_guard.start()
        self.addCleanup(self.process_guard.stop)
        self.hub = self.root / "hub"
        self.product = self.root / "product"
        self.evidence = self.product / "build" / "evidence"
        self.evidence.mkdir(parents=True)
        self.payload = self.root / "payload"
        self.user_plugins = self.root / "UserPlugins"
        self.game_plugins = self.root / "StarRailPlugins"
        for plugins in [self.user_plugins, self.game_plugins]:
            write(plugins / "user.asi", b"fixture plugin; never executable")
            write(plugins / "support.dll", b"fixture plugin dependency")
            write(plugins / "user.ini", b"personal configuration")
        write(self.payload / "OptiScaler.dll", b"old F6 artifact")
        write(self.payload / "OptiScaler" / "dependency.dll", b"F6 dependency")
        write(self.payload / "nvngx_dlssnr.dll", b"unselected NR")
        write(self.payload / "plugins" / "support.dll", b"must not be packaged")
        write(self.payload / "plugins" / "settings.txt", b"must not be packaged")
        for name in ["root.asi", "plugins/bundled.ASI", "nested/deep/plugin.aSi"]:
            write(self.payload / name, b"never copy this plugin")
        write(self.payload / "profiles" / "unused.dll", b"unused profile payload")
        write(self.payload / "notice.txt", b"payload notice")
        self.artifact = write(self.product / "candidate.dll", b"reviewed F6 artifact")
        source = write(self.product / "source.cpp", b"reviewed source fixture")
        write(self.product / "LICENSE", b"fixture license")
        manifest = self.evidence / "source.json"
        DEPLOY.save(manifest, {"SourceCommit": "fixture-commit", "Files": [
            {"Path": "source.cpp", "SHA256": digest(source)}]})
        tests = self.evidence / "tests.json"
        DEPLOY.save(tests, {"Status": "PASSED", "ArtifactSHA256": digest(self.artifact)})
        self.hub.mkdir()
        write(self.hub / "version.ini", b"version=fixture-hub.exe")
        self.games = []
        self.enable_keys = {}
        self.original_profiles = {}
        with contextlib.closing(sqlite3.connect(self.hub / "HoYoShadeHubDatabase.db")) as db, db:
            db.execute("CREATE TABLE Setting (Key TEXT PRIMARY KEY, Value TEXT)")
            db.execute("INSERT INTO Setting VALUES (?, ?)", ("unrelated", "preserve"))
            for index, biz in enumerate(["hk4e_cn", "hkrpg_bilibili", "nap_cn"]):
                text = (f"; original {biz}\n[FrameGen]\nExternal=false\nUpscalerIndex={index}\n"
                        f"[DLSSG]\nInterpolationCount=5\nUnknownUserOption=keep-{biz}\n"
                        f"[NR]\nVersion=personal-{index}\n[Plugins]\nLoadAsiPlugins=true\n"
                        f"Path={self.user_plugins}\n[Libraries]\nOptiDllPath=old-library\n"
                        f"[Log]\nLogFileName=old-{biz}.log\n[SR]\nSharpness={index}.25\n")
                profile = write(self.hub / "old" / (biz + ".ini"), text.encode())
                nr = write(self.hub / "old" / (biz + ".dll"), f"personal NR {biz}".encode())
                instance = ["GsFixture01", "EdtUqXfCHh", "ZzzFixture01"][index]
                game = {"Biz": biz,
                        "SelectionKey": f"launch_option_optiscaler_build_{biz}_{instance}",
                        "PreviousBuildId": "official/F6", "Profile": str(profile),
                        "NrSource": str(nr), "NrSHA256": digest(nr)}
                if index == 1:
                    game["UserPluginsPath"] = str(self.game_plugins)
                self.games.append(game)
                self.enable_keys[biz] = f"launch_option_use_optiscaler_{biz}_{instance}"
                self.original_profiles[biz] = DEPLOY.Deployment.config(profile.read_bytes())
                db.execute("INSERT INTO Setting VALUES (?, ?)", (game["SelectionKey"], "official/F6"))
                db.execute("INSERT INTO Setting VALUES (?, ?)",
                           (self.enable_keys[biz], str(index == 1)))
        protected = write(self.hub / "state.json", b'{"old":true}')
        self.plan = {"InstallationRoot": str(self.hub), "ProductRoot": str(self.product),
                     "BuildId": "local/Fixture", "DisplayName": "offline fixture",
                     "Artifact": str(self.artifact), "SHA256": digest(self.artifact),
                     "SourceCommit": "fixture-commit", "SourceManifest": str(manifest),
                     "TestsReceipt": str(tests), "Payload": str(self.payload),
                     "ProtectedRoots": [str(self.hub / "old"), str(self.user_plugins),
                                        str(self.game_plugins), str(self.payload)],
                     "ProtectedFiles": [str(protected)], "Games": self.games,
                     "EvidencePrefix": "fixture", "UserPluginsPath": str(self.user_plugins)}
        self.plan_path = self.evidence / "plan.json"
        self.save_plan()
        self.original_files = {root: snapshot(root) for root in [self.hub / "old",
            self.user_plugins, self.game_plugins, self.payload]}

    def save_plan(self):
        DEPLOY.save(self.plan_path, self.plan)

    def deployment(self):
        self.save_plan()
        return DEPLOY.Deployment(self.plan_path)

    def prepare(self):
        deployment = self.deployment()
        with contextlib.redirect_stdout(io.StringIO()):
            deployment.prepare()
        return deployment

    def verify(self, *, selected=False):
        # Recreate the object to avoid sharing its hash cache with mutation tests.
        deployment = DEPLOY.Deployment(self.plan_path)
        with contextlib.redirect_stdout(io.StringIO()):
            deployment.verify(require_selected=selected)
        return DEPLOY.load(deployment.evidence_file("deployment.json"))

    @contextlib.contextmanager
    def hub_cli(self, *, restore=True, drift=False, returncode=0, timeout=False):
        calls = []

        def run(args, **kwargs):
            self.assertEqual(args[:2], [str(self.hub / "fixture-hub.exe"), "run"])
            self.assertEqual(args[2], "--biz")
            self.assertEqual(args[4], "--file")
            self.assertEqual(len(args), 6)
            self.assertEqual(kwargs["cwd"], self.hub)
            self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
            self.assertEqual(kwargs["creationflags"],
                             subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            self.assertFalse(kwargs["stdout"].closed)
            game = next(g for g in self.games if g["Biz"] == args[3])
            action_path = Path(args[5])
            action = DEPLOY.load(action_path)
            with contextlib.closing(sqlite3.connect(self.hub / "HoYoShadeHubDatabase.db")) as db, db:
                for step in action["steps"]:
                    if step["action"] == "set_opt_build":
                        db.execute("UPDATE Setting SET Value=? WHERE Key=?",
                                   (step["build"], game["SelectionKey"]))
                        # Model the Hub side effect that exposed the deployment bug.
                        db.execute("UPDATE Setting SET Value='True' WHERE Key=?",
                                   (self.enable_keys[game["Biz"]],))
                    elif step["action"] == "set_opt":
                        self.assertIs(type(step["enabled"]), bool)
                        if restore:
                            db.execute("UPDATE Setting SET Value=? WHERE Key=?",
                                       (str(step["enabled"]), self.enable_keys[game["Biz"]]))
                    else:
                        self.fail("Unexpected CLI action: " + step["action"])
                if drift:
                    db.execute("UPDATE Setting SET Value='changed' WHERE Key='unrelated'")
            process = mock.Mock(pid=4321)
            if timeout:
                process.wait.side_effect = subprocess.TimeoutExpired(args, 50)
            else:
                process.wait.return_value = returncode
            calls.append({"Game": game, "Path": action_path, "Action": action, "Process": process})
            return process

        with mock.patch.object(DEPLOY.subprocess, "Popen", side_effect=run):
            try:
                yield calls
            finally:
                for call in calls:
                    call["Process"].wait.assert_called_once_with(timeout=50)
                    call["Process"].terminate.assert_not_called()
                    call["Process"].kill.assert_not_called()

    def test_select_restores_opposite_game_enable_states_in_independent_actions(self):
        deployment = self.prepare()
        pre = DEPLOY.load(deployment.evidence_file("pre.json"))
        with self.hub_cli() as calls, contextlib.redirect_stdout(io.StringIO()):
            deployment.select()
        self.assertEqual([call["Game"]["Biz"] for call in calls], [g["Biz"] for g in self.games])
        self.assertEqual(len({call["Path"] for call in calls}), len(self.games))
        for call in calls:
            enabled = pre["Settings"][self.enable_keys[call["Game"]["Biz"]]] == "True"
            self.assertEqual(call["Action"], {"steps": [
                {"action": "set_opt_build", "build": deployment.build_id},
                {"action": "set_opt", "enabled": enabled}]})
            self.assertEqual(DEPLOY.load(call["Path"]), call["Action"])
        report = self.verify(selected=True)
        self.assertEqual(set(report["ChangedLauncherSettings"]), {g["SelectionKey"] for g in self.games})
        self.assertEqual({key: deployment.settings()[key] for key in self.enable_keys.values()},
                         {key: pre["Settings"][key] for key in self.enable_keys.values()})

    def test_select_rejects_failed_restoration(self):
        deployment = self.prepare()
        with self.hub_cli(restore=False) as calls, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "Unrelated launcher settings changed"):
                deployment.select()
        self.assertEqual(len(calls), len(self.games))
        with self.assertRaisesRegex(ValueError, "Unrelated launcher settings changed"):
            self.verify(selected=True)

    def test_select_rejects_unrelated_setting_drift(self):
        deployment = self.prepare()
        with self.hub_cli(drift=True), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "Unrelated launcher settings changed.*unrelated"):
                deployment.select()
        with self.assertRaisesRegex(ValueError, "Unrelated launcher settings changed.*unrelated"):
            self.verify(selected=True)

    def test_select_timeout_records_pid_without_terminating_or_continuing(self):
        deployment = self.prepare()
        with self.hub_cli(timeout=True) as calls, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.TimeoutExpired):
                deployment.select()
        self.assertEqual(len(calls), 1)
        self.assertEqual(DEPLOY.load(deployment.evidence_file("selection-pending.json")),
                         {"Biz": self.games[0]["Biz"], "Pid": 4321})
        self.assertEqual(DEPLOY.load(deployment.evidence_file("deployment.json"))["Status"],
                         "PREPARED_DISK_VERIFIED")
        self.assertFalse((self.product / "build/current-asi-deployment.json").exists())

    def test_select_nonzero_exit_stops_without_selected_receipt(self):
        deployment = self.prepare()
        with self.hub_cli(returncode=7) as calls, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "Hub selection failed for hk4e_cn: 7"):
                deployment.select()
        self.assertEqual(len(calls), 1)
        self.assertFalse(deployment.evidence_file("selection-pending.json").exists())
        self.assertEqual(DEPLOY.load(deployment.evidence_file("deployment.json"))["Status"],
                         "PREPARED_DISK_VERIFIED")
        self.assertFalse((self.product / "build/current-asi-deployment.json").exists())

    def test_select_rejects_unmappable_selection_keys_before_any_cli(self):
        self.prepare()
        game = self.games[-1]
        for key in ["build_" + game["Biz"],
                    "launch_option_optiscaler_build_hkrpg_bilibili_EdtUqXfCHh",
                    "launch_option_optiscaler_build_" + game["Biz"] + "_"]:
            with self.subTest(key=key):
                game["SelectionKey"] = key
                deployment = self.deployment()
                with mock.patch.object(DEPLOY.subprocess, "Popen") as cli:
                    with contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaisesRegex(ValueError, "Cannot map SelectionKey"):
                            deployment.select()
                    cli.assert_not_called()
                self.assertFalse(any(self.evidence.glob("fixture-select-*.json")))

    def test_select_requires_valid_prepared_enable_value_for_every_game(self):
        deployment = self.prepare()
        key = self.enable_keys[self.games[-1]["Biz"]]
        pre = DEPLOY.load(deployment.evidence_file("pre.json"))
        for label, value in [("missing", None), ("null", None), ("empty", ""),
                             ("numeric", "1"), ("invalid", "yes")]:
            with self.subTest(value=label):
                with contextlib.closing(sqlite3.connect(self.hub / "HoYoShadeHubDatabase.db")) as db, db:
                    if label == "missing":
                        pre["Settings"].pop(key)
                        db.execute("DELETE FROM Setting WHERE Key=?", (key,))
                    else:
                        pre["Settings"][key] = value
                        db.execute("INSERT OR REPLACE INTO Setting VALUES (?, ?)", (key, value))
                DEPLOY.save(deployment.evidence_file("pre.json"), pre)
                with mock.patch.object(DEPLOY.subprocess, "Popen") as cli:
                    with contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaisesRegex(ValueError, "Prepared enable setting must be True or False"):
                            deployment.select()
                    cli.assert_not_called()
                self.assertFalse(any(self.evidence.glob("fixture-select-*.json")))

    def test_select_rejects_live_enable_drift_before_cli(self):
        deployment = self.prepare()
        with contextlib.closing(sqlite3.connect(self.hub / "HoYoShadeHubDatabase.db")) as db, db:
            db.execute("UPDATE Setting SET Value='True' WHERE Key=?",
                       (self.enable_keys[self.games[0]["Biz"]],))
        with mock.patch.object(DEPLOY.subprocess, "Popen") as cli:
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, "Unrelated launcher settings changed"):
                    deployment.select()
            cli.assert_not_called()

    def test_prepare_references_user_plugins_and_preserves_each_game(self):
        deployment = self.prepare()
        roots = [deployment.package, *(deployment.runtime(g) for g in self.games)]
        for root in roots:
            self.assertFalse(any(p.suffix.lower() == ".asi" for p in root.rglob("*")))
            self.assertFalse((root / "plugins").exists())
            self.assertFalse((root / "profiles" / "unused.dll").exists())
            self.assertEqual((root / "OptiScaler.dll").read_bytes(), self.artifact.read_bytes())
        for game in self.games:
            runtime = deployment.runtime(game)
            expected_plugins = game.get("UserPluginsPath", self.plan["UserPluginsPath"])
            authority = deployment.package / "profiles" / (game["Biz"] + ".ini")
            profile_paths = [authority, runtime / "OptiScaler.ini",
                             runtime / "profiles" / (game["Biz"] + ".ini"),
                             runtime / ".hub-authority-baseline", runtime / ".hub-runtime-baseline"]
            for path in profile_paths:
                config = DEPLOY.Deployment.config(path.read_bytes())
                self.assertEqual(config[("plugins", "path")], expected_plugins)
                self.assertEqual(config[("plugins", "loadasiplugins")], "true")
                self.assertEqual(config[("framegen", "external")], "false")
                for key, value in self.original_profiles[game["Biz"]].items():
                    if key not in {("plugins", "path"), ("libraries", "optidllpath"),
                                   ("log", "logfilename")}:
                        self.assertEqual(config[key], value, (game["Biz"], key))
            self.assertEqual(digest(runtime / "nvngx_dlssnr.dll"), game["NrSHA256"])
            self.assertTrue(os.path.samefile(runtime / "nvngx_dlssnr.dll", game["NrSource"]))
            for relative in ["OptiScaler.dll", "OptiScaler/dependency.dll"]:
                self.assertTrue(os.path.samefile(runtime / relative, deployment.package / relative))
        for root, before in self.original_files.items():
            self.assertEqual(snapshot(root), before)
        report = self.verify()
        self.assertEqual(report["Status"], "PREPARED_DISK_VERIFIED")
        self.assertFalse(report["UserPluginsDistributed"])
        self.assertFalse(report["GameProcessQueryPerformed"])
        self.assertFalse(report["GameControlPerformed"])
        for game in report["Games"]:
            self.assertFalse(game["UserPluginsCopied"])
            self.assertFalse(game["UserPluginsExecutionVerified"])
            self.assertFalse(game["RuntimeLoadingVerified"])
            self.assertEqual(game["PerformanceAcceptance"], "UNKNOWN")

    def test_selected_receipt_keeps_disk_and_game_acceptance_separate(self):
        deployment = self.prepare()
        with contextlib.closing(sqlite3.connect(self.hub / "HoYoShadeHubDatabase.db")) as db, db:
            for game in self.games:
                db.execute("UPDATE Setting SET Value=? WHERE Key=?",
                           (deployment.build_id, game["SelectionKey"]))
        report = self.verify(selected=True)
        self.assertEqual(report["Status"], "SELECTED_ON_DISK_RESTART_HUB_REQUIRED")
        self.assertTrue(all(g["DiskSelectionVerified"] for g in report["Games"]))
        self.assertTrue(all(not g["RuntimeLoadingVerified"] for g in report["Games"]))
        self.assertTrue((self.product / "build/current-asi-deployment.json").is_file())
        self.assertFalse((self.product / "build/current-deployment.json").exists())

    def test_installed_payload_uses_explicit_product_notices_once(self):
        write(self.payload / "Licenses" / "third-party.txt", b"installed notice")
        write(self.product / "Licenses" / "third-party.txt", b"current product notice")
        self.plan["Licenses"] = ["Licenses/third-party.txt"]
        deployment = self.prepare()
        for root in [deployment.package, *(deployment.runtime(g) for g in self.games)]:
            self.assertEqual((root / "Licenses" / "third-party.txt").read_bytes(),
                             b"current product notice")
        self.verify()

    def test_missing_legacy_path_fails_before_any_write_or_cli(self):
        self.plan.pop("UserPluginsPath")
        for game in self.games:
            game.pop("UserPluginsPath", None)
        deployment = self.deployment()
        before = snapshot(self.root)
        for action in [deployment.prepare, deployment.select,
                       lambda: deployment.verify(require_selected=True)]:
            with self.assertRaisesRegex(ValueError, "explicit UserPluginsPath"):
                action()
            self.assertEqual(snapshot(self.root), before)

    def test_invalid_or_overlapping_plugin_directory_fails_before_writes(self):
        deployment = self.deployment()
        # Existing generated directories are not valid user plugin stores either.
        deployment.package.mkdir(parents=True)
        deployment.runtime(self.games[0]).mkdir(parents=True)
        for value in ["relative/plugins", str(self.root / "absent"),
                      str(self.user_plugins / "user.asi"), str(deployment.package),
                      str(deployment.runtime(self.games[0])), str(self.hub)]:
            with self.subTest(path=value):
                self.plan["UserPluginsPath"] = value
                current = self.deployment()
                before = snapshot(self.root)
                with self.assertRaisesRegex(ValueError, "UserPluginsPath"):
                    current.prepare()
                self.assertEqual(snapshot(self.root), before)

    def test_per_game_paths_work_without_a_global_path(self):
        self.plan.pop("UserPluginsPath")
        for game in self.games:
            game["UserPluginsPath"] = str(self.user_plugins)
        self.prepare()
        self.assertTrue(all(g["UserPluginsPath"] == str(self.user_plugins)
                            for g in self.verify()["Games"]))

    def test_reprepare_refuses_to_modify_existing_package(self):
        deployment = self.prepare()
        before = snapshot(self.root)
        with self.assertRaises(FileExistsError):
            deployment.prepare()
        self.assertEqual(snapshot(self.root), before)

    def test_bundled_plugin_in_package_or_runtime_refuses_receipt(self):
        deployment = self.prepare()
        for root in [deployment.package, deployment.runtime(self.games[1])]:
            with self.subTest(root=root):
                path = write(root / "unexpected" / "third-party.ASI", b"fixture")
                before = snapshot(self.root)
                with self.assertRaisesRegex(ValueError, "Bundled ASI is unsupported"):
                    self.verify()
                self.assertEqual(snapshot(self.root), before)
                path.unlink()

    def test_user_plugin_reference_must_match_authority(self):
        deployment = self.prepare()
        authority = deployment.package / "profiles" / (self.games[0]["Biz"] + ".ini")
        authority.write_bytes(deployment.patch(authority.read_bytes(),
            {("plugins", "path"): str(self.game_plugins)}))
        with self.assertRaisesRegex(ValueError, "Profile configuration differs"):
            self.verify()

    def test_user_parameter_or_external_change_is_rejected(self):
        deployment = self.prepare()
        authority = deployment.package / "profiles" / (self.games[0]["Biz"] + ".ini")
        original = authority.read_bytes()
        for key, value, error in [(("sr", "sharpness"), "99", "User parameters changed"),
                                  (("framegen", "external"), "true", "User parameters changed")]:
            with self.subTest(key=key):
                authority.write_bytes(deployment.patch(original, {key: value}))
                with self.assertRaisesRegex(ValueError, error):
                    self.verify()
        authority.write_bytes(original)

    def test_dll_hardlink_loss_is_detected_even_with_identical_bytes(self):
        deployment = self.prepare()
        path = deployment.runtime(self.games[0]) / "OptiScaler.dll"
        data = path.read_bytes()
        path.unlink()
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "hardlink differs"):
            self.verify()

    def test_pinned_nr_and_protected_files_remain_enforced(self):
        deployment = self.prepare()
        game = self.games[0]
        # A wrong reviewed identity must fail even when the NR file itself is unchanged.
        self.plan["Games"][0]["NrSHA256"] = "0" * 64
        self.save_plan()
        with self.assertRaisesRegex(ValueError, "Pinned NR source differs"):
            self.verify()
        self.plan["Games"][0]["NrSHA256"] = digest(Path(game["NrSource"]))
        self.save_plan()
        write(self.hub / "state.json", b"changed protected state")
        with self.assertRaisesRegex(ValueError, "Existing protected file changed"):
            self.verify()


if __name__ == "__main__":
    unittest.main(verbosity=2)
