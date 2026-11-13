using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Atlas.Client.Desktop;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;
using Atlas.Protocol;
using Xunit;
using RpcAccount = Atlas.RpcFixture.Client.Account;
using RpcProbe = Atlas.RpcFixture.Client.Probe;

namespace Atlas.Client.IntegrationTests;

[Trait("Category", "Nightly")]
[Collection("RpcCluster")]
public sealed class RpcMailboxRealTests
{
    [Fact]
    public async Task GeneratedMailboxCompletesAndCleansUpAgainstRealBaseApp()
    {
        var bin = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR")
            ?? throw new InvalidOperationException("Set ATLAS_BIN_DIR to the isolated build output");
        using var cluster = new RealClusterFixture(Path.Combine(bin, "Atlas.RpcFixture.Base.dll"),
            Path.Combine(bin, "Atlas.RpcFixture.Cell.dll"));
        using var loop = new ManagedAtlasLoop();
        AtlasLoop.Install(loop);
        var session = new ClientSession();
        Atlas.Def.DefBootstrap.RegisterInto(session);
        using var client = new LoginClient(session);
        try
        {
            await Await(client.LoginAsync("127.0.0.1", cluster.LoginAppPort,
                "rpc_" + Guid.NewGuid().ToString("N"), "<test-password>"));
            var auth = await Await(client.AuthenticateAsync());
            session.CreateEntity(auth.EntityId, auth.TypeId);
            var account = Assert.IsType<RpcAccount>(session.EntityManager.Get(auth.EntityId));
            Assert.Equal(RpcErrorCodes.RemoteException,
                (await Await(account.Base.Throw(timeoutMs: 1000))).Error);
            Assert.Equal(RpcErrorCodes.RemoteException,
                (await Await(account.Base.BadAsyncReply(timeoutMs: 1000))).Error);
            Assert.Equal(int.MinValue, (await Await(account.Base.Echo(int.MinValue))).Value);
            var slow = account.Base.DelayedEcho(11, 250);
            var fast = account.Base.DelayedEcho(22, 20);
            Assert.Equal(22, (await Await(fast)).Value);
            Assert.Equal(11, (await Await(slow)).Value);
            Assert.Equal(71, (await Await(account.Base.Reject())).Error);
            Assert.Equal(RpcSendResult.Queued, account.Base.Notify(73));
            Assert.Equal(73, (await Await(account.Base.ReadNotification())).Value);
            Assert.Equal(RpcErrorCodes.Timeout,
                (await Await(account.Base.DelayedEcho(33, 300, timeoutMs: 20))).Error);
            using var cancellation = new AtlasCancellationSource();
            var cancelled = account.Base.DelayedEcho(44, 300, ct: cancellation.Token);
            cancellation.Cancel();
            Assert.Equal(RpcErrorCodes.Cancelled, (await Await(cancelled)).Error);
            var late = DateTime.UtcNow.AddMilliseconds(400);
            Pump(() => DateTime.UtcNow >= late);
            Assert.Equal(int.MaxValue, (await Await(account.Base.Echo(int.MaxValue))).Value);
            var destroyed = account.Base.DelayedEcho(55, 300);
            session.DestroyEntity(auth.EntityId);
            Assert.Equal(RpcErrorCodes.Cancelled, (await Await(destroyed)).Error);
            session.CreateEntity(auth.EntityId, auth.TypeId);
            account = Assert.IsType<RpcAccount>(session.EntityManager.Get(auth.EntityId));
            uint ready = 0;
            session.CellReadyReceived += id => ready = id;
            var attached = await Await(account.Base.AttachProbe());
            Assert.True(attached.IsOk, attached.ErrorMessage);
            Pump(() => ready == attached.Value);
            var probe = Assert.IsType<RpcProbe>(session.EntityManager.Get(attached.Value));
            Assert.Equal(101, (await Await(probe.Cell.Echo(101))).Value);
            Assert.Equal(102, (await Await(probe.Base.ViaCell(102))).Value);
            Assert.Equal(103, (await Await(probe.Cell.ViaBase(103))).Value);
            slow = probe.Cell.DelayedEcho(111, 250);
            fast = probe.Cell.DelayedEcho(122, 20);
            Assert.Equal(122, (await Await(fast)).Value);
            Assert.Equal(111, (await Await(slow)).Value);
            Assert.Equal(73, (await Await(probe.Cell.Reject())).Error);
            Assert.Equal(RpcSendResult.Queued, probe.Cell.Notify(173));
            Assert.Equal(173, (await Await(probe.Cell.ReadNotification())).Value);
            Assert.Equal(0, (await Await(probe.Base.SendBaseNotice(201))).Value);
            Pump(() => probe.LastNotice == 201);
            Assert.Equal(0, (await Await(probe.Cell.SendCellNotice(202))).Value);
            Pump(() => probe.LastNotice == 202);
            Assert.Equal(RpcErrorCodes.Timeout,
                (await Await(probe.Cell.DelayedEcho(133, 300, timeoutMs: 20))).Error);
            using var cellCancellation = new AtlasCancellationSource();
            cancelled = probe.Cell.DelayedEcho(144, 300, ct: cellCancellation.Token);
            cellCancellation.Cancel();
            Assert.Equal(RpcErrorCodes.Cancelled, (await Await(cancelled)).Error);
            late = DateTime.UtcNow.AddMilliseconds(400);
            Pump(() => DateTime.UtcNow >= late);
            Assert.Equal(155, (await Await(probe.Cell.Echo(155))).Value);
            var disconnected = probe.Cell.DelayedEcho(66, 300);
            client.Disconnect();
            Assert.Equal(RpcErrorCodes.ReceiverGone, (await Await(disconnected)).Error);
            Assert.Equal(0, ((ManagedRpcRegistry)session.RpcRegistry).PendingCount);
        }
        finally
        {
            session.Reset();
            client.Dispose();
            AtlasLoop.Reset();
        }

        async Task<T> Await<T>(AtlasTask<T> task)
        {
            Pump(() => task.Status != AtlasTaskStatus.Pending);
            return await task;
        }

        void Pump(Func<bool> complete)
        {
            var deadline = DateTime.UtcNow.AddSeconds(10);
            while (!complete() && DateTime.UtcNow < deadline)
            {
                client.Poll();
                loop.Drain();
                Thread.Sleep(1);
            }
            Assert.True(complete(), "RPC integration deadline expired");
        }
    }
}

[CollectionDefinition("RpcCluster", DisableParallelization = true)]
public sealed class RpcClusterCollection { }
