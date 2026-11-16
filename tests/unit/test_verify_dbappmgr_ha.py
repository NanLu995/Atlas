from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "cluster_control" / "verify_dbappmgr_ha.py"
SPEC = importlib.util.spec_from_file_location("verify_dbappmgr_ha", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify_dbappmgr_ha = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verify_dbappmgr_ha
SPEC.loader.exec_module(verify_dbappmgr_ha)


class ManagerHealthTest(unittest.TestCase):
    def read_health(self, *, count=2, shards=2, version=7, table="ranges",
                    recovery="false", expected=2):
        watchers = {
            "dbappmgr/dbapp_count": count,
            "dbappmgr/shards/count": shards,
            "dbappmgr/shards/version": version,
            "dbappmgr/shards/table": table,
            "dbappmgr/ha/recovery_window_active": recovery,
        }
        with mock.patch.object(verify_dbappmgr_ha, "int_watcher",
                               side_effect=lambda exe, host, target, path: int(watchers[path])), \
             mock.patch.object(verify_dbappmgr_ha, "watcher_value",
                               side_effect=lambda exe, host, target, path: watchers[path]):
            return verify_dbappmgr_ha.read_manager_health(
                Path("atlas_tool"), "machined", "dbappmgr:dbappmgr", expected)

    def test_registered_workers_with_shards_after_recovery_are_healthy(self):
        self.assertTrue(self.read_health()["healthy"])

    def test_active_or_unknown_recovery_window_is_not_healthy(self):
        for recovery in ("true", "", "unknown"):
            with self.subTest(recovery=recovery):
                self.assertFalse(self.read_health(recovery=recovery)["healthy"])

    def test_missing_workers_or_shards_are_not_healthy(self):
        for changes in ({"count": 1}, {"shards": 0}, {"version": 0}, {"table": ""}):
            with self.subTest(changes=changes):
                self.assertFalse(self.read_health(**changes)["healthy"])

    def test_explicit_empty_cluster_still_requires_recovery_to_finish(self):
        self.assertTrue(self.read_health(count=0, shards=0, version=0, table="",
                                         expected=0)["healthy"])
        self.assertFalse(self.read_health(count=0, shards=0, version=0, table="",
                                          expected=0, recovery="true")["healthy"])


if __name__ == "__main__":
    unittest.main()
