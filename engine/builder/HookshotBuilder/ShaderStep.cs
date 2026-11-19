using System.Text.RegularExpressions;

namespace HookshotBuilder;

/// The build's last step, the shaders' preparation (nx2/make_game.py prepare_shaders): the argument that leaves it to the game's
/// first start, and what its row says once the build is done
static class ShaderStep
{
    public const string Id = "shaders";
    public const string Later = "--no-prepare-shaders";
    // (make_game.py NOT_PREPARED: a plain line, so a builder that does not know it only shows it)
    static readonly Regex NotPrepared = new(@"^the shaders were not prepared now \((.*)\): ");

    /// make_game's arguments with the player's choice (unticked: the game prepares its shaders at its first start)
    public static List<string> Args(List<string> args, bool prepare)
    {
        if (!prepare) args.Add(Later);
        return args;
    }

    /// Why make_game left the shaders to the first start, when this line says so; else null
    public static string? Reason(string line) => NotPrepared.Match(line) is { Success: true } m ? m.Groups[1].Value : null;

    /// The note under the bar while the shaders are prepared (shader_time.py's text, which the Linux builder shows). The step took 112
    /// minutes on a PC whose graphics card has no mesh shaders, and "usually about 45 minutes" stayed on screen all that time: the
    /// time left now comes from the time taken and the game's @@progress (the programs by what they cost, then the pipelines).
    /// seconds: since the step began; fraction: its progress 0..1, below 0 when none came yet
    public static string Note(double seconds, double fraction, string tail)
    {
        var head = $"Preparing the shaders {(int)seconds / 60}:{(int)seconds % 60:00}";
        if (fraction < 0.03 || seconds < 120)
            return $"{head} · working out how long this takes. It is 10 to 40 minutes on most PCs, more with fewer CPU cores or a graphics card without mesh shaders{tail}";
        var left = seconds * (1 - Math.Min(fraction, 0.999)) / Math.Min(fraction, 0.999);
        if (left < 45) return $"{head} · Less than a minute left{tail}";
        if (left < 90) return $"{head} · About a minute left{tail}";
        var minutes = (int)Math.Round(left / 60);
        return $"{head} · {(minutes >= 60 ? $"About {minutes / 60} h {minutes % 60} min left" : $"About {minutes} minutes left")}{tail}";
    }

    /// The line under the row of a finished build when the shaders were not prepared, or null when they were (a check mark).
    /// began: the game's @@stage shaders came; reason: make_game's not-prepared line
    public static string? Skipped(bool began, string? reason) =>
        reason != null ? $"Left to the game's first start: {reason}"
        : began ? null
        : "Left to the game's first start";
}
