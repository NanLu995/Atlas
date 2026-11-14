using System;
using System.Globalization;
using System.IO;
using Atlas.Entity;

namespace Atlas.RpcFixture.Base;

internal sealed class HandoffBootstrap : IAtlasAppInitializer
{
    private Probe? _probe;

    public void OnAppInit()
    {
        var directory = Environment.GetEnvironmentVariable("ATLAS_RPC_FIXTURE_DIR");
        if (string.IsNullOrEmpty(directory)) return;
        _probe ??= EntityFactory.CreateBase("Probe", 1) as Probe;
        if (_probe is null || !_probe.TryGetClientHandoffTarget(out var target))
        {
            AppEvents.DeferAppInit();
            return;
        }
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"base-{target.ProcessInstance}.txt");
        File.WriteAllText(path + ".tmp", string.Join(",", new[]
        {
            target.EntityId.ToString(CultureInfo.InvariantCulture),
            target.BaseIp.ToString(CultureInfo.InvariantCulture),
            target.BasePort.ToString(CultureInfo.InvariantCulture),
            target.ProcessInstance.ToString(CultureInfo.InvariantCulture)
        }));
        File.Move(path + ".tmp", path, overwrite: true);
    }
}
