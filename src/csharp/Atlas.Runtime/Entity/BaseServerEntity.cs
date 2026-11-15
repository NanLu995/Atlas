using System;
using Atlas.Core;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Protocol;
using Atlas.Runtime.Coro;
using Atlas.Serialization;

namespace Atlas.Entity;

// Base-resident entity base — owns the client proxy and base→cell orchestration.
public abstract class BaseServerEntity : ServerEntity
{
    public bool TryGetClientHandoffTarget(out ClientHandoffTarget target)
    {
        ThreadGuard.EnsureMainThread();
        if (NativeApi.GetClientHandoffTarget(EntityId, out var ip, out var port,
                out var instance) != 0)
        {
            target = new ClientHandoffTarget(EntityId, ip, port, instance);
            return true;
        }
        target = default;
        return false;
    }

    protected internal AtlasTask<RpcReply<bool>> GiveClientToAsync(uint destEntityId,
        AtlasCancellationToken ct = default)
        => GiveClientToAsync(new ClientHandoffTarget(destEntityId, 0, 0, 0), ct);

    protected internal AtlasTask<RpcReply<bool>> GiveClientToAsync(ClientHandoffTarget target,
        AtlasCancellationToken ct = default)
    {
        ThreadGuard.EnsureMainThread();
        var requestId = AtlasRpc.NextRequestId();
        var source = AtlasRpcSource<RpcReply<bool>>.Rent();
        source.Start(NativeRpcRegistry.Instance, MessageIds.EntityRpcReply, requestId,
            static (ref SpanReader reader) => RpcReply<bool>.Ok(reader.ReadBool()),
            RpcReplyHelpers.For<bool>(), target.ProcessInstance == 0 ? AtlasRpc.DefaultTimeoutMs : 40000,
            ct, LifecycleCancellation);
        var task = source.Task;
        if (!task.GetAwaiter().IsCompleted &&
            NativeApi.TransferClient(EntityId, target.EntityId, target.BaseIp, target.BasePort,
                target.ProcessInstance, requestId) != RpcSendResult.Queued)
            source.FailSend();
        return task;
    }

    // hysteresis is the leave-band width: enter at radius, leave at
    // radius + hysteresis. Queued until cell EnableWitness lands.
    public void SetAoIRadius(float radius, float hysteresis = 5f)
    {
        NativeApi.SetAoIRadius(EntityId, radius, hysteresis);
    }

    // Accounts detached after GiveClientToAsync need an explicit snapshot;
    // BaseApp automatically saves the entity owning the client during logoff.
    public void PersistToDb()
    {
        var w = new SpanWriter(256);
        try
        {
            Serialize(ref w);
            var result = NativeApi.WriteEntityToDb(EntityId, w.WrittenSpan);
            if (result != RpcSendResult.Queued)
                throw new InvalidOperationException($"Persistence request rejected: {result}");
        }
        finally { w.Dispose(); }
    }

    public AtlasTask<RpcReply<long>> PersistToDbAsync(AtlasCancellationToken ct = default)
    {
        ThreadGuard.EnsureMainThread();
        var requestId = AtlasRpc.NextRequestId();
        var source = AtlasRpcSource<RpcReply<long>>.Rent();
        source.Start(NativeRpcRegistry.Instance, MessageIds.EntityRpcReply, requestId,
            static (ref SpanReader reader) => RpcReply<long>.Ok(reader.ReadInt64()),
            RpcReplyHelpers.For<long>(), AtlasRpc.DefaultTimeoutMs, ct, LifecycleCancellation);
        var task = source.Task;
        if (task.GetAwaiter().IsCompleted) return task;
        var writer = new SpanWriter(256);
        try
        {
            Serialize(ref writer);
            if (!task.GetAwaiter().IsCompleted &&
                NativeApi.WriteEntityToDb(EntityId, writer.WrittenSpan, requestId) !=
                RpcSendResult.Queued)
                source.FailSend();
        }
        catch (Exception ex)
        {
            Atlas.Diagnostics.Log.Error($"Persistence serialization failed for entity {EntityId}: {ex}");
            source.FailSend();
        }
        finally { writer.Dispose(); }
        return task;
    }
}

public readonly struct ClientHandoffTarget
{
    public ClientHandoffTarget(uint entityId, uint baseIp, ushort basePort, ulong processInstance)
    {
        EntityId = entityId;
        BaseIp = baseIp;
        BasePort = basePort;
        ProcessInstance = processInstance;
    }

    public uint EntityId { get; }
    public uint BaseIp { get; }
    public ushort BasePort { get; }
    public ulong ProcessInstance { get; }
}
