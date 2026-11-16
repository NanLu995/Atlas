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
using Xunit;
using RpcAccount = Atlas.RpcFixture.Client.Account;
using RpcProbe = Atlas.RpcFixture.Client.Probe;

namespace Atlas.Client.IntegrationTests;

[Trait("Category", "Nightly")]
[Collection("RpcCluster")]
public sealed class SessionRecoveryRealTests
{
    [Theory]
    [InlineData("live")]
    [InlineData("source")]
    [InlineData("temporary-target")]
    [InlineData("persistent-target")]
    public async Task MysqlTransferredSessionRecoversOnlyItsConfirmedIdentity(string failure)
    {
        Assert.Equal("mysql", Environment.GetEnvironmentVariable("ATLAS_TEST_DB_BACKEND") ?? "mysql");
        var bin = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR")
            ?? throw new InvalidOperationException("ATLAS_BIN_DIR must name isolated build output");
        using var cluster = new RealClusterFixture(Path.Combine(bin, "Atlas.RpcFixture.Base.dll"),
            baseapps: 2);
        using var loop = new ManagedAtlasLoop();
        AtlasLoop.Install(loop);
        var session = new ClientSession();
        var recoveredSession = new ClientSession();
        Atlas.Def.DefBootstrap.RegisterInto(session);
        Atlas.Def.DefBootstrap.RegisterInto(recoveredSession);
        using var client = new LoginClient(session);
        using var recovered = new LoginClient(recoveredSession);
        cluster.ConfigureClient(client);
        cluster.ConfigureClient(recovered);
        try
        {
            await Await(client.LoginAsync("127.0.0.1", cluster.LoginAppPort,
                "transfer_recover_" + Guid.NewGuid().ToString("N"), "<test-password>"));
            var auth = await Await(client.AuthenticateAsync());
            session.CreateEntity(auth.EntityId, auth.TypeId);
            var account = Assert.IsType<RpcAccount>(session.EntityManager.Get(auth.EntityId));
            var origin = await Await(account.Base.HandoffLocation());
            Assert.True(origin.IsOk, origin.ErrorMessage);
            Pump(() => Directory.GetFiles(cluster.HandoffFixtureDirectory, "base-*.txt").Length == 2);
            var target = Assert.Single(Directory.GetFiles(cluster.HandoffFixtureDirectory, "base-*.txt")
                .Select(path => File.ReadAllText(path).Split(',')
                    .Select(value => ulong.Parse(value, CultureInfo.InvariantCulture)).ToArray()),
                address => address[3] != origin.Value[3]);
            var ready = checked((uint)target[0]);
            var handoff = account.Base.HandoffClient(checked((uint)target[0]),
                checked((uint)target[1]), checked((ushort)target[2]), target[3]);
            Pump(() => client.State == AtlasNetState.Connected &&
                session.EntityManager.Get(ready) is RpcProbe && account.IsDestroyed);
            await Await(handoff);
            var probe = Assert.IsType<RpcProbe>(session.EntityManager.Get(ready));
            var credential = Assert.IsType<SessionRecoveryCredential>(client.GetRecoveryCredential());
            if (failure == "persistent-target")
            {
                var saved = await Await(probe.Base.Save());
                Assert.True(saved.IsOk, saved.ErrorMessage);
                Assert.True(saved.Value > 0);
                Pump(() => !credential.Value.Span.SequenceEqual(
                    client.GetRecoveryCredential()!.Value.Span));
                credential = client.GetRecoveryCredential()!;
            }
            if (failure != "live")
            {
                cluster.KillBaseApp(checked((ushort)(failure == "source" ? origin.Value[2] : target[2])));
                Pump(() => cluster.BaseAppCount() == 1);
            }
            client.Disconnect(AtlasDisconnectReason.Internal);
            if (failure == "temporary-target")
            {
                var refused = await Assert.ThrowsAsync<LoginFailedException>(async () =>
                    await Await(recovered.ResumeAsync("127.0.0.1", cluster.LoginAppPort, credential)));
                Assert.Contains("temporary_entity", refused.Message);
                return;
            }
            await Await(recovered.ResumeAsync("127.0.0.1", cluster.LoginAppPort, credential));
            var resumed = await Await(recovered.AuthenticateAsync());
            if (failure != "persistent-target") Assert.Equal(ready, resumed.EntityId);
            recoveredSession.CreateEntity(resumed.EntityId, resumed.TypeId);
            var restored = Assert.IsType<RpcProbe>(recoveredSession.EntityManager.Get(resumed.EntityId));
            Assert.True((await Await(restored.Base.SendBaseNotice(700))).IsOk);
            Assert.True(await Await(recovered.LogoutAsync()));
        }
        finally
        {
            client.Dispose();
            recovered.Dispose();
            session.Reset();
            recoveredSession.Reset();
            AtlasLoop.Reset();
        }

        async Task<T> Await<T>(AtlasTask<T> task)
        {
            Pump(() => task.Status != AtlasTaskStatus.Pending);
            return await task;
        }

        void Pump(Func<bool> complete)
        {
            var deadline = DateTime.UtcNow.AddSeconds(35);
            while (!complete() && DateTime.UtcNow < deadline)
            {
                client.Poll();
                recovered.Poll();
                loop.Drain();
                Thread.Sleep(1);
            }
            Assert.True(complete(), "transferred session recovery verification deadline expired");
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MysqlRecoveryRotatesCredentialAndLogoutRevokesIt(bool crash)
    {
        Assert.Equal("mysql", Environment.GetEnvironmentVariable("ATLAS_TEST_DB_BACKEND") ?? "mysql");
        var bin = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR")
            ?? throw new InvalidOperationException("ATLAS_BIN_DIR must name isolated build output");
        using var cluster = new RealClusterFixture(Path.Combine(bin, "Atlas.RpcFixture.Base.dll"));
        using var loop = new ManagedAtlasLoop();
        AtlasLoop.Install(loop);
        var session = new ClientSession();
        Atlas.Def.DefBootstrap.RegisterInto(session);
        using var client = new LoginClient(session);
        cluster.ConfigureClient(client);
        try
        {
            await Await(client.LoginAsync("127.0.0.1", cluster.LoginAppPort,
                "recover_" + Guid.NewGuid().ToString("N"), "<test-password>"));
            var auth = await Await(client.AuthenticateAsync());
            session.CreateEntity(auth.EntityId, auth.TypeId);
            var account = Assert.IsType<RpcAccount>(session.EntityManager.Get(auth.EntityId));
            var location = await Await(account.Base.HandoffLocation());
            Assert.True(location.IsOk, location.ErrorMessage);
            Assert.Equal(Atlas.Protocol.RpcSendResult.Queued, account.Base.Notify(1042));
            Assert.Equal(1042, (await Await(account.Base.ReadNotification())).Value);
            var saved = await Await(account.Base.Save(false));
            Assert.True(saved.IsOk, saved.ErrorMessage);
            var original = Assert.IsType<SessionRecoveryCredential>(client.GetRecoveryCredential());
            var port = checked((ushort)location.Value[2]);
            if (crash)
            {
                cluster.KillBaseApp(port);
                Pump(() => cluster.BaseAppCount() == 0);
                cluster.RestartBaseApp(port);
                Pump(() => cluster.AvailableBaseAppCount() == 1);
            }
            client.Disconnect(AtlasDisconnectReason.Internal);
            session.Reset();
            await Await(client.ResumeAsync("127.0.0.1", cluster.LoginAppPort, original));
            var resumed = await Await(client.AuthenticateAsync());
            if (!crash) Assert.Equal(auth.EntityId, resumed.EntityId);
            session.CreateEntity(resumed.EntityId, resumed.TypeId);
            account = Assert.IsType<RpcAccount>(session.EntityManager.Get(resumed.EntityId));
            Assert.Equal(1042, (await Await(account.Base.ReadNotification())).Value);
            var rotated = Assert.IsType<SessionRecoveryCredential>(client.GetRecoveryCredential());
            Assert.False(original.Value.Span.SequenceEqual(rotated.Value.Span));
            Assert.True(await Await(client.LogoutAsync()));
            Pump(() => client.State == AtlasNetState.Disconnected);
            Assert.Null(client.GetRecoveryCredential());
            await Assert.ThrowsAsync<LoginFailedException>(async () =>
                await Await(client.ResumeAsync("127.0.0.1", cluster.LoginAppPort, rotated)));
            await Assert.ThrowsAsync<LoginFailedException>(async () =>
                await Await(client.ResumeAsync("127.0.0.1", cluster.LoginAppPort, original)));
        }
        finally
        {
            client.Dispose();
            session.Reset();
            AtlasLoop.Reset();
        }

        async Task<T> Await<T>(AtlasTask<T> task)
        {
            Pump(() => task.Status != AtlasTaskStatus.Pending);
            return await task;
        }

        void Pump(Func<bool> complete)
        {
            var deadline = DateTime.UtcNow.AddSeconds(45);
            while (!complete() && DateTime.UtcNow < deadline)
            {
                client.Poll();
                loop.Drain();
                Thread.Sleep(1);
            }
            Assert.True(complete(), "session recovery verification deadline expired");
        }
    }
}
