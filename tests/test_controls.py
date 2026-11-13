import importlib.machinery
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("framehands_controls_cli", str(ROOT / "framehands"))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)

class ControlTests(unittest.TestCase):
    def test_touch_and_smoothness_persist(self):
        commands = []
        def fake_ctl(*args, **_kwargs):
            commands.append(args)
            return SimpleNamespace(returncode=0, stderr="")
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "CONFIG", Path(tmp)/"config"), \
             patch.object(cli, "ctl", side_effect=fake_ctl), patch.object(cli, "active", return_value=True):
            self.assertEqual(cli.main(["touch-on"]), 0)
            self.assertIn(("enable", cli.UNITS[3]), commands)
            self.assertIn(("start", cli.UNITS[3]), commands)
            self.assertEqual(cli.main(["smoothness", "95"]), 0)
            self.assertEqual(cli.config()["HANDS_TOUCH"], "1")
            self.assertEqual(cli.config()["HANDS_SMOOTHNESS"], "95")
            commands.clear()
            self.assertEqual(cli.main(["touch-off"]), 0)
            self.assertIn(("disable", cli.UNITS[3]), commands)
            self.assertIn(("stop", cli.UNITS[3]), commands)
            self.assertEqual(cli.config()["HANDS_TOUCH"], "0")
            self.assertEqual(cli.main(["smoothness", "101"]), 1)
            self.assertEqual(cli.config()["HANDS_SMOOTHNESS"], "95")

    def test_touch_off_keeps_gesture_input_running(self):
        commands = []
        def fake_ctl(*args, **_kwargs):
            commands.append(args)
            return SimpleNamespace(returncode=0, stderr="")
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "CONFIG", Path(tmp)/"config"), \
             patch.object(cli, "ctl", side_effect=fake_ctl), patch.object(cli, "active", return_value=True):
            cli.CONFIG.write_text("GESTURE_INPUT=1\nHANDS_TOUCH=1\n")
            self.assertEqual(cli.main(["touch-off"]), 0)
            self.assertIn(("enable", cli.UNITS[3]), commands)
            self.assertIn(("start", cli.UNITS[3]), commands)
            self.assertNotIn(("stop", cli.UNITS[3]), commands)

    def test_visuals_toggle_only_starts_and_stops_the_view(self):
        commands = []
        def fake_ctl(*args, **_kwargs):
            commands.append(args)
            return SimpleNamespace(returncode=0, stderr="")
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "CONFIG", Path(tmp)/"config"), \
             patch.object(cli, "ctl", side_effect=fake_ctl), patch.object(cli, "active", return_value=True):
            cli.CONFIG.write_text("TRACKING=1\nGESTURE_INPUT=1\nRENDER=1\n")
            self.assertEqual(cli.main(["visuals-off"]), 0)
            self.assertEqual(cli.config()["RENDER"], "0")
            self.assertIn(("stop", cli.UNITS[2]), commands)
            self.assertNotIn(("stop", cli.UNITS[1]), commands)
            self.assertNotIn(("stop", cli.UNITS[3]), commands)
            commands.clear()
            self.assertEqual(cli.main(["visuals-on"]), 0)
            self.assertEqual(cli.config()["RENDER"], "1")
            self.assertIn(("start", cli.UNITS[2]), commands)

    def test_click_is_not_also_bound_to_switch_laser_hand(self):
        path = ROOT / "pointer/driver/fh_pointer/resources/input/fh_pointer_vrcompositor.json"
        bindings = json.loads(path.read_text())["bindings"]
        secondary = bindings["/actions/lasermouse_secondary"]["sources"]
        self.assertFalse(any(source["path"].endswith("/trigger") for source in secondary))

    def test_watchdog_is_only_enabled_when_tracking_is_autostarted(self):
        from types import SimpleNamespace
        commands=[]
        def fake_ctl(*a,**_):
            commands.append(a)
            return SimpleNamespace(returncode=0,stderr="")
        with patch.object(cli,"ctl", side_effect=fake_ctl):
            with patch.object(cli,"config",return_value={"AUTO_START":"1","TRACKING":"1","AUTO_RECOVER":"1"}):
                cli.sync_enabled()
                self.assertIn(("enable",cli.WATCHDOG_UNIT),commands)
            commands.clear()
            with patch.object(cli,"config",return_value={"AUTO_START":"0","TRACKING":"1","AUTO_RECOVER":"1"}):
                cli.sync_enabled()
                self.assertIn(("disable",cli.WATCHDOG_UNIT),commands)

if __name__ == "__main__": unittest.main()
