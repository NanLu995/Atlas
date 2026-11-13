using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;

namespace Atlas.RpcFixture.Base;

[Entity("Probe")]
public partial class Probe : BaseServerEntity
{
    public partial AtlasTask<RpcReply<int>> BaseEcho(int value) => RpcReply<int>.Ok(value);

    public async partial AtlasTask<RpcReply<int>> ViaCell(int value) => await Cell.Echo(value);

    public partial AtlasTask<RpcReply<int>> SendBaseNotice(int value) =>
        RpcReply<int>.Ok((int)Client.Notice(value));
}
