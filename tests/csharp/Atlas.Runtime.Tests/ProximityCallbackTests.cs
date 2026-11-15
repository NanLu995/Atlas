using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Atlas.Core;
using Atlas.Diagnostics;
using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

[Collection("EntityManager")]
public unsafe sealed class ProximityCallbackTests : IDisposable
{
    [ThreadStatic] private static string? _error;
    private readonly AtlasSynchronizationContext _context = new();
    private readonly AtlasSynchronizationContext? _previousContext = EngineContext.SyncContext;
    private readonly ILogBackend _previousLog = Log.Backend;
    private readonly RecordingLog _log = new();

    public ProximityCallbackTests()
    {
        EntityManager.Instance.Reset();
        ThreadGuard.SetMainThread();
        _error = null;
        ErrorBridge.RegisterNativeFunctions(&SetError, &ClearError, &GetErrorCode);
        EngineContext.SyncContext = _context;
        Log.ResetBackend();
        Log.SetBackend(_log);
    }

    public void Dispose()
    {
        ErrorBridge.RegisterNativeFunctions(null, null, null);
        EntityManager.Instance.Reset();
        EngineContext.SyncContext = _previousContext;
        Log.ResetBackend();
        Log.SetBackend(_previousLog);
    }

    private sealed class RecordingLog : ILogBackend
    {
        public readonly List<string> Errors = new();
        public void Log(int level, string message)
        {
            if (level >= 4) Errors.Add(message);
        }
    }

    [UnmanagedCallersOnly]
    private static void SetError(int code, byte* message, int length)
        => _error = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(message, length));

    [UnmanagedCallersOnly]
    private static void ClearError() => _error = null;

    [UnmanagedCallersOnly]
    private static int GetErrorCode() => _error is null ? 0 : -1;

    private static void Dispatch(uint entityId, int argument, uint peer, byte enter)
    {
        delegate* unmanaged<uint, int, uint, byte, void> callback = &NativeCallbacks.ProximityEvent;
        callback(entityId, argument, peer, enter);
    }

    private sealed class Probe : CellServerEntity
    {
        public Probe() { }
        public override string TypeName => "ProximityProbe";
        public readonly List<(bool Enter, int Argument, uint Peer)> Events = new();
        public bool Throw;
        public bool DestroyOnEnter;
        public Action? EnterAction;
        public override void Serialize(ref SpanWriter writer) { }
        public override void Deserialize(ref SpanReader reader) { }
        protected override void OnProximityEnter(int userArg, uint peerEntityId)
        {
            if (Throw) throw new InvalidOperationException("injected proximity failure");
            Events.Add((true, userArg, peerEntityId));
            EnterAction?.Invoke();
            if (DestroyOnEnter) EntityManager.Instance.Destroy(EntityId);
        }
        protected override void OnProximityLeave(int userArg, uint peerEntityId)
            => Events.Add((false, userArg, peerEntityId));
    }

    [Fact]
    public void NativeEntryDispatchesEnterAndLeaveWithTheOriginalArguments()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        Dispatch(entity.EntityId, int.MinValue, uint.MaxValue, 1);
        Dispatch(entity.EntityId, int.MaxValue, 42, 0);
        Assert.Empty(entity.Events);
        _context.ProcessQueue();
        Assert.Equal(new[] { (true, int.MinValue, uint.MaxValue), (false, int.MaxValue, 42u) },
                     entity.Events);
        Assert.Null(_error);
    }

    [Fact]
    public void MissingDestroyedGhostAndCancelledEntitiesReceiveNoCallback()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        entity.IsGhost = true;
        Dispatch(entity.EntityId, 1, 2, 1);
        entity.IsGhost = false;
        entity.TriggerLifecycleCancellation();
        Dispatch(entity.EntityId, 1, 2, 0);
        EntityManager.Instance.Destroy(entity.EntityId);
        Dispatch(entity.EntityId, 1, 2, 1);
        Dispatch(uint.MaxValue, 1, 2, 0);
        _context.ProcessQueue();
        Assert.Empty(entity.Events);
        Assert.Null(_error);
    }

    [Fact]
    public void DestroyDuringEnterPreventsALaterLeave()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        entity.DestroyOnEnter = true;
        Dispatch(entity.EntityId, 1, 2, 1);
        Dispatch(entity.EntityId, 1, 2, 0);
        _context.ProcessQueue();
        Assert.Single(entity.Events);
        Assert.Null(_error);
    }

    [Fact]
    public void ExceptionsAreLoggedWithoutLeakingIntoTheNativeErrorBridge()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        entity.Throw = true;
        Dispatch(entity.EntityId, 1, 2, 1);
        _context.ProcessQueue();
        Assert.Contains(_log.Errors, message => message.Contains("injected proximity failure"));
        Assert.Null(_error);
        entity.Throw = false;
        Dispatch(entity.EntityId, 1, 2, 2);
        Assert.Contains(_log.Errors, message => message.Contains("isEnter"));
        Assert.Null(_error);
        Assert.Empty(entity.Events);
    }

    [Fact]
    public void QueuedEventsAreDroppedAfterLifecycleCancellation()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        Dispatch(entity.EntityId, 1, 2, 1);
        entity.TriggerLifecycleCancellation();
        _context.ProcessQueue();
        Assert.Empty(entity.Events);
    }

    [Fact]
    public void UserCallbackRunsAfterTheNativeEntryReturns()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        bool insideNative = true;
        entity.EnterAction = () => Assert.False(insideNative);
        Dispatch(entity.EntityId, 1, 2, 1);
        insideNative = false;
        _context.ProcessQueue();
        Assert.Single(entity.Events);
        Assert.Empty(_log.Errors);
    }

    [Fact]
    public void HotReloadCancellationPreventsQueuedUserCodeFromRunning()
    {
        var entity = EntityManager.Instance.Create<Probe>();
        Dispatch(entity.EntityId, 1, 2, 1);
        Atlas.Hosting.HotReloadManager.CancelLifecyclesAndDrain(new[] { entity });
        Assert.Equal(0, _context.ProcessQueue());
        Assert.Empty(entity.Events);
    }

    [Fact]
    public void QueuedEventsDoNotReachAReplacementInstanceWithTheSameId()
    {
        var original = EntityManager.Instance.Create<Probe>();
        Dispatch(original.EntityId, 1, 2, 1);
        EntityManager.Instance.Reset();
        var replacement = EntityManager.Instance.Create<Probe>();
        Assert.Equal(original.EntityId, replacement.EntityId);
        _context.ProcessQueue();
        Assert.Empty(original.Events);
        Assert.Empty(replacement.Events);
    }

    [Theory]
    [InlineData(-1f)]
    [InlineData(float.NaN)]
    [InlineData(float.PositiveInfinity)]
    [InlineData(float.NegativeInfinity)]
    public void InvalidRangesAreRejectedBeforeCallingNative(float range)
    {
        var entity = EntityManager.Instance.Create<Probe>();
        Assert.Equal(0, entity.AddProximity(range));
        entity.IsGhost = true;
        Assert.Equal(0, entity.AddProximity(1));
        entity.CancelController(1);
        entity.IsGhost = false;
        entity.TriggerLifecycleCancellation();
        Assert.Equal(0, entity.AddProximity(1));
        entity.CancelController(1);
    }
}
