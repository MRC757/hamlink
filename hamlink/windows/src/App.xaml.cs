// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;

namespace HamLink;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, ex) =>
        {
            MessageBox.Show(ex.Exception.Message, "HamLink error");
            ex.Handled = true;
        };
    }
}
