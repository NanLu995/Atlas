using System;
using Atlas.Core;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Serialization;

namespace Atlas.Runtime.Coro;

// Generated dispatchers call SendReplyOnComplete after invoking a user
// reply-style RPC method. Synchronous completions short-circuit; async
// ones register a continuation that fires the reply via NativeApi when
// the task settles.
public static class EntityRpcReplyHelpers
{
    public delegate void ReplySerializer<T>(ref SpanWriter writer, T value);

    private const int kRemoteExceptionMessageMax = 512;

    public static void SendReplyOnComplete<T>(AtlasTask<RpcReply<T>> task,
        IntPtr replyChannel, ulong requestId, ReplySerializer<T> serializer)
    {
        var awaiter = task.GetAwaiter();
        if (awaiter.IsCompleted)
        {
            Send(awaiter, replyChannel, requestId, serializer);
            return;
        }
        awaiter.UnsafeOnCompleted(() => Send(awaiter, replyChannel, requestId, serializer));
    }

    private static void Send<T>(AtlasTask<RpcReply<T>>.Awaiter awaiter,
        IntPtr replyChannel, ulong requestId, ReplySerializer<T> serializer)
    {
        RpcReply<T> reply;
        try { reply = awaiter.GetResult(); }
        catch (Exception ex)
        {
            SendException(replyChannel, requestId, ex);
            return;
        }

        if (replyChannel == IntPtr.Zero) return;

        if (reply.IsOk)
        {
            var body = new SpanWriter(256);
            try
            {
                serializer(ref body, reply.Value);
                NativeApi.SendEntityRpcSuccess(replyChannel, requestId, body.WrittenSpan);
            }
            catch (Exception ex) { SendException(replyChannel, requestId, ex); }
            finally { body.Dispose(); }
        }
        else
        {
            NativeApi.SendEntityRpcFailure(replyChannel, requestId, reply.Error, reply.ErrorMessage);
        }
    }

    internal static void SendException(IntPtr replyChannel, ulong requestId, Exception exception)
    {
        if (replyChannel == IntPtr.Zero || requestId == 0) return;
        var message = exception.Message ?? "";
        if (message.Length > kRemoteExceptionMessageMax)
            message = message.Substring(0, kRemoteExceptionMessageMax);
        NativeApi.SendEntityRpcFailure(replyChannel, requestId, RpcErrorCodes.RemoteException, message);
    }
}
