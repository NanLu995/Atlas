using System;
using System.Runtime.InteropServices;
using Atlas.Core;
using Atlas.Diagnostics;
using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.RuntimeTest;

public static unsafe class TimerCallbackEntryPoint
{
    private sealed class Probe : CellServerEntity
    {
        public Probe() { }
        public override string TypeName => "TimerCallbackProbe";
        public override void Serialize(ref SpanWriter writer) { }
        public override void Deserialize(ref SpanReader reader) { }
    }

    private sealed class Recorder : ILogBackend
    {
        public bool SawError;
        public void Log(int level, string message)
        {
            SawError |= level >= 4 && message.Contains("injected timer failure");
        }
    }

    [UnmanagedCallersOnly]
    public static int CheckCallbacks()
    {
        var previousLog = Log.Backend;
        var recorder = new Recorder();
        try
        {
            Log.ResetBackend();
            Log.SetBackend(recorder);
            var entity = EntityManager.Instance.Create<Probe>();
            int calls = 0;
            var timer = entity.StartTimer(1, true, () =>
            {
                ++calls;
                throw new InvalidOperationException("injected timer failure");
            });
            if (timer == 0) return 1;
            delegate* unmanaged<uint, int, void> fire = &NativeCallbacks.OnTimerFired;
            ErrorBridge.Clear();
            fire(entity.EntityId, (int)timer);
            if (ErrorBridge.GetErrorCode() != 0) return 2;
            if (calls != 1 || !recorder.SawError) return 3;
            entity.IsGhost = true;
            fire(entity.EntityId, (int)timer);
            entity.IsGhost = false;
            Atlas.Hosting.HotReloadManager.CancelLifecyclesAndDrain(new[] { entity });
            if (entity.StartTimer(1, true, () => ++calls) != 0) return 5;
            fire(entity.EntityId, (int)timer);
            EntityManager.Instance.Destroy(entity.EntityId);
            fire(entity.EntityId, (int)timer);
            return calls == 1 && ErrorBridge.GetErrorCode() == 0 ? 0 : 4;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
        finally
        {
            Log.ResetBackend();
            Log.SetBackend(previousLog);
            EntityManager.Instance.Reset();
        }
    }
}
