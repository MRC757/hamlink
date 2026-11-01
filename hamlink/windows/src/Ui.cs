// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;

namespace HamLink;

internal static class Ui
{
    /// <summary>Enables or disables a control; while disabled, hovering it shows why.</summary>
    public static void Enable(Control c, bool on, string whyDisabled)
    {
        c.IsEnabled = on;
        c.ToolTip = on ? null : whyDisabled;
        ToolTipService.SetShowOnDisabled(c, true);
    }
}
