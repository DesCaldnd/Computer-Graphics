#!/usr/bin/env python3
"""Собирает всю документацию OxwaldEngine в один PDF.

markdown -> HTML (таблицы, fenced code, pygments) -> PDF через headless Chrome/Edge/Chromium.
Картинки встраиваются (data URI), ссылки между .md переписываются во внутренние якоря.
Номера страниц в колонтитуле и закладки делает сам браузер (нужен Chrome/Edge >= 131).

    python tools/docs_pdf/build_docs_pdf.py [--output build/docs/OxwaldEngine-docs.pdf]
        [--include GLOB ...] [--exclude GLOB ...] [--html-only] [--browser PATH]
"""
from __future__ import annotations

import argparse
import base64
import fnmatch
import html
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SECTION_GUIDE = "Руководство пользователя"
SECTION_DEV = "Документация разработчика"
SECTION_REPORTS = "Отчёты и история изменений"


@dataclass
class Doc:
    path: Path          # absolute
    rel: str            # posix path relative to the root
    section: str
    id: str = ""
    title: str = ""
    html: str = ""
    toc: list = field(default_factory=list)   # [(level, id, title)] h1..h2


def doc_id(rel: str) -> str:
    return "d-" + re.sub(r"[^0-9a-z]+", "-", rel.lower()).strip("-")


def slugify(value: str, separator: str = "-") -> str:
    """Якорь заголовка: сохраняет юникод (кириллицу), как GitHub."""
    value = re.sub(r"<[^>]+>", "", value).strip().lower()
    value = re.sub(r"[^\w\s-]", "", value, flags=re.UNICODE)
    return re.sub(r"[\s-]+", separator, value).strip(separator) or "section"


# ---------------------------------------------------------------- discovery

def _md(root: Path, pattern: str):
    return sorted(p for p in root.glob(pattern) if p.is_file())


def discover_docs(root: Path, include=(), exclude=()) -> list[Doc]:
    """Документы в порядке: руководство, примеры, разработчикам, отчёты/changelog."""
    root = root.resolve()
    ordered: list[tuple[Path, str]] = []

    def add(paths, section):
        for p in paths:
            ordered.append((p, section))

    add([root / "README.md"], SECTION_GUIDE)
    guide = root / "docs/guide"
    add([guide / "README.md"], SECTION_GUIDE)
    chapters = [p for p in _md(guide, "*.md") if re.match(r"\d+-", p.name)]
    chapters.sort(key=lambda p: (int(re.match(r"(\d+)-", p.name).group(1)), p.name))
    add(chapters, SECTION_GUIDE)
    add([root / "samples/OxwaldShowcase/README.md"], SECTION_GUIDE)
    dev = root / "docs/dev"
    add([dev / "ARCHITECTURE.md"], SECTION_DEV)
    add(_md(dev, "modules/*.md"), SECTION_DEV)
    add([p for p in _md(dev, "*.md") if p.name != "ARCHITECTURE.md"], SECTION_DEV)
    add(_md(dev, "reports/*.md"), SECTION_REPORTS)
    add([root / "CHANGELOG.md"], SECTION_REPORTS)

    docs, seen = [], set()
    for p, section in ordered:
        if not p.is_file() or p in seen:
            continue
        seen.add(p)
        rel = p.relative_to(root).as_posix()
        if include and not any(fnmatch.fnmatch(rel, g) for g in include):
            continue
        if any(fnmatch.fnmatch(rel, g) for g in exclude):
            continue
        docs.append(Doc(path=p, rel=rel, section=section, id=doc_id(rel)))
    return docs


# ---------------------------------------------------------------- rendering

_ATTR = re.compile(r'\b(href|src)="([^"]*)"')
_EXTERNAL = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")


def make_markdown(prefix: str):
    import markdown
    return markdown.Markdown(
        extensions=["tables", "fenced_code", "toc", "codehilite", "sane_lists", "attr_list", "md_in_html"],
        extension_configs={
            "toc": {"slugify": lambda v, s: prefix + slugify(v, s), "toc_depth": "1-3"},
            "codehilite": {"guess_lang": False, "css_class": "hl", "noclasses": False},
        })


