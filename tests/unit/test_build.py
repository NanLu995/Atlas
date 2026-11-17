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
    def test_version_floor_and_exact_sdk(self):
        for actual, minimum, exact, accepted in (
                ('3.31.6-msvc6', (3, 28, 0), False, True),
                ('3.27.9', (3, 28, 0), False, False),
                ('1.11.1', (1, 11, 0), False, True),
                ('1.10.2', (1, 11, 0), False, False),
                ('10.0.405', (10, 0, 404), True, False),
                ('10.0.404', (10, 0, 404), True, True)):
            with self.subTest(actual=actual), mock.patch.object(
                    build.subprocess, 'run', return_value=mock.Mock(stdout=actual)):
                if accepted:
                    build.check_version('tool', minimum, {}, exact=exact)
                else:
                    with self.assertRaises(RuntimeError):
                        build.check_version('tool', minimum, {}, exact=exact)

    def test_preview_or_wrong_series_cannot_be_selected(self):
        for installation in ({'installationVersion': '18.0.1', 'isComplete': True},
                             {'installationVersion': '17.14.1', 'isComplete': True,
                              'isPrerelease': True}):
            with self.subTest(installation=installation), \
                    mock.patch.object(build.Path, 'is_file', return_value=True), \
                    mock.patch.object(build.subprocess, 'run', return_value=mock.Mock(
                        stdout=build.json.dumps([installation]).encode())):
                with self.assertRaisesRegex(RuntimeError, '17.14 stable'):
                    build.detect_vs_install()

    def test_python_below_floor_fails_before_loading_tools(self):
        with mock.patch.object(build.sys, 'version_info', (3, 10, 14)), \
                mock.patch.object(build, 'check_version') as check:
            with self.assertRaisesRegex(RuntimeError, 'Python 3.11.8'):
                build.check_environment({}, 'cmake')
            check.assert_not_called()

    def test_old_python_is_rejected_before_path_or_tool_discovery(self):
        with mock.patch.object(build.sys, 'argv', ['build.py', 'debug']), \
                mock.patch.object(build.sys, 'version_info', (3, 8, 20)), \
                mock.patch.object(build, 'load_msvc_env') as load:
            with self.assertRaisesRegex(RuntimeError, 'Python 3.11.8'):
                build.main()
            load.assert_not_called()

    def test_ninja_checksum_mismatch_stops_before_extraction(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(build, 'REPO_ROOT', Path(directory)), \
                mock.patch.object(build.platform, 'system', return_value='Windows'), \
                mock.patch.object(build.shutil, 'which', return_value=None), \
                mock.patch.object(build.urllib.request, 'urlretrieve',
                                  side_effect=lambda url, path: path.write_bytes(b'corrupt')), \
                mock.patch.object(build.zipfile, 'ZipFile') as extract:
            with self.assertRaisesRegex(RuntimeError, 'Ninja checksum mismatch'):
                build.ensure_ninja({})
            extract.assert_not_called()

    def test_linux_rejects_a_different_compiler_major(self):
        with mock.patch.object(build.platform, 'system', return_value='Linux'), \
                mock.patch.object(build.platform, 'freedesktop_os_release', return_value={
                    'ID': 'ubuntu', 'VERSION_ID': '24.04'}), \
                mock.patch.object(build, 'check_version', side_effect=[
                    '3.31.6', '10.0.404', '14.2.0']):
            with self.assertRaisesRegex(RuntimeError, 'select GCC 13'):
                build.check_environment({}, 'cmake')

    def test_missing_pinned_sdk_reports_the_required_version(self):
        with mock.patch.object(build.subprocess, 'run', side_effect=
                               build.subprocess.CalledProcessError(145, ['dotnet', '--version'])):
            with self.assertRaisesRegex(RuntimeError, 'install 10.0.404 exactly'):
                build.check_version('dotnet', (10, 0, 404), {}, exact=True)

    def test_cached_preview_generator_is_rejected_without_touching_it(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'CMakeCache.txt'
            content = 'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n'
            path.write_text(content)
            with self.assertRaisesRegex(RuntimeError, 'fresh --build-dir'):
                build.validate_build_cache(Path(directory), {})
            self.assertEqual(path.read_text(), content)

    def test_cached_compiler_cannot_override_the_selected_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / 'CMakeCache.txt').write_text(
                'CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n'
                'CMAKE_CXX_COMPILER:FILEPATH=old/compiler\n')
            with mock.patch.object(build.shutil, 'which', return_value='selected/compiler'):
                with self.assertRaisesRegex(RuntimeError, 'Cached compiler'):
                    build.validate_build_cache(Path(directory), {})

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
                mock.patch.object(build, "check_environment"), \
                mock.patch.object(build, "run_cmake") as run:
            self.assertEqual(build.main(), 0)
        self.assertEqual(run.call_args_list[0].args[0], [
            "cmake.exe", "--preset", "vs", "-B", str(directory)])
        self.assertEqual(run.call_args_list[1].args[0], [
            "cmake.exe", "--build", str(directory), "--config", "Debug"])

    def test_release_acceptance_enables_existing_tests(self):
        with mock.patch.object(build.sys, 'argv', [
                'build.py', 'release', '--with-tests', '--config-only']), \
                mock.patch.object(build.platform, 'system', return_value='Windows'), \
                mock.patch.object(build, 'load_msvc_env', return_value=({}, 17)), \
                mock.patch.object(build, 'resolve_cmake', return_value='cmake.exe'), \
                mock.patch.object(build, 'check_environment'), \
                mock.patch.object(build, 'run_cmake') as run:
            self.assertEqual(build.main(), 0)
        self.assertIn('-DATLAS_BUILD_TESTS=ON', run.call_args.args[0])
        self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
