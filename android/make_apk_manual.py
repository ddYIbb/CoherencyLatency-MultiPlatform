import html, re, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parent
src = ROOT / "docs" / "APK使用说明.md"
dst = ROOT / "docs" / "APK使用说明.html"
lines = src.read_text(encoding="utf-8").splitlines()

def inline(s):
    s = html.escape(s, quote=False)
    s = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", r'<img alt="\1" src="\2">', s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"\x60([^\x60]+)\x60", r"<code>\1</code>", s)
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', s)
    return s

def is_table_sep(s):
    return bool(re.match(r"^\s*\|?[\s:|-]+\|[\s:|-]*$", s)) and "-" in s

out = []
i = 0
while i < len(lines):
    line = lines[i]
    if line.startswith("\x60\x60\x60"):
        i += 1
        code = []
        while i < len(lines) and not lines[i].startswith("\x60\x60\x60"):
            code.append(lines[i]); i += 1
        i += 1
        out.append("<pre><code>" + html.escape("\n".join(code)) + "</code></pre>")
        continue
    if line.strip() == "":
        i += 1
        continue
    if line.strip() == "---":
        out.append("<hr>"); i += 1; continue
    m = re.match(r"^(#{1,6}) (.*)$", line)
    if m:
        level = len(m.group(1))
        out.append(f"<h{level}>{inline(m.group(2))}</h{level}>")
        i += 1
        continue
    if line.startswith("> "):
        block = []
        while i < len(lines) and lines[i].startswith("> "):
            block.append(lines[i][2:]); i += 1
        out.append("<blockquote><p>" + inline(" ".join(block)) + "</p></blockquote>")
        continue
    if line.lstrip().startswith("|") and i + 1 < len(lines) and is_table_sep(lines[i + 1]):
        header = [c.strip() for c in line.strip().strip("|").split("|")]
        i += 2
        rows = []
        while i < len(lines) and lines[i].lstrip().startswith("|"):
            rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
            i += 1
        parts = ["<table><thead><tr>"]
        parts += [f"<th>{inline(c)}</th>" for c in header]
        parts.append("</tr></thead><tbody>")
        for row in rows:
            parts.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in row) + "</tr>")
        parts.append("</tbody></table>")
        out.append("".join(parts))
        continue
    if re.match(r"^\s*[-*] ", line):
        items = []
        while i < len(lines) and re.match(r"^\s*[-*] ", lines[i]):
            items.append(re.sub(r"^\s*[-*] ", "", lines[i])); i += 1
        out.append("<ul>" + "".join(f"<li>{inline(x)}</li>" for x in items) + "</ul>")
        continue
    if re.match(r"^\s*\d+\. ", line):
        items = []
        while i < len(lines) and re.match(r"^\s*\d+\. ", lines[i]):
            items.append(re.sub(r"^\s*\d+\. ", "", lines[i])); i += 1
        out.append("<ol>" + "".join(f"<li>{inline(x)}</li>" for x in items) + "</ol>")
        continue
    para = [line]; i += 1
    while i < len(lines) and lines[i].strip() != "" and not lines[i].startswith(("#", "> ", "|")) and not lines[i].startswith("\x60\x60\x60") and not re.match(r"^\s*[-*] ", lines[i]) and not re.match(r"^\s*\d+\. ", lines[i]) and lines[i].strip() != "---":
        para.append(lines[i]); i += 1
    out.append("<p>" + inline(" ".join(para)) + "</p>")

body = "\n".join(out)
html_doc = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CoherencyLatency Android 图形界面版 · 使用说明</title>
<style>
  :root { color-scheme: light; }
  body { font-family: -apple-system, BlinkMacSystemFont, "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", Arial, sans-serif; max-width: 900px; margin: 0 auto; padding: 34px 28px 80px; line-height: 1.75; color: #1f2328; }
  h1 { font-size: 2em; border-bottom: 2px solid #d0d7de; padding-bottom: .3em; }
  h2 { margin-top: 2em; border-bottom: 1px solid #d8dee4; padding-bottom: .25em; }
  h3 { margin-top: 1.4em; }
  code { background: #f1f3f5; border-radius: 4px; padding: .12em .3em; font-family: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace; font-size: .9em; }
  pre { background: #f6f8fa; border: 1px solid #d8dee4; border-radius: 6px; padding: 14px 16px; overflow: auto; line-height: 1.45; }
  pre code { background: none; padding: 0; }
  blockquote { margin: 1em 0; padding: .7em 1em; border-left: 4px solid #d97706; background: #fff8eb; color: #5f3b00; }
  table { border-collapse: collapse; width: 100%; margin: 1em 0; }
  th, td { border: 1px solid #d8dee4; padding: 8px 10px; text-align: left; vertical-align: top; }
  th { background: #f6f8fa; }
  a { color: #0969da; }
  img { display: block; max-width: 380px; width: 100%; height: auto; margin: 1.2em auto; border: 1px solid #d8dee4; border-radius: 12px; }
  img[src*="watch"] { max-width: 280px; }
  img[src*="tablet"], img[src*="tv-"] { max-width: 720px; border-radius: 8px; }
  hr { border: none; border-top: 1px solid #d8dee4; margin: 2em 0; }
  @media print { body { max-width: none; padding: 0; margin: 14mm 16mm; } a { color: #000; text-decoration: none; } pre, blockquote, img, table { break-inside: avoid; } h1, h2, h3 { break-after: avoid; } img { max-width: 300px; } img[src*="tablet"], img[src*="tv-"] { max-width: 620px; } }
</style>
</head>
<body>
""" + body + """
</body>
</html>
"""
dst.write_text(html_doc, encoding="utf-8")
print(dst)
