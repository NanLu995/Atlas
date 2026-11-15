using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;
using System;
using System.Globalization;
using System.IO;

namespace Atlas.RpcFixture.Cell;

[Entity("Probe")]
public partial class Probe : CellServerEntity
{
    private int _proximityController;
    private uint _proximityPeer;
    private int _proximityEnters;

    protected override void OnProximityEnter(int userArg, uint peerEntityId)
    {
        if (userArg != 37 || peerEntityId != _proximityPeer) return;
        ++_proximityEnters;
        CancelController(_proximityController);
        _proximityController = 0;
    }

    public async partial AtlasTask<RpcReply<int>> CheckProximityCancellation()
    {
        var position = Position;
        var peer = EntityFactory.CreateLocalCell("Probe", SpaceId,
            new Atlas.DataTypes.Vector3(position.X + 100, position.Y, position.Z + 100), Direction);
        if (peer is null) return RpcReply<int>.Fail(75, "fixture local peer unavailable");
        try
        {
            _proximityPeer = peer.EntityId;
            _proximityEnters = 0;
            _proximityController = AddProximity(10, 37);
            if (_proximityController == 0)
                return RpcReply<int>.Fail(75, "fixture proximity unavailable");
            peer.Position = new Atlas.DataTypes.Vector3(position.X + 2, position.Y, position.Z + 2);
            await AtlasTask.Delay(100, LifecycleCancellation);
            return RpcReply<int>.Ok(_proximityEnters);
        }
        finally
        {
            CancelController(_proximityController);
            _proximityController = 0;
            _proximityPeer = 0;
            peer.DestroySelf();
        }
    }

    private int _notification;

    public partial AtlasTask<RpcReply<int>> Echo(int value) => Atlas.Time.DeltaTime > 0
        ? RpcReply<int>.Ok(value)
        : RpcReply<int>.Fail(76, "native CellApp frame delta unavailable");

    public async partial AtlasTask<RpcReply<int>> DelayedEcho(int value, int delayMs)
    {
        await AtlasTask.Delay(delayMs, LifecycleCancellation);
        return RpcReply<int>.Ok(value);
    }

    public async partial AtlasTask<RpcReply<int>> ViaBase(int value) => await Base.BaseEcho(value);

    public partial AtlasTask<RpcReply<int>> Reject() => RpcReply<int>.Fail(73, "cell fixture rejected");

    public partial void Notify(int value)
    {
        _notification = value;
        var directory = Environment.GetEnvironmentVariable("ATLAS_RPC_FIXTURE_DIR");
        if (string.IsNullOrEmpty(directory)) return;
        var path = Path.Combine(directory, $"notification-{EntityId}.txt");
        File.WriteAllText(path + ".tmp", value.ToString(CultureInfo.InvariantCulture));
        File.Move(path + ".tmp", path, overwrite: true);
    }

    public partial AtlasTask<RpcReply<int>> ReadNotification() => RpcReply<int>.Ok(_notification);

    public partial AtlasTask<RpcReply<int>> SendCellNotice(int value) =>
        RpcReply<int>.Ok((int)Client.Notice(value));
}