def resolve_image(src: str, doc_dir: Path):
    """Путь картинки на диске или None, если это не локальный файл."""
    if _EXTERNAL.match(src) or src.startswith("data:") or src.startswith("//"):
        return None
    p = (doc_dir / urllib.parse.unquote(src.split("#")[0].split("?")[0])).resolve()
    return p if p.is_file() else None


MAX_IMAGE_WIDTH = 1500     # px; A4 минус поля при ~210 dpi
_image_cache: dict[Path, str] = {}


def embed_image(p: Path) -> str:
    """data URI; растровые картинки шире MAX_IMAGE_WIDTH уменьшаются и перекодируются (нужен Pillow, иначе как есть)."""
    if p in _image_cache:
        return _image_cache[p]
    mime = mimetypes.guess_type(p.name)[0] or "application/octet-stream"
    data = p.read_bytes()
    if mime in ("image/png", "image/jpeg", "image/bmp") and len(data) > 150_000:
        try:
            import io
            from PIL import Image
            im = Image.open(io.BytesIO(data))
            if im.width > MAX_IMAGE_WIDTH:
                im = im.resize((MAX_IMAGE_WIDTH, round(im.height * MAX_IMAGE_WIDTH / im.width)), Image.LANCZOS)
            buf = io.BytesIO()
            if im.mode == "P":
                im = im.convert("RGBA")
            if im.mode in ("RGBA", "LA") and im.getchannel("A").getextrema()[0] == 255:
                im = im.convert("RGB")      # непрозрачный альфа-канал не нужен
            if im.mode in ("RGBA", "LA"):
                im.save(buf, "PNG", optimize=True)
                mime = "image/png"
            else:
                im.convert("RGB").save(buf, "JPEG", quality=85)
                mime = "image/jpeg"
            if len(buf.getvalue()) < len(data):
                data = buf.getvalue()
        except ImportError:
            pass
    uri = f"data:{mime};base64," + base64.b64encode(data).decode("ascii")
    _image_cache[p] = uri
    return uri


def rewrite_links(body: str, doc: Doc, root: Path, by_rel: dict[str, Doc], warn=None) -> str:
    """src -> data URI; href на .md из набора -> #якорь; прочие локальные ссылки -> обычный текст."""
    warn = warn or (lambda m: None)
    doc_dir = doc.path.parent

    def sub(m):
        attr, val = m.group(1), html.unescape(m.group(2))
        if attr == "src":
            img = resolve_image(val, doc_dir)
            if img is None:
                if not (_EXTERNAL.match(val) or val.startswith("data:")):
                    warn(f"{doc.rel}: image not found: {val}")
                return m.group(0)
            return f'src="{embed_image(img)}"'
        if val.startswith("#"):
            frag = urllib.parse.unquote(val[1:])
            return f'href="#{doc.id}-{slugify(frag)}"' if frag else m.group(0)
        if _EXTERNAL.match(val) or val.startswith("//"):
            return m.group(0)
        path, _, frag = val.partition("#")
        path = urllib.parse.unquote(path)
        target = (doc_dir / path).resolve()
        try:
            rel = target.relative_to(root).as_posix()
        except ValueError:
            rel = None
        if rel in by_rel:
            t = by_rel[rel]
            frag = urllib.parse.unquote(frag)
            return f'href="#{t.id}-{slugify(frag)}"' if frag else f'href="#{t.id}"'
        return 'data-local-link="1"'   # ссылка на исходник/папку: в PDF она бесполезна

    return _ATTR.sub(sub, body)


