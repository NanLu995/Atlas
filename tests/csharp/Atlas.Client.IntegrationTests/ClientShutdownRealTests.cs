using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Atlas.Client.Desktop;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;
using Xunit;
using RpcAccount = Atlas.RpcFixture.Client.Account;

namespace Atlas.Client.IntegrationTests;

[Trait("Category", "Nightly")]
[Collection("RpcCluster")]
public sealed class ClientShutdownRealTests
{
    [Fact]
    public async Task MysqlGracefulShutdownPersistsStateAndCancelsRpcBeforeProcessRestart()
    {
        Assert.Equal("mysql", Environment.GetEnvironmentVariable("ATLAS_TEST_DB_BACKEND") ?? "mysql");
        var bin = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR")
            ?? throw new InvalidOperationException("ATLAS_BIN_DIR must name isolated build output");
        using var cluster = new RealClusterFixture(Path.Combine(bin, "Atlas.RpcFixture.Base.dll"));
        using var loop = new ManagedAtlasLoop();
        AtlasLoop.Install(loop);
        var firstSession = new ClientSession();
        var secondSession = new ClientSession();
        Atlas.Def.DefBootstrap.RegisterInto(firstSession);
        Atlas.Def.DefBootstrap.RegisterInto(secondSession);
        using var first = new LoginClient(firstSession);
        cluster.ConfigureClient(first);
        using var second = new LoginClient(secondSession);
        cluster.ConfigureClient(second);
        var username = "shutdown_" + Guid.NewGuid().ToString("N");
        try
        {
            var account = await Connect(first, firstSession);
            var location = await Await(account.Base.HandoffLocation());
            Assert.True(location.IsOk, location.ErrorMessage);
            var port = checked((ushort)location.Value[2]);
            Assert.Equal(Atlas.Protocol.RpcSendResult.Queued, account.Base.Notify(12345));
            Assert.Equal(12345, (await Await(account.Base.ReadNotification())).Value);
            var pending = account.Base.DelayedEcho(99, 10000, timeoutMs: 15000);
            Pump(() => cluster.RpcReplyCount() != 0);
            var stopped = Task.Run(() => cluster.ShutdownBaseApp(port));
            Pump(() => stopped.IsCompleted);
            await stopped;
            var cancelled = await Await(pending);
            Assert.False(cancelled.IsOk);
            Assert.Equal(RpcErrorCodes.ReceiverGone, cancelled.Error);
            Assert.Equal(0, ((ManagedRpcRegistry)firstSession.RpcRegistry).PendingCount);
            Pump(() => cluster.BaseAppCount() == 0);
            cluster.RestartBaseApp(port);
            Pump(() => cluster.BaseAppCount() == 1);
            Pump(() => cluster.AvailableBaseAppCount() == 1);
            var restored = await Connect(second, secondSession);
            Assert.Equal(12345, (await Await(restored.Base.ReadNotification())).Value);
        }
        finally
        {
            first.Dispose();
            second.Dispose();
            firstSession.Reset();
            secondSession.Reset();
            AtlasLoop.Reset();
        }

        async Task<RpcAccount> Connect(LoginClient client, ClientSession session)
        {
            await Await(client.LoginAsync("127.0.0.1", cluster.LoginAppPort, username, "<test-password>"));
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
            var deadline = DateTime.UtcNow.AddSeconds(45);
            while (!complete() && DateTime.UtcNow < deadline)
            {
                first.Poll();
                second.Poll();
                loop.Drain();
                Thread.Sleep(1);
            }
            Assert.True(complete(), "graceful shutdown verification deadline expired");
        }
    }
}
