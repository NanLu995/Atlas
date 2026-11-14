using System;
using System.Threading.Tasks;
using Atlas.Coro.Rpc;
using Atlas.Protocol;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests;

public sealed class ClientRpcReplyTests
{
    private sealed class RpcEntity : ClientEntity
    {
        public override string TypeName => "RpcEntity";
    }

    [Fact]
    public async Task EntityDestroyCancelsItsReplyWaitWithoutCancellingSessionPeers()
    {
        var session = new ClientSession();
        var entity = new RpcEntity();
        session.EntityManager.Register(entity);
        var source = AtlasRpcSource<RpcReply<int>>.Rent();
        source.Start(session.RpcRegistry, 102, 17,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()),
            RpcReplyHelpers.For<int>(), 0, lifetime: entity.LifecycleCancellation);
        var peer = AtlasRpc.Await<int>(session.RpcRegistry, 102, 18,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        var task = source.Task;
        session.EntityManager.Destroy(entity.EntityId);
        Assert.True(task.GetAwaiter().IsCompleted);
        Assert.Equal(RpcErrorCodes.Cancelled, (await task).Error);
        session.DeliverFromServer(102, Reply(17, 7));
        Assert.False(peer.GetAwaiter().IsCompleted);
        session.DeliverFromServer(102, Reply(18, 8));
        Assert.Equal(8, (await peer).Value);
        Assert.Equal(0, ((ManagedRpcRegistry)session.RpcRegistry).PendingCount);
        session.Reset();
    }
    private sealed class ReentrantCallback : IAtlasRpcCallback
    {
        private readonly ClientSession _session;
        public long RejectedHandle = -1;
        public ReentrantCallback(ClientSession session) => _session = session;
        public void OnReply(ReadOnlySpan<byte> payload) => Assert.Fail("unexpected reply");
        public void OnError(RpcCompletionStatus status)
        {
            _session.CancelPendingRpcs();
            RejectedHandle = _session.RpcRegistry.RegisterPending(102, 18, 0, this);
        }
    }

    [Fact]
    public void DisconnectRejectsReentrantRegistration()
    {
        var session = new ClientSession();
        var callback = new ReentrantCallback(session);
        var registry = (ManagedRpcRegistry)session.RpcRegistry;
        registry.RegisterPending(102, 17, 0, callback);
        session.CancelPendingRpcs();
        Assert.Equal(0, callback.RejectedHandle);
        Assert.Equal(0, registry.PendingCount);
        session.Reset();
    }
    private static byte[] Reply(ulong requestId, int value)
    {
        var writer = new SpanWriter(16);
        try
        {
            writer.WriteUInt64(requestId);
            writer.WriteInt32(0);
            writer.WriteInt32(value);
            return writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }
    }

    [Fact]
    public async Task HandoffCancelsOldWaitsAndRejectsTheirLateReplies()
    {
        var session = new ClientSession();
        var entity = new RpcEntity();
        session.EntityManager.Register(entity);
        session.SpaceDataManager.SetKey(9, 1, new byte[] { 7 });
        var old = AtlasRpc.Await<int>(session.RpcRegistry, 102, 17,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        session.DeliverFromServer(ClientCallbacks.kClientHandoffMessageId, Array.Empty<byte>());
        Assert.True(entity.IsDestroyed);
        Assert.Equal(0, session.EntityManager.Count);
        Assert.False(session.SpaceDataManager.TryGet(9, 1, out _));
        Assert.True(old.GetAwaiter().IsCompleted);
        Assert.Equal(RpcErrorCodes.ReceiverGone, (await old).Error);
        var current = AtlasRpc.Await<int>(session.RpcRegistry, 102, 18,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        session.DeliverFromServer(102, Reply(17, 7));
        Assert.False(current.GetAwaiter().IsCompleted);
        session.DeliverFromServer(102, Reply(18, 8));
        Assert.Equal(8, (await current).Value);
        session.Reset();
    }

    [Fact]
    public async Task ReplyIsDeliveredOnlyToItsSessionAndCompletesOnce()
    {
        var first = new ClientSession();
        var other = new ClientSession();
        const ulong requestId = (1UL << 32) + 17;
        var task = AtlasRpc.Await<int>(first.RpcRegistry, MessageIds.EntityRpcReply, requestId,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        var body = Reply(requestId, 71);
        other.DeliverFromServer(MessageIds.EntityRpcReply, body);
        Assert.False(task.GetAwaiter().IsCompleted);
        first.DeliverFromServer(MessageIds.EntityRpcReply, body);
        first.DeliverFromServer(MessageIds.EntityRpcReply, body);
        Assert.True(task.GetAwaiter().IsCompleted);
        Assert.Equal(71, (await task).Value);
        Assert.Equal(0, ((ManagedRpcRegistry)first.RpcRegistry).PendingCount);
        first.Reset();
        other.Reset();
    }

    [Fact]
    public async Task DisconnectCompletesWaiterAndRejectsLateReplyAfterReconnect()
    {
        var session = new ClientSession();
        var oldRegistry = (ManagedRpcRegistry)session.RpcRegistry;
        var old = AtlasRpc.Await<int>(oldRegistry, MessageIds.EntityRpcReply, 17,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        session.CancelPendingRpcs();
        Assert.True(old.GetAwaiter().IsCompleted);
        Assert.Equal(RpcErrorCodes.ReceiverGone, (await old).Error);
        Assert.Equal(0, oldRegistry.PendingCount);
        var current = AtlasRpc.Await<int>(session.RpcRegistry, MessageIds.EntityRpcReply, 18,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        session.DeliverFromServer(MessageIds.EntityRpcReply, Reply(17, 7));
        session.DeliverFromServer(MessageIds.EntityRpcReply, new byte[7]);
        Assert.False(current.GetAwaiter().IsCompleted);
        session.DeliverFromServer(MessageIds.EntityRpcReply, Reply(18, 8));
        Assert.Equal(8, (await current).Value);
        session.Reset();
    }
}