def render_doc(doc: Doc, root: Path, by_rel: dict[str, Doc], warn=None) -> None:
    text = doc.path.read_text(encoding="utf-8")
    md = make_markdown(doc.id + "-")
    body = md.convert(text)
    body = rewrite_links(body, doc, root, by_rel, warn)
    # <a data-local-link> без href -> <span>, чтобы не выглядело ссылкой
    body = re.sub(r'<a data-local-link="1"([^>]*)>(.*?)</a>', r'<span class="srclink"\1>\2</span>', body, flags=re.S)
    doc.html = body
    doc.toc = []

    def walk(tokens):
        for t in tokens:
            if t["level"] <= 2:
                doc.toc.append((t["level"], t["id"], t["name"]))
            walk(t["children"])
    walk(md.toc_tokens)
    h1 = next((t for t in doc.toc if t[0] == 1), None)
    doc.title = html.unescape(re.sub(r"<[^>]+>", "", h1[2])) if h1 else doc.path.stem
    if h1 is None:
        doc.html = f'<h1 id="{doc.id}-title">{html.escape(doc.title)}</h1>' + doc.html
        doc.toc.insert(0, (1, doc.id + "-title", html.escape(doc.title)))


# ---------------------------------------------------------------- html page

CSS = r"""
@page { size: A4; margin: 18mm 16mm 20mm 16mm;
  @bottom-left { content: "OxwaldEngine — документация"; font: 8pt 'Segoe UI', Arial, sans-serif; color: #777; }
  @bottom-right { content: counter(page); font: 8pt 'Segoe UI', Arial, sans-serif; color: #777; } }
@page cover { margin: 0; @bottom-left { content: none; } @bottom-right { content: none; } }
html { font: 10pt/1.45 'Segoe UI', 'Noto Sans', 'DejaVu Sans', Arial, sans-serif; color: #1b1f24; }
body { margin: 0; overflow-wrap: anywhere; }
.cover { page: cover; height: 297mm; box-sizing: border-box; padding: 90mm 25mm 0; background: #14213d; color: #fff; break-after: page; }
.cover h1 { font-size: 40pt; margin: 0 0 6mm; border: 0; color: #fff; }
.cover p { font-size: 15pt; margin: 2mm 0; color: #cdd6ea; }
.doc { break-before: page; }
.toc-page { break-before: page; }
h1 { font-size: 22pt; border-bottom: 2px solid #3b5bdb; padding-bottom: 3pt; margin: 0 0 10pt; color: #14213d; }
h2 { font-size: 15pt; margin: 18pt 0 6pt; border-bottom: 1px solid #d0d7de; padding-bottom: 2pt; color: #14213d; }
h3 { font-size: 12pt; margin: 14pt 0 4pt; } h4 { font-size: 10.5pt; margin: 10pt 0 3pt; }
h1, h2, h3, h4 { break-after: avoid; }
p, li { orphans: 3; widows: 3; }
a { color: #1c5fd1; text-decoration: none; }
.srclink { color: #444; }
code { font: 8.6pt Consolas, 'DejaVu Sans Mono', 'Cascadia Mono', monospace; background: #eff1f4; padding: 0 2pt; border-radius: 2pt; }
pre { background: #f6f8fa; border: 1px solid #d8dee4; border-radius: 3pt; padding: 5pt 7pt; margin: 6pt 0;
      white-space: pre-wrap; overflow-wrap: anywhere; font-size: 8pt; line-height: 1.35; break-inside: auto; }
pre code { background: none; padding: 0; font-size: 8pt; }
.hl { background: #f6f8fa; }
table { border-collapse: collapse; width: 100%; margin: 7pt 0; font-size: 8.8pt; table-layout: auto; }
th, td { border: 1px solid #c9d1d9; padding: 3pt 5pt; vertical-align: top; text-align: left; }
th { background: #e8edf5; } tr:nth-child(even) td { background: #fafbfc; }
tr { break-inside: avoid; }
img { max-width: 100%; height: auto; break-inside: avoid; }
blockquote { margin: 6pt 0; padding: 2pt 10pt; border-left: 3pt solid #9db4e8; background: #f3f6fc; color: #333; }
hr { border: 0; border-top: 1px solid #d0d7de; }
.toc h2 { border: 0; margin: 14pt 0 4pt; font-size: 13pt; color: #3b5bdb; }
.toc ul { list-style: none; margin: 0; padding: 0; }
.toc li { margin: 0; padding: 0; display: flex; gap: 6pt; }
.toc li .t { flex: 1; } .toc li .n { color: #666; min-width: 18pt; text-align: right; }
.toc .l1 { font-weight: 600; margin-top: 3pt; font-size: 10pt; }
.toc .l2 { margin-left: 14pt; font-size: 8.6pt; color: #444; }
"""


