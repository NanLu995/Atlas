using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;
using System.Collections.Generic;

namespace Atlas.RpcFixture.Base;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public partial AtlasTask<RpcReply<long>> Save(bool cancelled)
    {
        if (!cancelled) return PersistToDbAsync();
        using var cancellation = new AtlasCancellationSource();
        cancellation.Cancel();
        return PersistToDbAsync(cancellation.Token);
    }

    public partial AtlasTask<RpcReply<int>> Throw() =>
        throw new System.InvalidOperationException("fixture exception");

    public async partial AtlasTask<RpcReply<System.Collections.Generic.List<int>>> BadAsyncReply()
    {
        await AtlasTask.Delay(1, LifecycleCancellation);
        return RpcReply<System.Collections.Generic.List<int>>.Ok(null!);
    }

    private static AtlasTask<RpcReply<List<ulong>>> Describe(BaseServerEntity? entity)
    {
        if (entity is null || !entity.TryGetClientHandoffTarget(out var target))
            return RpcReply<List<ulong>>.Fail(74, "fixture target unavailable");
        return RpcReply<List<ulong>>.Ok(new List<ulong>
            { target.EntityId, target.BaseIp, target.BasePort, target.ProcessInstance });
    }

    public partial AtlasTask<RpcReply<List<ulong>>> HandoffLocation() => Describe(this);

    public async partial AtlasTask<RpcReply<int>> HandoffClient(uint entityId, uint baseIp,
        ushort basePort, ulong instance)
    {
        var result = await GiveClientToAsync(new ClientHandoffTarget(entityId, baseIp, basePort, instance));
        return result.IsOk ? RpcReply<int>.Ok(1) : RpcReply<int>.Fail(result.Error, result.ErrorMessage);
    }

    public partial AtlasTask<RpcReply<int>> Echo(int value) => Atlas.Time.DeltaTime > 0
        ? RpcReply<int>.Ok(value)
        : RpcReply<int>.Fail(76, "native BaseApp frame delta unavailable");

    public async partial AtlasTask<RpcReply<int>> DelayedEcho(int value, int delayMs)
    {
        await AtlasTask.Delay(delayMs, LifecycleCancellation);
        return RpcReply<int>.Ok(value);
    }

    public partial AtlasTask<RpcReply<int>> Reject() => RpcReply<int>.Fail(71, "fixture rejected");

    public partial void Notify(int value) => Notification = value;

    public partial AtlasTask<RpcReply<int>> ReadNotification() => RpcReply<int>.Ok(Notification);

    public async partial AtlasTask<RpcReply<uint>> AttachProbe()
    {
        var probe = EntityFactory.CreateBase("Probe", 1);
        if (probe is null) return RpcReply<uint>.Fail(72, "fixture probe creation failed");
        var moved = await GiveClientToAsync(probe.EntityId);
        if (!moved.IsOk) return RpcReply<uint>.Fail(moved.Error, moved.ErrorMessage);
        return RpcReply<uint>.Ok(probe.EntityId);
    }
}
