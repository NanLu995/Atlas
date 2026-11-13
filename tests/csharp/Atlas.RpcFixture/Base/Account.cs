using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;

namespace Atlas.RpcFixture.Base;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public partial AtlasTask<RpcReply<int>> Throw() =>
        throw new System.InvalidOperationException("fixture exception");

    public async partial AtlasTask<RpcReply<System.Collections.Generic.List<int>>> BadAsyncReply()
    {
        await AtlasTask.Delay(1, LifecycleCancellation);
        return RpcReply<System.Collections.Generic.List<int>>.Ok(null!);
    }

    private int _notification;

    public partial AtlasTask<RpcReply<int>> Echo(int value) => RpcReply<int>.Ok(value);

    public async partial AtlasTask<RpcReply<int>> DelayedEcho(int value, int delayMs)
    {
        await AtlasTask.Delay(delayMs, LifecycleCancellation);
        return RpcReply<int>.Ok(value);
    }

    public partial AtlasTask<RpcReply<int>> Reject() => RpcReply<int>.Fail(71, "fixture rejected");

    public partial void Notify(int value) => _notification = value;

    public partial AtlasTask<RpcReply<int>> ReadNotification() => RpcReply<int>.Ok(_notification);

    public partial AtlasTask<RpcReply<uint>> AttachProbe()
    {
        var probe = EntityFactory.CreateBase("Probe", 1);
        if (probe is null) return RpcReply<uint>.Fail(72, "fixture probe creation failed");
        GiveClientTo(probe.EntityId);
        return RpcReply<uint>.Ok(probe.EntityId);
    }
}
