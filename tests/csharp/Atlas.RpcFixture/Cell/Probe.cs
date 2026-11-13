using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;

namespace Atlas.RpcFixture.Cell;

[Entity("Probe")]
public partial class Probe : CellServerEntity
{
    private int _notification;

    public partial AtlasTask<RpcReply<int>> Echo(int value) => RpcReply<int>.Ok(value);

    public async partial AtlasTask<RpcReply<int>> DelayedEcho(int value, int delayMs)
    {
        await AtlasTask.Delay(delayMs, LifecycleCancellation);
        return RpcReply<int>.Ok(value);
    }

    public async partial AtlasTask<RpcReply<int>> ViaBase(int value) => await Base.BaseEcho(value);

    public partial AtlasTask<RpcReply<int>> Reject() => RpcReply<int>.Fail(73, "cell fixture rejected");

    public partial void Notify(int value) => _notification = value;

    public partial AtlasTask<RpcReply<int>> ReadNotification() => RpcReply<int>.Ok(_notification);

    public partial AtlasTask<RpcReply<int>> SendCellNotice(int value) =>
        RpcReply<int>.Ok((int)Client.Notice(value));
}