def pygments_css() -> str:
    try:
        from pygments.formatters import HtmlFormatter
        return HtmlFormatter(style="default").get_style_defs(".hl")
    except Exception:
        return ""


def build_toc(docs: list[Doc], pages: dict[str, int] | None, depth: int) -> str:
    out = ['<section class="toc-page toc"><h1 id="toc-title">Содержание</h1>']
    cur = None
    for d in docs:
        if d.section != cur:
            if cur is not None:
                out.append("</ul>")
            cur = d.section
            out.append(f"<h2>{html.escape(cur)}</h2><ul>")
        first = True
        for level, hid, name in d.toc:
            if level > depth:
                continue
            n = pages.get(hid, "") if pages else ""
            target = d.id if (level == 1 and first) else hid
            first = False
            out.append(f'<li class="l{level}"><span class="t"><a href="#{target}">{name}</a></span><span class="n">{n}</span></li>')
    if cur is not None:
        out.append("</ul>")
    out.append("</section>")
    return "\n".join(out)


def build_html(docs: list[Doc], pages=None, depth: int = 2, title="OxwaldEngine") -> str:
    parts = [f'<!doctype html><html lang="ru"><head><meta charset="utf-8"><title>{html.escape(title)} — документация</title>',
             f"<style>{CSS}\n{pygments_css()}</style></head><body>",
             f'<section class="cover"><h1>{html.escape(title)}</h1><p>Полная документация: руководство пользователя, '
             f'документация разработчика, отчёты и история изменений</p><p>{len(docs)} документов</p></section>',
             build_toc(docs, pages, depth)]
    for d in docs:
        parts.append(f'<section class="doc" id="{d.id}">{d.html}</section>')
    parts.append("</body></html>")
    return "\n".join(parts)


# ---------------------------------------------------------------- browser

def find_browser(explicit: str | None = None) -> str | None:
    if explicit:
        return explicit if (Path(explicit).is_file() or shutil.which(explicit)) else None
    env = os.environ.get("OX_DOCS_BROWSER")
    if env and Path(env).is_file():
        return env
    cands = []
    for var in ("ProgramFiles", "ProgramFiles(x86)", "LocalAppData"):
        base = os.environ.get(var)
        if base:
            cands += [Path(base) / "Google/Chrome/Application/chrome.exe",
                      Path(base) / "Microsoft/Edge/Application/msedge.exe",
                      Path(base) / "Chromium/Application/chrome.exe"]
    cands += [Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"),
              Path("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"),
              Path("/Applications/Chromium.app/Contents/MacOS/Chromium")]
    for c in cands:
        if c.is_file():
            return str(c)
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "microsoft-edge", "chrome", "msedge"):
        w = shutil.which(name)
        if w:
            return w
    return None


def html_to_pdf(browser: str, html_path: Path, pdf_path: Path, timeout: int = 900) -> None:
    pdf_path.parent.mkdir(parents=True, exist_ok=True)
    if pdf_path.exists():
        pdf_path.unlink()
    prof = tempfile.mkdtemp(prefix="oxdocs_profile_")
    try:
        cmd = [browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
               "--disable-extensions", f"--user-data-dir={prof}", "--no-pdf-header-footer",
               "--generate-pdf-document-outline", "--allow-file-access-from-files",
               f"--print-to-pdf={pdf_path}", html_path.resolve().as_uri()]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        # Chrome может завершить launcher-процесс раньше, чем допишет файл: ждём, пока размер перестанет меняться.
        deadline, last, stable = time.time() + timeout, -1, 0
        while time.time() < deadline:
            done = proc.poll() is not None
            size = pdf_path.stat().st_size if pdf_path.exists() else 0
            stable = stable + 1 if (size > 0 and size == last) else 0
            last = size
            if stable >= 4 or (done and proc.returncode not in (0, None) and size == 0):
                break
            time.sleep(0.5)
        else:
            proc.kill()
            raise RuntimeError("browser timed out")
        if proc.poll() is None:
            proc.terminate()
        err = proc.stderr.read()[-500:] if proc.stderr else ""
    finally:
        shutil.rmtree(prof, ignore_errors=True)
    if not pdf_path.is_file() or pdf_path.stat().st_size == 0:
        raise RuntimeError(f"browser produced no PDF: {err}")


