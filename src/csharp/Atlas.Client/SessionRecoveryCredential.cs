using System;

namespace Atlas.Client;

// Contains a secret; keep it out of logs and ordinary configuration files.
public sealed class SessionRecoveryCredential
{
    private readonly byte[] _value;
    public long AccountDbId { get; }
    public ushort AccountTypeId { get; }
    public long ExpiresAtMs { get; }
    public ReadOnlyMemory<byte> Value => _value;

    public SessionRecoveryCredential(long accountDbId, ushort accountTypeId,
        ReadOnlySpan<byte> value, long expiresAtMs)
    {
        if (accountDbId <= 0 || accountTypeId == 0 || value.Length != 32 || expiresAtMs <= 0)
            throw new ArgumentException("Invalid recovery credential metadata");
        bool present = false;
        foreach (var item in value) present |= item != 0;
        if (!present) throw new ArgumentException("Empty recovery credential", nameof(value));
        AccountDbId = accountDbId;
        AccountTypeId = accountTypeId;
        ExpiresAtMs = expiresAtMs;
        _value = value.ToArray();
    }
}
