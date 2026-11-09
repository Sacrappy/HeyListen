using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace HookshotBuilder;

/// (HeyListen) Saves to and from an emulator: the builder's face on upstream Hookshot's scripts/oot_saves.py (docs/saves.md),
/// not a converter of its own: the emulators keep the same files, so the script copies them. The player picks the emulator
/// (or a folder), the script's --dry-run says what it would do, and nothing is copied before the player says Yes. The script
/// itself backs up the side it overwrites first (here into %LOCALAPPDATA%\HeyListenBuilder\save-backups) and refuses while a
/// game is running.
static class Saves
{
    // (oot_saves.py's EMULATORS: name, what the player sees)
    static readonly (string Name, string Display)[] Emulators =
    {
        ("lichii", "Lichii"), ("frizti", "Frizti (Fritchii)"), ("orchid", "Orchid"), ("pokedinner", "PokeDinner"),
        ("eden", "Eden"), ("eglantine", "Eglantine"), ("nx2navi", "NX2navi"),
    };
    const string OtherFolder = "Another folder…";

    public static string BackupDir => Path.Combine(Storage.Base, "save-backups");

    static string? Script(Tools tools)
    {
        var path = Path.Combine(tools.Root, "scripts", "oot_saves.py");
        return File.Exists(path) ? path : null;
    }

    /// import: from an emulator into the game in gameFolder; else export: out of it
    public static async void Run(Window owner, string gameFolder, bool import)
    {
        const string caption = "HeyListen saves";
        var tools = Tools.Find();
        var script = tools == null ? null : Script(tools);
        if (tools == null || script == null)
        {
            MessageBox.Show(owner, "This builder has no save tool (scripts/oot_saves.py). Update the HeyListen Builder and try again.", caption);
            return;
        }
        if (!Directory.Exists(Path.Combine(gameFolder, "user", "save")))
        {
            MessageBox.Show(owner, $"No HeyListen save folder yet in {gameFolder}. Start the game once (and reach the title screen), quit it, " +
                                   "then copy the saves.", caption);
            return;
        }

        var (_, listed) = await Python(tools, script, "list", "--hookshot", gameFolder);
        if (Pick(owner, import, listed) is not { } choice) return;

        var args = new List<string> { import ? "import" : "export" };
        if (choice.Folder != null) args.AddRange(new[] { import ? "--from" : "--to", choice.Folder });
        else args.Add(choice.Name!);
        args.AddRange(new[] { "--hookshot", gameFolder, "--backup-dir", BackupDir });

        var (dryOk, dry) = await Python(tools, script, args.Append("--dry-run").ToArray());
        if (!dryOk)
        {
            MessageBox.Show(owner, dry.Trim(), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        var question = (import
                ? "Replace your HeyListen saves with these? Your current HeyListen saves are backed up first."
                : "Replace the emulator's saves with your HeyListen saves? The emulator's current saves are backed up first.") +
            $"\n\n{dry.Trim()}\n\nBackups go to {BackupDir}. Close every game and emulator before you continue.";
        if (MessageBox.Show(owner, question, caption, MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        var (ok, done) = await Python(tools, script, args.ToArray());
        MessageBox.Show(owner, done.Trim(), caption, MessageBoxButton.OK, ok ? MessageBoxImage.Information : MessageBoxImage.Warning);
    }

    sealed record Choice(string? Name, string? Folder);

    /// The emulator (or a folder) to copy from/to; null: cancelled
    static Choice? Pick(Window owner, bool import, string listed)
    {
        var w = new Window
        {
            Owner = owner, Title = import ? "Import saves from an emulator" : "Export saves to an emulator",
            Width = 640, Height = 480, WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize,
        };
        var root = new DockPanel { Margin = new Thickness(16) };
        var head = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8),
            Text = import ? "Copy an Ocarina of Time save from an emulator into HeyListen. Pick where it comes from:"
                          : "Copy your HeyListen save to an emulator. Pick where it goes:",
        };
        DockPanel.SetDock(head, Dock.Top);
        var box = new ComboBox { Margin = new Thickness(0, 0, 0, 8) };
        foreach (var (_, display) in Emulators) box.Items.Add(display);
        box.Items.Add(OtherFolder);
        box.SelectedIndex = 0;
        DockPanel.SetDock(box, Dock.Top);
        var found = new TextBlock { Text = "What was found on this PC:", Margin = new Thickness(0, 0, 0, 4) };
        DockPanel.SetDock(found, Dock.Top);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 10, 0, 0) };
        var go = new Button { Content = "Continue…", Padding = new Thickness(14, 4, 14, 4), IsDefault = true };
        var cancel = new Button { Content = "Cancel", Padding = new Thickness(14, 4, 14, 4), Margin = new Thickness(8, 0, 0, 0), IsCancel = true };
        buttons.Children.Add(go);
        buttons.Children.Add(cancel);
        DockPanel.SetDock(buttons, Dock.Bottom);
        var text = new TextBox
        {
            Text = listed.Trim(), IsReadOnly = true, FontFamily = new FontFamily("Consolas"), FontSize = 12,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        root.Children.Add(head);
        root.Children.Add(box);
        root.Children.Add(found);
        root.Children.Add(buttons);
        root.Children.Add(text);
        w.Content = root;

        Choice? choice = null;
        go.Click += (_, _) =>
        {
            if (box.SelectedIndex < Emulators.Length) choice = new Choice(Emulators[box.SelectedIndex].Name, null);
            else
            {
                var dialog = new Microsoft.Win32.OpenFolderDialog
                {
                    Title = import ? "The folder with the save (account.sav and slot_*), or one above it"
                                   : "The folder to copy the save into (an emulator's save folder, or one above it)",
                };
                if (dialog.ShowDialog(w) != true) return;
                choice = new Choice(null, dialog.FolderName);
            }
            w.DialogResult = true;
        };
        return w.ShowDialog() == true ? choice : null;
    }

    /// oot_saves.py with args: whether it ended well, and what it printed (stdout and stderr)
    static Task<(bool Ok, string Output)> Python(Tools tools, string script, params string[] args) => Task.Run(() =>
    {
        var si = new ProcessStartInfo(tools.Python)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
        };
        si.Environment["PYTHONIOENCODING"] = "utf-8";
        foreach (var a in tools.PythonArgs ?? Array.Empty<string>()) si.ArgumentList.Add(a);
        si.ArgumentList.Add("-I");
        si.ArgumentList.Add(script);
        foreach (var a in args) si.ArgumentList.Add(a);
        try
        {
            using var p = Process.Start(si)!;
            var err = p.StandardError.ReadToEndAsync();
            var output = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(120_000)) { try { p.Kill(); } catch (Exception) { } return (false, "The save tool did not answer."); }
            // (the script says Hookshot: HeyListen is that engine)
            var all = (output + err.Result).Replace("Hookshot", "HeyListen");
            return (p.ExitCode == 0, all);
        }
        catch (Exception e)
        {
            return (false, "The save tool could not be started: " + e.Message);
        }
    });
}