def heading_pages(pdf_path: Path, docs: list[Doc]) -> dict[str, int]:
    """Номера страниц заголовков из закладок PDF; ключ - id заголовка. Пусто, если нет pypdf."""
    try:
        from pypdf import PdfReader
    except ImportError:
        return {}
    rd = PdfReader(str(pdf_path))
    flat = []

    def walk(items):
        for it in items:
            if isinstance(it, list):
                walk(it)
            else:
                try:
                    flat.append((str(it.title).strip(), rd.get_destination_page_number(it) + 1))
                except Exception:
                    pass
    walk(rd.outline)
    # заголовки в DOM идут в том же порядке, что и закладки: последовательное сопоставление
    res, pos = {}, 0
    for k, (t, _) in enumerate(flat):      # пропускаем обложку и само оглавление
        if t == "Содержание":
            pos = k + 1
            break
    for d in docs:
        for level, hid, name in d.toc:
            plain = html.unescape(re.sub(r"<[^>]+>", "", name)).strip()
            for k in range(pos, len(flat)):
                if flat[k][0] == plain:
                    res[hid] = flat[k][1]
                    pos = k + 1
                    break
    return res


# ---------------------------------------------------------------- main

def build(args) -> int:
    root = Path(args.root).resolve()
    docs = discover_docs(root, args.include, args.exclude)
    if not docs:
        print("no documents selected", file=sys.stderr)
        return 2
    by_rel = {d.rel: d for d in docs}
    warnings = []
    for d in docs:
        render_doc(d, root, by_rel, warnings.append)
    for w in warnings:
        print("warning:", w, file=sys.stderr)

    out = Path(args.output)
    if not out.is_absolute():
        out = root / out
    out.parent.mkdir(parents=True, exist_ok=True)
    html_path = out.with_suffix(".html")
    html_path.write_text(build_html(docs, None, args.toc_depth), encoding="utf-8")
    print(f"{len(docs)} documents -> {html_path}")
    if args.html_only:
        return 0

    browser = find_browser(args.browser)
    if not browser:
        print("error: no Chrome/Edge/Chromium found; pass --browser PATH or set OX_DOCS_BROWSER", file=sys.stderr)
        return 3
    print("browser:", browser)
    html_to_pdf(browser, html_path, out)
    pages = heading_pages(out, docs)
    if pages:   # второй проход: номера страниц в содержании (объём оглавления не меняется)
        html_path.write_text(build_html(docs, pages, args.toc_depth), encoding="utf-8")
        html_to_pdf(browser, html_path, out)
    if not args.keep_html:
        html_path.unlink(missing_ok=True)
    print(f"PDF: {out} ({out.stat().st_size / 1048576:.1f} MiB)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(REPO_ROOT), help="корень репозитория")
    ap.add_argument("--output", default="build/docs/OxwaldEngine-docs.pdf")
    ap.add_argument("--include", action="append", default=[], metavar="GLOB", help="оставить только пути (от корня) по маске; можно повторять")
    ap.add_argument("--exclude", action="append", default=[], metavar="GLOB", help="исключить пути по маске; можно повторять")
    ap.add_argument("--html-only", action="store_true", help="только промежуточный HTML (рядом с --output), без PDF")
    ap.add_argument("--keep-html", action="store_true", help="не удалять HTML после сборки PDF")
    ap.add_argument("--browser", help="путь к Chrome/Edge/Chromium (иначе поиск автоматически или OX_DOCS_BROWSER)")
    ap.add_argument("--toc-depth", type=int, default=2, choices=[1, 2], help="глубина оглавления")
    return build(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
