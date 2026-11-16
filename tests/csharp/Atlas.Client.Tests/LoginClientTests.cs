using System;
using System.Threading.Tasks;
using Atlas.Client;
using Atlas.Client.Desktop;
using Atlas.Client.Native;
using Atlas.Coro.Rpc;
using Atlas.Protocol;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests;

public sealed class LoginClientTests
{
    [Fact]
    public async Task FailedDtlsConfigurationCannotStartPlaintextLogin()
    {
        using var client = new LoginClient(new ClientSession());
        Assert.Throws<ArgumentException>(() => client.ConfigureDtls("", "atlas.test"));
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await client.LoginAsync("127.0.0.1", 1, "account", "test-only-credential"));
    }

    [Fact]
    public async Task DesktopFacadePreservesFailedDtlsConfiguration()
    {
        using var client = new AtlasClient(installCoroLoop: false);
        Assert.Throws<ArgumentException>(() => client.ConfigureDtls("", "atlas.test"));
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await client.ConnectAsync("127.0.0.1", 1, "account", "test-only-credential"));
    }
    [Theory]
    [InlineData("", "credential")]
    [InlineData("account", "")]
    [InlineData("a\0b", "credential")]
    [InlineData("account", "p\0q")]
    public async Task InvalidCredentialsAreRejectedBeforeNativeStringConversion(
        string username, string credential)
    {
        using var client = new LoginClient(new ClientSession());
        for (int attempt = 0; attempt < 2; ++attempt)
            await Assert.ThrowsAsync<ArgumentException>(async () =>
                await client.LoginAsync("127.0.0.1", 1, username, credential));
    }

    [Fact]
    public void MailboxSendReportsDisconnectedAndDisposed()
    {
        var session = new ClientSession();
        var client = new LoginClient(session);
        Assert.Same(session, client.Session);
        Assert.Equal(RpcSendResult.InvalidTarget, client.SendBaseRpc(1, 1, default));
        Assert.Equal(RpcSendResult.InvalidTarget, client.SendCellRpc(1, 1, default));
        client.Dispose();
        Assert.Equal(RpcSendResult.Stopping, client.SendBaseRpc(1, 1, default));
        Assert.Equal(RpcSendResult.Stopping, client.SendCellRpc(1, 1, default));
        Assert.Null(session.SendBaseRpcHandler);
        Assert.Null(session.SendCellRpcHandler);
    }

    [Fact]
    public async Task NativeEventsAndDisconnectStayWithinExplicitSession()
    {
        var first = new ClientSession();
        var second = new ClientSession();
        using var left = new LoginClient(first);
        using var right = new LoginClient(second);
        var a = AtlasRpc.Await<int>(first.RpcRegistry, 102, 17,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        var b = AtlasRpc.Await<int>(second.RpcRegistry, 102, 17,
            static (ref SpanReader reader) => RpcReply<int>.Ok(reader.ReadInt32()), timeoutMs: 0);
        ((IAtlasNetEvents)left).OnDisconnect(0);
        Assert.Equal(RpcErrorCodes.ReceiverGone, (await a).Error);
        Assert.False(b.GetAwaiter().IsCompleted);
        var writer = new SpanWriter(16);
        byte[] reply;
        try
        {
            writer.WriteUInt64(17);
            writer.WriteInt32(0);
            writer.WriteInt32(71);
            reply = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }
        ((IAtlasNetEvents)left).OnDeliver(102, reply);
        Assert.False(b.GetAwaiter().IsCompleted);
        ((IAtlasNetEvents)right).OnDeliver(102, reply);
        Assert.Equal(71, (await b).Value);
    }

    [Fact]
    public void StandaloneMailboxTransportExposesSessionAndSendResults()
    {
        var type = typeof(Atlas.Client.Desktop.LoginClient);
        Assert.Equal(typeof(ClientSession), type.GetProperty("Session")?.PropertyType);
        Assert.Equal(typeof(RpcSendResult), type.GetMethod("SendBaseRpc")?.ReturnType);
        Assert.Equal(typeof(RpcSendResult), type.GetMethod("SendCellRpc")?.ReturnType);
    }

    [Fact]
    public void LoginResult_StoresHostAndPort()
    {
        var r = new LoginResult("10.0.0.5", 20013);
        Assert.Equal("10.0.0.5", r.BaseAppHost);
        Assert.Equal((ushort)20013, r.BaseAppPort);
    }

    [Fact]
    public void AuthResult_StoresEntityIdAndTypeId()
    {
        var r = new AuthResult(0x01000003, 7);
        Assert.Equal(0x01000003u, r.EntityId);
        Assert.Equal((ushort)7, r.TypeId);
    }

    [Fact]
    public void LoginFailedException_KeepsStatusAndMessage()
    {
        var ex = new LoginFailedException(AtlasLoginStatus.InvalidCredentials, "bad password");
        Assert.Equal(AtlasLoginStatus.InvalidCredentials, ex.Status);
        Assert.Equal("bad password", ex.Message);
    }

    [Fact]
    public void AuthFailedException_KeepsMessage()
    {
        var ex = new AuthFailedException("not yet ready");
        Assert.Equal("not yet ready", ex.Message);
    }

    [Fact]
    public void AtlasClientType_ExposesConnectAsyncSignature()
    {
        var t = typeof(Atlas.Client.Desktop.AtlasClient);
        var method = t.GetMethod("ConnectAsync");
        Assert.NotNull(method);
        var ps = method!.GetParameters();
        Assert.Equal(5, ps.Length);
        Assert.Equal("loginAppHost", ps[0].Name);
        Assert.Equal("loginAppPort", ps[1].Name);
        Assert.Equal("username", ps[2].Name);
        Assert.Equal("passwordHash", ps[3].Name);
    }
}
