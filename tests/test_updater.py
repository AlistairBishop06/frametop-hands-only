"""Offline checks for stable and prerelease selection."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("framehands_updater", ROOT / "scripts/framehands-updater.py")
updater = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updater)


def release(tag, prerelease=False):
    base = f"https://github.com/AlistairBishop06/frametop-hands-only/releases/download/{tag}/"
    return {"tag_name": tag, "draft": False, "prerelease": prerelease,
            "assets": [{"name": name, "browser_download_url": base + name}
                       for name in ("framehands-aarch64.tar.gz", "SHA256SUMS")]}


class UpdaterTest(unittest.TestCase):
    def test_channels_select_expected_release(self):
        releases = [release("v0.2.0-dev.2", True), release("v0.1.9"),
                    release("v0.2.0-dev.1", True), release("v0.3.0-dev.1", True) | {"draft": True}]
        with patch.object(updater, "fetch", return_value=json.dumps(releases).encode()):
            self.assertEqual(updater.latest("stable")[0], "v0.1.9")
            self.assertEqual(updater.latest("dev")[0], "v0.2.0-dev.2")

    def test_switching_back_to_stable_offers_stable_release(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(updater, "APP", Path(temp)):
            (Path(temp) / ".framehands-release-tag").write_text("v0.2.0-dev.2\n")
            self.assertTrue(updater.newer("v0.1.9", "stable"))
            self.assertFalse(updater.newer("v0.2.0-dev.2", "dev"))
            self.assertTrue(updater.newer("v0.2.0-dev.3", "dev"))

    def test_channel_defaults_to_stable(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(updater, "CONFIG", Path(temp) / "config"):
            self.assertEqual(updater.channel(), "stable")
            updater.CONFIG.write_text("UPDATE_CHANNEL=dev\n")
            self.assertEqual(updater.channel(), "dev")


if __name__ == "__main__":
    unittest.main()
