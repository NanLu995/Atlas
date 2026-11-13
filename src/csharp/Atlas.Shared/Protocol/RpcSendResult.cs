namespace Atlas.Protocol;

public enum RpcSendResult : int
{
    Queued = 0,
    InvalidTarget = 1,
    Overloaded = 2,
    Stopping = 3,
    SendFailed = 4,
    PartiallyQueued = 5,
}
