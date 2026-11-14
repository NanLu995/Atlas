using System;
using System.IO;
using System.Linq;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;
using Atlas.Client.Desktop;
using Atlas.Client.Native;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;
using Xunit;
using RpcAccount = Atlas.RpcFixture.Client.Account;
using RpcProbe = Atlas.RpcFixture.Client.Probe;

namespace Atlas.Client.IntegrationTests;

[Trait("Category", "Nightly")]
[Collection("RpcCluster")]
public sealed class ClientHandoffRealTests
{
    [Theory]
    [InlineData("none")]
    [InlineData("source")]
    [InlineData("target")]
    [InlineData("source-auth")]
    [InlineData("target-auth")]
    [InlineData("network-auth")]
    [InlineData("target-restart")]
    public async Task MysqlTwoBaseAppsPreserveSingleControlAcrossHandoffReloginAndCrashes(string crashedEnd)
    {
        Assert.Equal("mysql", Environment.GetEnvironmentVariable("ATLAS_TEST_DB_BACKEND") ?? "mysql");
        var bin = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR")
            ?? throw new InvalidOperationException("ATLAS_BIN_DIR must name the isolated build output");
        using var cluster = new RealClusterFixture(Path.Combine(bin, "Atlas.RpcFixture.Base.dll"),
            Path.Combine(bin, "Atlas.RpcFixture.Cell.dll"), baseapps: 2);
        using var loop = new ManagedAtlasLoop();
        AtlasLoop.Install(loop);
        var sourceSession = new ClientSession();
        var reloginSession = new ClientSession();
        foreach (var session in new[] { sourceSession, reloginSession })
            Atlas.Def.DefBootstrap.RegisterInto(session);
        using var source = new LoginClient(sourceSession);
        using var relogin = new LoginClient(reloginSession);
        var clients = new[] { source, relogin };
        var username = "handoff_" + Guid.NewGuid().ToString("N");
        try
        {
            var account = await Connect(source, sourceSession, username);
            var origin = await Await(account.Base.HandoffLocation());
            Assert.True(origin.IsOk, origin.ErrorMessage);
            Pump(() => Directory.GetFiles(cluster.HandoffFixtureDirectory, "base-*.txt").Length == 2);
            var candidates = Directory.GetFiles(cluster.HandoffFixtureDirectory, "base-*.txt")
                .Select(path => File.ReadAllText(path).Split(',')
                    .Select(value => ulong.Parse(value, CultureInfo.InvariantCulture)).ToArray()).ToArray();
            Assert.All(candidates, candidate => Assert.Equal(4, candidate.Length));
            Assert.NotEqual(candidates[0][3], candidates[1][3]);
            var location = Assert.Single(candidates, candidate => candidate[3] != origin.Value[3]);
            Assert.NotEqual(origin.Value[3], location[3]);
            var sourcePort = checked((ushort)origin.Value[2]);
            var targetPort = checked((ushort)location[2]);
            Assert.Equal(1, cluster.ClientBindingCount(sourcePort));
            Assert.Equal(0, cluster.ClientBindingCount(targetPort));
            var bad = await Await(account.Base.HandoffClient(checked((uint)location[0]),
                checked((uint)location[1]), targetPort, location[3] + 1));
            Assert.False(bad.IsOk);
            Assert.Equal(17, (await Await(account.Base.Echo(17))).Value);
            uint ready = 0;
            sourceSession.CellReadyReceived += id => ready = id;
            var old = account.Base.HandoffClient(checked((uint)location[0]),
                checked((uint)location[1]), targetPort, location[3]);
            if (crashedEnd is "source-auth" or "target-auth" or "network-auth")
            {
                Pump(() => source.State == AtlasNetState.Handoff);
                if (crashedEnd == "network-auth")
                    Assert.Equal(AtlasNetReturnCode.Ok, source.SetTransportImpairment(0, 10000));
                else
                    cluster.KillBaseApp(crashedEnd == "source-auth" ? sourcePort : targetPort);
                if (crashedEnd is "target-auth" or "network-auth")
                {
                    Pump(() => source.State == AtlasNetState.Disconnected);
                    Assert.False((await Await(old)).IsOk);
                    Assert.Equal(0, ((ManagedRpcRegistry)sourceSession.RpcRegistry).PendingCount);
                    if (crashedEnd == "target-auth")
                        Pump(() => cluster.BaseAppCount() == 1);
                    else
                        Assert.Equal(AtlasNetReturnCode.Ok, source.SetTransportImpairment(0, 0));
                    try
                    {
                        var recovered = await Connect(relogin, reloginSession, username);
                        Assert.Equal(89, (await Await(recovered.Base.Echo(89))).Value);
                        var recoveredLocation = await Await(recovered.Base.HandoffLocation());
                        Assert.True(recoveredLocation.IsOk, recoveredLocation.ErrorMessage);
                        Assert.Equal(1, cluster.ClientBindingCount(checked((ushort)recoveredLocation.Value[2])));
                        if (crashedEnd == "network-auth")
                            Pump(() => cluster.ClientBindingCount(sourcePort) +
                                cluster.ClientBindingCount(targetPort) == 1);
                    }
                    catch (LoginFailedException failure)
                    {
                        Assert.Contains("account_session_", failure.Message);
                    }
                    return;
                }
            }
            Pump(() => ready == checked((uint)location[0]));
            Assert.True(account.IsDestroyed);
            Assert.Equal(RpcErrorCodes.ReceiverGone, (await Await(old)).Error);
            Assert.Equal(AtlasNetState.Connected, source.State);
            var probe = Assert.IsType<RpcProbe>(sourceSession.EntityManager.Get(ready));
            Assert.Equal(81, (await Await(probe.Cell.Echo(81))).Value);
            Assert.Equal(82, (await Await(probe.Base.ViaCell(82))).Value);
            Assert.Equal(83, (await Await(probe.Cell.ViaBase(83))).Value);
            Assert.Equal(Atlas.Protocol.RpcSendResult.Queued, probe.Cell.Notify(123));
            Assert.Equal(123, (await Await(probe.Cell.ReadNotification())).Value);
            var notification = Path.Combine(cluster.HandoffFixtureDirectory, $"notification-{ready}.txt");
            Assert.Equal("123", File.ReadAllText(notification));
            if (crashedEnd == "source-auth")
            {
                Assert.Equal(1, cluster.ClientBindingCount(targetPort));
                Assert.Equal(0, ((ManagedRpcRegistry)sourceSession.RpcRegistry).PendingCount);
                return;
            }
            Pump(() => cluster.ClientBindingCount(sourcePort) == 0);
            Assert.Equal(1, cluster.ClientBindingCount(targetPort));
            if (crashedEnd == "source")
            {
                cluster.KillBaseApp(sourcePort);
                Assert.Equal(86, (await Await(probe.Cell.Echo(86))).Value);
                Assert.Equal(87, (await Await(probe.Cell.ViaBase(87))).Value);
                Assert.Equal(1, cluster.ClientBindingCount(targetPort));
                return;
            }
            if (crashedEnd is "target" or "target-restart")
            {
                cluster.KillBaseApp(targetPort);
                Assert.False((await Await(probe.Cell.Echo(88, timeoutMs: 200))).IsOk);
                Pump(() => cluster.BaseAppCount() == 1);
                if (crashedEnd == "target-restart")
                {
                    cluster.RestartBaseApp(targetPort);
                    Pump(() => cluster.BaseAppCount() == 2);
                    Assert.Equal(0, cluster.ClientBindingCount(targetPort));
                    Assert.False((await Await(probe.Cell.Echo(90, timeoutMs: 200))).IsOk);
                }
                var failure = await Assert.ThrowsAsync<LoginFailedException>(async () =>
                    await Await(relogin.LoginAsync("127.0.0.1", cluster.LoginAppPort,
                        username, "<test-password>")));
                Assert.Contains("account_session_", failure.Message);
                Assert.Equal(0, cluster.ClientBindingCount(sourcePort));
                Assert.Equal("123", File.ReadAllText(notification));
                return;
            }
            var replacement = await Connect(relogin, reloginSession, username);
            Assert.Equal(84, (await Await(replacement.Base.Echo(84))).Value);
            var replacementLocation = await Await(replacement.Base.HandoffLocation());
            Assert.True(replacementLocation.IsOk, replacementLocation.ErrorMessage);
            var replacementPort = checked((ushort)replacementLocation.Value[2]);
            Pump(() => cluster.ClientBindingCount(sourcePort) + cluster.ClientBindingCount(targetPort) == 1);
            Assert.Equal(1, cluster.ClientBindingCount(replacementPort));
            Assert.Equal(Atlas.Protocol.RpcSendResult.Queued, probe.Cell.Notify(999));
            var rejected = await Await(probe.Cell.Echo(85, timeoutMs: 200));
            Assert.False(rejected.IsOk);
            Assert.Equal("123", File.ReadAllText(notification));
        }
        finally
        {
            foreach (var client in clients) client.Dispose();
            foreach (var session in new[] { sourceSession, reloginSession }) session.Reset();
            AtlasLoop.Reset();
        }

        async Task<RpcAccount> Connect(LoginClient client, ClientSession session, string user)
        {
            await Await(client.LoginAsync("127.0.0.1", cluster.LoginAppPort, user, "<test-password>"));
            var auth = await Await(client.AuthenticateAsync());
            session.CreateEntity(auth.EntityId, auth.TypeId);
            return Assert.IsType<RpcAccount>(session.EntityManager.Get(auth.EntityId));
        }
        async Task<T> Await<T>(AtlasTask<T> task)
        {
            Pump(() => task.Status != AtlasTaskStatus.Pending);
            return await task;
        }
        void Pump(Func<bool> complete)
        {
            var deadline = DateTime.UtcNow.AddSeconds(30);
            while (!complete() && DateTime.UtcNow < deadline)
            {
                foreach (var client in clients) client.Poll();
                loop.Drain();
                Thread.Sleep(1);
            }
            Assert.True(complete(), "two-BaseApp handoff deadline expired");
        }
    }
}
