import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "atlas_build", Path(__file__).resolve().parents[2] / "tools" / "build.py")
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class CmakeDiscoveryTest(unittest.TestCase):
    def test_prefers_path(self):
        with mock.patch.object(build.shutil, "which", return_value="custom/cmake.exe"):
            self.assertEqual(build.resolve_cmake({"PATH": "custom"}), "custom/cmake.exe")

    def test_uses_selected_visual_studio(self):
        with tempfile.TemporaryDirectory() as directory:
            cmake = Path(directory) / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
            cmake.parent.mkdir(parents=True)
            cmake.touch()
            with mock.patch.object(build.shutil, "which", return_value=None), \
                    mock.patch.object(build.platform, "system", return_value="Windows"):
                self.assertEqual(build.resolve_cmake({"VSINSTALLDIR": directory}), str(cmake))

    def test_missing_cmake_names_fix(self):
        with mock.patch.object(build.shutil, "which", return_value=None), \
                mock.patch.object(build.platform, "system", return_value="Linux"):
            with self.assertRaisesRegex(RuntimeError, "CMake.*PATH"):
                build.resolve_cmake({"PATH": ""})

    def test_rejects_unsafe_build_directories_before_loading_tools(self):
        for directory in (".", "build", "build/../src", "../outside"):
            with self.subTest(directory=directory), \
                    mock.patch.object(build.sys, "argv", [
                        "build.py", "vs", "--build-dir", directory, "--clean"]), \
                    mock.patch.object(build, "load_msvc_env") as load_env:
                with self.assertRaisesRegex(RuntimeError, "directly under"):
                    build.main()
                load_env.assert_not_called()

    def test_isolated_directory_used_for_configure_and_build(self):
        directory = build.REPO_ROOT / "build" / "verify-windows"
        with mock.patch.object(build.sys, "argv", [
                "build.py", "vs", "--build-dir", "build/verify-windows"]), \
                mock.patch.object(build.platform, "system", return_value="Windows"), \
                mock.patch.object(build, "load_msvc_env", return_value=({}, 17)), \
                mock.patch.object(build, "resolve_cmake", return_value="cmake.exe"), \
                mock.patch.object(build, "run_cmake") as run:
            self.assertEqual(build.main(), 0)
        self.assertEqual(run.call_args_list[0].args[0], [
            "cmake.exe", "--preset", "vs", "-B", str(directory)])
        self.assertEqual(run.call_args_list[1].args[0], [
            "cmake.exe", "--build", str(directory), "--config", "Debug"])


if __name__ == "__main__":
    unittest.main()
