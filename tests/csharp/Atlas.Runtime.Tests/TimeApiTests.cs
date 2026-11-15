using System;
using System.Threading.Tasks;
using Atlas.Core;
using Xunit;

namespace Atlas.Tests;

[Collection("EntityManager")]
public sealed class TimeApiTests
{
#if DEBUG
    [Fact]
    public async Task FrameDeltaRejectsWorkerThreadAccessBeforeNativeCall()
    {
        ThreadGuard.SetMainThread();
        var error = await Task.Run(() => Record.Exception(() => Atlas.Time.DeltaTime));
        var invalidThread = Assert.IsType<InvalidOperationException>(error);
        Assert.Contains("main thread", invalidThread.Message);
    }
#endif
}
