using System;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Coro.Testing;
using Atlas.Core;
using Atlas.Coro.Hosting;
using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("EntityManager")]
public sealed class LifecycleCancellationTests : IDisposable
{
    private readonly TestLoop _loop;
    private readonly ManagedRpcRegistry _registry;

    public LifecycleCancellationTests()
    {
        EntityManager.Instance.Reset();
        _loop = new TestLoop();
        AtlasLoop.Install(_loop);
        _registry = new ManagedRpcRegistry(_loop);
    }

    public void Dispose()
    {
        _registry.Dispose();
        AtlasLoop.Reset();
        EntityManager.Instance.Reset();
    }

    private sealed class Dummy : ServerEntity
    {
        public int TickCount { get; private set; }
        public override string TypeName => "Dummy";
        protected internal override void OnTick(float deltaTime) => ++TickCount;
        public override void Serialize(ref SpanWriter w) { }
        public override void Deserialize(ref SpanReader r) { }
    }

    [Fact]
    public void ShutdownDrainProcessesQueuedContinuationsWithoutEntityTick()
    {
        using var loop = new ManagedAtlasLoop();
        var previousLoop = EngineContext.CoroLoop;
        var previousContext = EngineContext.SyncContext;
        EngineContext.CoroLoop = loop;
        EngineContext.SyncContext = new AtlasSynchronizationContext();
        var entity = EntityManager.Instance.Create<Dummy>();
        var posted = 0;
        try
        {
            loop.PostMainThread(_ => ++posted, null);
            EngineContext.SyncContext.Post(_ => ++posted, null);
            Assert.Equal(0, posted);
            Lifecycle.DoOnDrain();
            Assert.Equal(2, posted);
            Assert.Equal(0, entity.TickCount);
            Lifecycle.DoOnDrain();
            Assert.Equal(2, posted);
        }
        finally
        {
            EngineContext.CoroLoop = previousLoop;
            EngineContext.SyncContext = previousContext;
        }
    }

    private static readonly AtlasRpcSource<RpcReply<int>>.SpanDeserializer Deser =
        static (ref SpanReader r) => RpcReply<int>.Ok(r.ReadInt32());

    [Fact]
    public void NewEntity_LifecycleTokenNotCancelled()
    {
        var e = EntityManager.Instance.Create<Dummy>();
        Assert.False(e.LifecycleCancellation.IsCancellationRequested);
    }

    [Fact]
    public void Destroy_CancelsLifecycleToken()
    {
        var e = EntityManager.Instance.Create<Dummy>();
        EntityManager.Instance.Destroy(e.EntityId);
        Assert.True(e.LifecycleCancellation.IsCancellationRequested);
    }

    [Fact]
    public void Destroy_InFlightRpc_CompletesAsCancelledReply()
    {
        var e = EntityManager.Instance.Create<Dummy>();
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, replyId: 100, requestId: 1,
            Deser, RpcReplyHelpers.For<int>(),
            timeoutMs: 5000, ct: e.LifecycleCancellation);

        Assert.Equal(1, _registry.PendingCount);
        EntityManager.Instance.Destroy(e.EntityId);
        Assert.Equal(0, _registry.PendingCount);
        var reply = _loop.RunAwait(src.Task);
        Assert.Equal(RpcErrorCodes.Cancelled, reply.Error);
    }

    [Fact]
    public void Cancel_IsIdempotent_RepeatedDestroyDoesNotThrow()
    {
        var e = EntityManager.Instance.Create<Dummy>();
        EntityManager.Instance.Destroy(e.EntityId);
        // Trigger again: must not throw.
        e.TriggerLifecycleCancellation();
        Assert.True(e.LifecycleCancellation.IsCancellationRequested);
    }
}
