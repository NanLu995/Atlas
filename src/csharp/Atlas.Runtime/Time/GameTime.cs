using Atlas.Core;

namespace Atlas;

/// <summary>Reads timing values from the native server clock.</summary>
public static class Time
{
    /// <summary>Monotonic server time in seconds with an unspecified epoch.</summary>
    public static double ServerTime => NativeApi.ServerTime();

    /// <summary>Last game-frame duration in seconds, including the game clock's time scale.</summary>
    public static float DeltaTime => NativeApi.DeltaTime();
}
