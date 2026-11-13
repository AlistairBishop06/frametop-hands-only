import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("framehands_controls_cli", str(ROOT / "framehands"))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)

class ControlTests(unittest.TestCase):
    def test_touch_and_smoothness_persist(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "CONFIG", Path(tmp)/"config"):
            self.assertEqual(cli.main(["touch-on"]), 0)
            self.assertEqual(cli.main(["smoothness", "95"]), 0)
            self.assertEqual(cli.config()["HANDS_TOUCH"], "1")
            self.assertEqual(cli.config()["HANDS_SMOOTHNESS"], "95")
            self.assertEqual(cli.main(["touch-off"]), 0)
            self.assertEqual(cli.config()["HANDS_TOUCH"], "0")
            self.assertEqual(cli.main(["smoothness", "101"]), 1)
            self.assertEqual(cli.config()["HANDS_SMOOTHNESS"], "95")

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
