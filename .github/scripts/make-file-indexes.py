#!/usr/bin/env python3
import os
import sys
import time

def format_size(size):
    for unit in ['B', 'K', 'M', 'G']:
        if size < 1024:
            return f"{size:4.0f}{unit}" if unit != 'B' else f"{size:4d}B"
        size /= 1024
    return f"{size:4.1f}T"

def build_indexes(site_dir):
    for root, dirs, files in os.walk(site_dir):
        dirs.sort()
        files.sort()
        rel = os.path.relpath(root, site_dir)
        web_path = "/" if rel == "." else f"/{rel.replace(os.sep, '/')}/"

        lines = []
        lines.append("<!DOCTYPE html>")
        lines.append("<html>")
        lines.append("<head>")
        lines.append('  <meta charset="utf-8">')
        lines.append(f"  <title>Index of {web_path}</title>")
        lines.append("</head>")
        lines.append("<body>")
        lines.append(f"<h1>Index of {web_path}</h1>")
        lines.append("<hr>")
        lines.append("<pre>")
        lines.append(f"{'Name':<50} {'Last modified':<17} {'Size':>8}")
        lines.append("-" * 77)

        if rel != ".":
            lines.append(f'<a href="../">../</a>')

        for d in dirs:
            full = os.path.join(root, d)
            mtime = time.strftime("%Y-%m-%d %H:%M", time.gmtime(os.path.getmtime(full)))
            text = d + "/"
            pad = " " * max(1, 50 - len(text))
            lines.append(f'<a href="{d}/">{text}</a>{pad} {mtime}          -')

        for f in files:
            if f == "index.html":
                continue
            full = os.path.join(root, f)
            mtime = time.strftime("%Y-%m-%d %H:%M", time.gmtime(os.path.getmtime(full)))
            sz = format_size(os.path.getsize(full))
            pad = " " * max(1, 50 - len(f))
            lines.append(f'<a href="{f}">{f}</a>{pad} {mtime} {sz:>8}')

        lines.append("-" * 77)
        lines.append("</pre>")
        lines.append("<hr>")
        lines.append("</body>")
        lines.append("</html>")
        lines.append("")

        with open(os.path.join(root, "index.html"), "w", encoding="utf-8") as out:
            out.write("\n".join(lines))

if __name__ == "__main__":
    site = sys.argv[1] if len(sys.argv) > 1 else "site"
    build_indexes(site)
