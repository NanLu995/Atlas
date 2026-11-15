using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;

namespace Atlas.Client.IntegrationTests;

// One cluster per test class via IClassFixture; teardown closes the
// launcher's stdin so it cleans up subprocesses on its own.
public sealed class RealClusterFixture : IDisposable
{
    private Process? _launcher;
    private readonly StringBuilder _stderrBuf = new();
    private readonly StringBuilder _stdoutBuf = new();
    public ushort LoginAppPort { get; private set; }
    public ushort BaseAppExternalPort { get; private set; }
    public ushort MachinedPort { get; private set; }
    internal string HandoffFixtureDirectory { get; } = Path.Combine(ResolveRepoRoot(), ".tmp",
        "rpc_handoff_" + Guid.NewGuid().ToString("N"));
    private readonly List<ushort> _baseInternalPorts = new();

    public RealClusterFixture() : this(null) { }

    internal RealClusterFixture(string? assembly, string? cellAssembly = null, int baseapps = 1)
    {
        var repoRoot = ResolveRepoRoot();
        var launcher = Path.Combine(repoRoot, "tools", "cluster_control", "test_cluster.py");
        if (!File.Exists(launcher))
            throw new FileNotFoundException("test_cluster.py not found", launcher);

        _launcher = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = ResolvePython(),
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = repoRoot,
            },
        };

        if (baseapps == 2)
        {
            Directory.CreateDirectory(HandoffFixtureDirectory);
            _launcher.StartInfo.Environment["ATLAS_RPC_FIXTURE_DIR"] = HandoffFixtureDirectory;
        }
        var arguments = _launcher.StartInfo.ArgumentList;
        arguments.Add(launcher);
        arguments.Add("--db-backend");
        arguments.Add(Environment.GetEnvironmentVariable("ATLAS_TEST_DB_BACKEND") ?? "mysql");
        arguments.Add("--baseapps");
        arguments.Add(baseapps.ToString());
        arguments.Add("--build-dir");
        arguments.Add(Path.GetFileName(Environment.GetEnvironmentVariable("ATLAS_BIN_DIR") ?? "debug"));
        if (Environment.GetEnvironmentVariable("ATLAS_TEST_CLUSTER_LOG_LEVEL") is { Length: > 0 } level)
        {
            arguments.Add("--log-level");
            arguments.Add(level);
        }
        if (!string.IsNullOrEmpty(assembly))
        {
            arguments.Add("--base-assembly");
            arguments.Add(assembly);
        }
        if (!string.IsNullOrEmpty(cellAssembly))
        {
            arguments.Add("--cell-assembly");
            arguments.Add(cellAssembly);
        }

        // Async stderr drain so a chatty launcher can't deadlock on a full
        // OS pipe; the buffer feeds diagnostics on the timeout path below.
        _launcher.ErrorDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            lock (_stderrBuf) _stderrBuf.AppendLine(e.Data);
        };
        _launcher.Start();
        _launcher.BeginErrorReadLine();

        // Include database initialization and script-host cold startup.
        var deadline = DateTime.UtcNow.AddSeconds(180);
        string? line = null;
        while (DateTime.UtcNow < deadline)
        {
            line = _launcher.StandardOutput.ReadLine();
            if (line is null) break;
            _stdoutBuf.AppendLine(line);
            if (line.StartsWith("READY ", StringComparison.Ordinal))
            {
                ParseReady(line);
                // READY fires on machined registration; peer-to-peer RUDP
                // channels (loginapp ↔ dbapp) still need an async Birth tick.
                Thread.Sleep(1500);
                return;
            }
        }

        // Brief wait so ExitCode is settled and BeginErrorReadLine has flushed.
        _launcher.WaitForExit(2_000);
        string stderr;
        lock (_stderrBuf) stderr = _stderrBuf.ToString();
        int exitCode = _launcher.HasExited ? _launcher.ExitCode : -1;
        TeardownAndThrow(
            $"test_cluster.py never reported READY; exit_code={exitCode}; " +
            $"last stdout line: {line ?? "<EOF>"}\n" +
            $"--- stdout transcript ---\n{_stdoutBuf}" +
            $"--- stderr transcript ---\n{stderr}");
    }

    private void ParseReady(string line)
    {
        foreach (var tok in line.AsSpan(6).ToString().Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = tok.IndexOf('=');
            if (eq <= 0) continue;
            var key = tok.AsSpan(0, eq);
            var value = tok.AsSpan(eq + 1);
            if (key.SequenceEqual("loginapp_port")) LoginAppPort = ushort.Parse(value);
            else if (key.SequenceEqual("baseapp_external_port")) BaseAppExternalPort = ushort.Parse(value);
            else if (key.SequenceEqual("machined_port")) MachinedPort = ushort.Parse(value);
            else if (key.SequenceEqual("baseapp_internal_ports"))
                foreach (var port in value.ToString().Split(',')) _baseInternalPorts.Add(ushort.Parse(port));
        }
        if (LoginAppPort == 0)
            TeardownAndThrow("READY line missing loginapp_port");
    }

    private void TeardownAndThrow(string message)
    {
        try { Dispose(); }
        catch (Exception error) { throw new InvalidOperationException(message, error); }
        throw new InvalidOperationException(message);
    }

    internal void KillBaseApp(ushort internalPort)
        => ControlBaseApp("kill-baseapp", "KILLED", internalPort);

    internal void RestartBaseApp(ushort internalPort)
        => ControlBaseApp("restart-baseapp", "RESTARTED", internalPort);

    internal void ShutdownBaseApp(ushort internalPort)
        => ControlBaseApp("shutdown-baseapp", "SHUTDOWN", internalPort);

    private void ControlBaseApp(string command, string status, ushort internalPort)
    {
        if (!_baseInternalPorts.Contains(internalPort) || _launcher is null)
            throw new InvalidOperationException("BaseApp is not in this test topology");
        _launcher.StandardInput.WriteLine($"{command} {internalPort}");
        _launcher.StandardInput.Flush();
        var deadline = DateTime.UtcNow.AddSeconds(command == "shutdown-baseapp" ? 45 : 10);
        while (DateTime.UtcNow < deadline)
        {
            var reply = _launcher.StandardOutput.ReadLineAsync()
                .WaitAsync(deadline - DateTime.UtcNow).GetAwaiter().GetResult();
            if (reply == $"{status} baseapp_internal_port={internalPort}") return;
            if (reply is null || !reply.StartsWith("[", StringComparison.Ordinal))
                throw new InvalidOperationException($"BaseApp {command} not confirmed: {reply}");
        }
        throw new TimeoutException($"BaseApp {command} confirmation expired");
    }

    internal int ClientBindingCount(ushort internalPort)
    {
        var index = _baseInternalPorts.IndexOf(internalPort);
        if (index < 0) throw new InvalidOperationException("BaseApp is not in this test topology");
        return WatcherCount(index == 0 ? "baseapp:baseapp" : "baseapp:baseapp-2",
            "baseapp/client_binding_count");
    }

    internal int BaseAppCount() => WatcherCount("baseappmgr:baseappmgr", "baseappmgr/baseapp_count");
    internal int AvailableBaseAppCount()
        => WatcherCount("baseappmgr:baseappmgr", "baseappmgr/available_baseapp_count");
    internal int RpcReplyCount()
        => WatcherCount("baseapp:baseapp", "baseapp/rpc_reply_count");

    private int WatcherCount(string target, string path)
    {
        var root = ResolveRepoRoot();
        var bin = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR")
            ?? throw new InvalidOperationException("ATLAS_BIN_DIR is required");
        using var process = new Process { StartInfo = new ProcessStartInfo {
            FileName = Path.Combine(bin, OperatingSystem.IsWindows() ? "atlas_tool.exe" : "atlas_tool"),
            WorkingDirectory = root, RedirectStandardOutput = true, RedirectStandardError = true,
            UseShellExecute = false, CreateNoWindow = true } };
        foreach (var argument in new[] { "--machined", $"127.0.0.1:{MachinedPort}", "watch",
            target, path })
            process.StartInfo.ArgumentList.Add(argument);
        process.Start();
        var output = process.StandardOutput.ReadToEnd();
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        var fields = output.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (process.ExitCode != 0 || fields.Length != 2 || !int.TryParse(fields[1], out var count))
            throw new InvalidOperationException(
                $"Watcher {target} {path} failed (pid={process.Id}, exit={process.ExitCode}, " +
                $"stdout={output.Length}, stderr={error.Length}): {output} {error}");
        return count;
    }

    public void Dispose()
    {
        if (_launcher is null) return;
        try
        {
            _launcher.StandardInput.Close();
            if (!_launcher.WaitForExit(210_000))
            {
                _launcher.Kill(entireProcessTree: true);
                _launcher.WaitForExit(5_000);
                throw new TimeoutException("Cluster teardown exceeded its ordered drain budgets");
            }
            if (_launcher.ExitCode != 0)
                throw new InvalidOperationException($"Cluster teardown failed: exit={_launcher.ExitCode}");
        }
        finally
        {
            _launcher.Dispose();
            _launcher = null;
        }
    }

    private static string ResolveRepoRoot()
    {
        // Walk up from bin/[platform/]<config>/<tfm>/ to the cluster_control
        // marker; the platform subdir (x64 on Windows) varies the depth.
        var dir = AppContext.BaseDirectory;
        while (dir is not null)
        {
            if (Directory.Exists(Path.Combine(dir, "tools", "cluster_control")))
                return dir;
            dir = Path.GetDirectoryName(dir);
        }
        throw new DirectoryNotFoundException("Cannot locate repo root from " + AppContext.BaseDirectory);
    }

    private static string ResolvePython()
    {
        var pyEnv = Environment.GetEnvironmentVariable("ATLAS_PYTHON");
        if (!string.IsNullOrEmpty(pyEnv)) return pyEnv;
        return OperatingSystem.IsWindows() ? "python" : "python3";
    }
}
