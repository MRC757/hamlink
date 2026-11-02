// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties

/**
 * Help: docs/help/HELP.md (packaged as an asset; the Windows app embeds the same file). Renders the small Markdown
 * subset that file uses: # headings, paragraphs, "- " bullets, "1. " steps, ``` code blocks, **bold** and `code`.
 */
@Composable
fun HelpDialog(onClose: () -> Unit) {
    val ctx = LocalContext.current
    val blocks = remember {
        val md = try { ctx.assets.open("HELP.md").bufferedReader().use { it.readText() } }
                 catch (_: Exception) { "# Help\n\nThe help text is missing from this build." }
        parse(md)
    }
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().safeDrawingPadding()) {
                Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Help", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
                    Button(onClick = onClose) { Text("Close") }
                }
                SelectionContainer(Modifier.weight(1f)) {
                    Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        blocks.forEach { Block(it) }
                    }
                }
            }
        }
    }
}

private sealed class HelpBlock {
    class Heading(val level: Int, val text: String) : HelpBlock()
    class Para(val text: String) : HelpBlock()
    class Item(val marker: String, val text: String) : HelpBlock()
    class Code(val text: String) : HelpBlock()
}

private fun parse(md: String): List<HelpBlock> {
    val out = ArrayList<HelpBlock>()
    var cur: StringBuilder? = null      // paragraph or list item being collected
    var curMarker: String? = null       // null = paragraph, else the list item's marker
    var code: StringBuilder? = null
    fun flush() {
        cur?.let { out += if (curMarker == null) HelpBlock.Para(it.toString()) else HelpBlock.Item(curMarker!!, it.toString()) }
        cur = null; curMarker = null
    }
    val heading = Regex("^(#{1,3}) (.*)")
    val item = Regex("^(- |(\\d+)\\. )(.*)")
    for (raw in md.replace("\r", "").split("\n")) {
        val line = raw.trimEnd()
        val block = code
        if (line.startsWith("```")) {
            if (block == null) { flush(); code = StringBuilder() } else { out += HelpBlock.Code(block.toString().trimEnd()); code = null }
            continue
        }
        if (block != null) { block.append(raw).append('\n'); continue }
        if (line.isEmpty()) { flush(); continue }
        val h = heading.find(line)
        if (h != null) { flush(); out += HelpBlock.Heading(h.groupValues[1].length, h.groupValues[2]); continue }
        val i = item.find(line)
        if (i != null) {
            flush()
            curMarker = if (i.groupValues[2].isNotEmpty()) "${i.groupValues[2]}." else "•"
            cur = StringBuilder(i.groupValues[3])
            continue
        }
        val c = cur
        if (c != null) c.append(' ').append(line.trim())   // continuation line
        else cur = StringBuilder(line)
    }
    flush()
    code?.let { out += HelpBlock.Code(it.toString().trimEnd()) }
    return out
}

/** **bold**, `code` and *italic* spans. */
private fun styled(text: String): AnnotatedString = buildAnnotatedString {
    for (m in Regex("\\*\\*(.+?)\\*\\*|`(.+?)`|\\*(.+?)\\*|[^*`]+|[*`]").findAll(text)) {
        val (bold, code, italic) = m.destructured
        when {
            bold.isNotEmpty() -> withStyle(SpanStyle(fontWeight = FontWeight.Bold)) { append(bold) }
            code.isNotEmpty() -> withStyle(SpanStyle(fontFamily = FontFamily.Monospace)) { append(code) }
            italic.isNotEmpty() -> withStyle(SpanStyle(fontStyle = FontStyle.Italic)) { append(italic) }
            else -> append(m.value)
        }
    }
}

@Composable
private fun Block(b: HelpBlock) {
    when (b) {
        is HelpBlock.Heading -> Text(styled(b.text), modifier = Modifier.padding(top = if (b.level == 1) 0.dp else 10.dp),
            style = when (b.level) { 1 -> MaterialTheme.typography.headlineSmall; 2 -> MaterialTheme.typography.titleLarge
                                     else -> MaterialTheme.typography.titleMedium })
        is HelpBlock.Para -> Text(styled(b.text), style = MaterialTheme.typography.bodyMedium)
        is HelpBlock.Item -> Row {
            Text(b.marker, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.width(22.dp))
            Text(styled(b.text), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
        }
        is HelpBlock.Code -> Text(b.text, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surfaceVariant).padding(8.dp))
    }
}
