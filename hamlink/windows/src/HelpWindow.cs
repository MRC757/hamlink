// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;

namespace HamLink;

/// <summary>
/// Help: docs/help/HELP.md, embedded in the program (the Android app shows the same file). Renders the small Markdown
/// subset that file uses: # headings, paragraphs, "- " bullets, "1. " steps, ``` code blocks, **bold** and `code`.
/// The viewer has search built in (Ctrl+F).
/// </summary>
internal sealed class HelpWindow : Window
{
    public HelpWindow()
    {
        Title = "HamLink help";
        Width = 760;
        Height = 820;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var viewer = new FlowDocumentScrollViewer { Document = Render(Load()), VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Content = viewer;
    }

    static string Load()
    {
        using var s = typeof(HelpWindow).Assembly.GetManifestResourceStream("HELP.md");
        if (s == null) return "# Help\n\nThe help text is missing from this build. See docs/help/HELP.md.";
        using var r = new StreamReader(s);
        return r.ReadToEnd();
    }

    static FlowDocument Render(string md)
    {
        var doc = new FlowDocument { FontFamily = new FontFamily("Segoe UI"), FontSize = 14, PagePadding = new Thickness(24), TextAlignment = TextAlignment.Left };
        List? list = null;           // the bullet or numbered list being built
        Paragraph? para = null;      // the paragraph (or list item paragraph) being built
        bool inCode = false;
        Paragraph? code = null;

        void EndBlock() { para = null; }
        void EndList() { list = null; para = null; }

        foreach (string raw in md.Replace("\r", "").Split('\n'))
        {
            string line = raw.TrimEnd();
            if (line.StartsWith("```"))
            {
                if (!inCode) { EndList(); code = new Paragraph { FontFamily = new FontFamily("Consolas"), FontSize = 13, Background = new SolidColorBrush(Color.FromRgb(0xF2, 0xF2, 0xF2)), Padding = new Thickness(8) }; doc.Blocks.Add(code); }
                inCode = !inCode;
                continue;
            }
            if (inCode) { if (code!.Inlines.Count > 0) code.Inlines.Add(new LineBreak()); code.Inlines.Add(new Run(raw)); continue; }
            if (line.Length == 0) { EndBlock(); continue; }

            var h = Regex.Match(line, "^(#{1,3}) (.*)");
            if (h.Success)
            {
                EndList();
                int level = h.Groups[1].Length;
                var p = new Paragraph { FontWeight = FontWeights.SemiBold, FontSize = level == 1 ? 24 : level == 2 ? 19 : 16, Margin = new Thickness(0, level == 1 ? 0 : 14, 0, 4) };
                AddInlines(p, h.Groups[2].Value);
                doc.Blocks.Add(p);
                continue;
            }
            var item = Regex.Match(line, @"^(- |\d+\. )(.*)");
            if (item.Success)
            {
                bool numbered = item.Groups[1].Value != "- ";
                if (list == null || (list.MarkerStyle == TextMarkerStyle.Decimal) != numbered)
                {
                    list = new List { MarkerStyle = numbered ? TextMarkerStyle.Decimal : TextMarkerStyle.Disc, Margin = new Thickness(0, 2, 0, 6) };
                    doc.Blocks.Add(list);
                }
                para = new Paragraph { Margin = new Thickness(0, 0, 0, 4) };
                AddInlines(para, item.Groups[2].Value);
                list.ListItems.Add(new ListItem(para));
                continue;
            }
            if (para != null) { para.Inlines.Add(new Run(" ")); AddInlines(para, line.Trim()); continue; } // continuation line
            EndList();
            para = new Paragraph { Margin = new Thickness(0, 0, 0, 8) };
            AddInlines(para, line);
            doc.Blocks.Add(para);
        }
        return doc;
    }

    /// <summary>Text with **bold** and `code` spans.</summary>
    static void AddInlines(Paragraph p, string text)
    {
        foreach (Match m in Regex.Matches(text, @"\*\*(.+?)\*\*|`(.+?)`|\*(.+?)\*|[^*`]+|[*`]"))
        {
            if (m.Groups[1].Success) p.Inlines.Add(new Bold(new Run(m.Groups[1].Value)));
            else if (m.Groups[2].Success) p.Inlines.Add(new Run(m.Groups[2].Value) { FontFamily = new FontFamily("Consolas") });
            else if (m.Groups[3].Success) p.Inlines.Add(new Italic(new Run(m.Groups[3].Value)));
            else p.Inlines.Add(new Run(m.Value));
        }
    }
}
