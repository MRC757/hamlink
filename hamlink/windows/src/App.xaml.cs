// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Windows;

namespace HamLink;

public partial class App : Application
{
    public static string LogPath { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HamLink", "error.log");

    protected override void OnStartup(StartupEventArgs e)
    {
        DispatcherUnhandledException += (_, ex) =>
        {
            LogError("UI thread", ex.Exception);
            MessageBox.Show($"{ex.Exception.Message}\n\nDetails were saved to:\n{LogPath}", "HamLink error");
            ex.Handled = true;
        };
        AppDomain.CurrentDomain.UnhandledException += (_, ex) =>
        {
            if (ex.ExceptionObject is Exception x) LogError("background thread", x);
        };

        string? problem = CheckNativeLibrary();
        if (problem != null)
        {
            MessageBox.Show(problem, "HamLink cannot start", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
            return;
        }
        base.OnStartup(e);
    }

    /// <summary>Fails fast if hamlink_core.dll is missing, unloadable or a different API version, instead of
    /// surfacing later as an obscure EntryPointNotFoundException from whichever screen calls it first.</summary>
    static string? CheckNativeLibrary()
    {
        try
        {
            int v = NativeMethods.hl_api_version();
            if (v == NativeMethods.ApiVersion) return null;
            return $"hamlink_core.dll is API version {v}, but this app needs version {NativeMethods.ApiVersion}.\n\n" +
                   "Rebuild the native library (windows/build-native.sh) or the app so they match.";
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            LogError("startup native library check", ex);
            return $"hamlink_core.dll could not be loaded: {ex.Message}\n\n" +
                   $"It must be next to HamLink.exe in:\n{AppContext.BaseDirectory}";
        }
    }

    public static void LogError(string where, Exception ex)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
            File.AppendAllText(LogPath, $"==== {DateTime.Now:yyyy-MM-dd HH:mm:ss} ({where}) ====\n{ex}\n\n");
        }
        catch { /* logging must never take the app down */ }
    }
}
