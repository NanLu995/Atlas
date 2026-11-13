using System;
using System.Collections.Generic;

namespace Atlas.Coro.Rpc;

// In-memory IAtlasRpcRegistry. Client uses it directly; server tests too.
// Single-threaded — all calls expected on the IAtlasLoop main thread.
public sealed class ManagedRpcRegistry : IAtlasRpcRegistry, IDisposable
{
    public const int MaxPendingRpcs = 4096;
    private readonly IAtlasLoop _loop;
    private readonly Dictionary<long, Pending> _byHandle = new();
    private readonly Dictionary<(int, ulong), long> _byKey = new();
    private long _nextHandle;
    private bool _disposed;

    private static readonly Action<object?> TimerFiredCallback =
        static state =>
        {
            var (registry, handle) = ((ManagedRpcRegistry, long))state!;
            registry.OnTimeout(handle);
        };

    public ManagedRpcRegistry(IAtlasLoop loop)
    {
        _loop = loop ?? throw new ArgumentNullException(nameof(loop));
    }

    public int PendingCount => _byHandle.Count;

    public long RegisterPending(int replyId, ulong requestId, int timeoutMs,
        IAtlasRpcCallback callback)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));
        var key = (replyId, requestId);
        if (_disposed || replyId <= 0 || requestId == 0 || _nextHandle == long.MaxValue ||
            _byHandle.Count >= MaxPendingRpcs ||
            _byKey.ContainsKey(key)) return 0;

        var handle = ++_nextHandle;
        long timerHandle = 0;
        if (timeoutMs > 0)
        {
            timerHandle = _loop.RegisterTimer(timeoutMs, TimerFiredCallback, (this, handle));
        }
        _byHandle[handle] = new Pending
        {
            Key = key,
            Callback = callback,
            TimerHandle = timerHandle,
        };
        _byKey[key] = handle;
        return handle;
    }

    public void CancelPending(long handle)
    {
        CancelInternal(handle, RpcCompletionStatus.Cancelled);
    }

    // Returns true if a pending entry claimed the reply.
    public bool TryDispatch(int replyId, ulong requestId, ReadOnlySpan<byte> payload)
    {
        var key = (replyId, requestId);
        if (!_byKey.TryGetValue(key, out var handle)) return false;
        if (!_byHandle.TryGetValue(handle, out var pending)) return false;

        if (pending.TimerHandle != 0) _loop.CancelTimer(pending.TimerHandle);
        _byHandle.Remove(handle);
        _byKey.Remove(key);

        pending.Callback.OnReply(payload);
        return true;
    }

    public void Dispose() => Close(RpcCompletionStatus.Cancelled);

    public void Close(RpcCompletionStatus status)
    {
        if (_disposed) return;
        _disposed = true;

        var snapshot = new List<long>(_byHandle.Keys);
        foreach (var h in snapshot)
            CancelInternal(h, status);
    }

    private void OnTimeout(long handle)
    {
        if (!_byHandle.TryGetValue(handle, out var pending)) return;
        _byHandle.Remove(handle);
        _byKey.Remove(pending.Key);
        pending.Callback.OnError(RpcCompletionStatus.Timeout);
    }

    private void CancelInternal(long handle, RpcCompletionStatus status)
    {
        if (!_byHandle.TryGetValue(handle, out var pending)) return;
        _byHandle.Remove(handle);
        _byKey.Remove(pending.Key);
        if (pending.TimerHandle != 0) _loop.CancelTimer(pending.TimerHandle);
        pending.Callback.OnError(status);
    }

    private struct Pending
    {
        public (int, ulong) Key;
        public IAtlasRpcCallback Callback;
        public long TimerHandle;
    }
}
