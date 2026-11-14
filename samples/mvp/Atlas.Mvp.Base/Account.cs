using System;
using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.Mvp.Base;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public async partial void SelectAvatar(int avatarIndex)
    {
        const uint kSpaceId = 1;

        // Fast relogin reuses the Account and skips OnInit.
        string previousLastLogin = LastLogin;
        LoginCount += 1;
        LastLogin = DateTime.UtcNow.ToString("o");
        Log.Info($"[Mvp.Base] Account {EntityId} login #{LoginCount} at {LastLogin}");
        // Detached-after-GiveClientTo Accounts aren't covered by BaseApp's
        // logoff-snapshot persist path; flush state to DBApp ourselves.
        PersistToDb();

        var avatar = EntityFactory.CreateBase("Avatar", kSpaceId);
        if (avatar == null)
        {
            Log.Error("[Mvp.Base] SelectAvatar: failed to create Avatar");
            return;
        }

        var moved = await GiveClientToAsync(avatar.EntityId);
        if (!moved.IsOk)
        {
            Log.Error($"[Mvp.Base] Client transfer failed: {moved.ErrorMessage}");
            return;
        }

        // PublishReplicationFrame is no-op on BaseApp (no base→client delta path);
        // push session info via the owner-targeted ClientRpc instead.
        if (avatar is Avatar mvpAvatar)
        {
            string when = string.IsNullOrEmpty(previousLastLogin) || previousLastLogin.Length < 16
                              ? "first"
                              : previousLastLogin.Substring(5, 11);
            mvpAvatar.OnSessionInfo($"#{LoginCount} · {when}");
        }
    }
}
