import struct
import zlib

import pytest

pytest.importorskip("markdown")
import build_docs_pdf as b  # noqa: E402


def png_bytes(w=4, h=4):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d))
    raw = b"".join(b"\x00" + b"\xff\x00\x00" * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


@pytest.fixture
def repo(tmp_path):
    def w(rel, text):
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    w("README.md", "# Корень\n\nСм. [главу](docs/guide/02-b.md#второй-раздел) и [код](engine/x.cpp).\n")
    w("docs/guide/README.md", "# Руководство\n")
    w("docs/guide/10-c.md", "# Глава десять\n")
    w("docs/guide/02-b.md", "# Глава два\n\n## Второй раздел\n\n"
      "![картинка](images/pic.png)\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```cpp\nint x = 1;\n```\n\n"
      "Назад: [к началу](10-c.md), [внутри](#второй-раздел), [сайт](https://example.com).\n")
    (tmp_path / "docs/guide/images").mkdir()
    (tmp_path / "docs/guide/images/pic.png").write_bytes(png_bytes())
    w("docs/dev/ARCHITECTURE.md", "# Архитектура\n")
    w("docs/dev/modules/z.md", "# Z\n")
    w("docs/dev/modules/a.md", "# A\n")
    w("docs/dev/other.md", "# Other\n")
    w("docs/dev/reports/r.md", "# Report\n")
    w("docs/dev/.idea/junk.md", "# junk\n")
    w("CHANGELOG.md", "# История\n")
    w("samples/OxwaldShowcase/README.md", "# Showcase\n")
    return tmp_path


def rels(docs):
    return [d.rel for d in docs]


def test_ordering(repo):
    assert rels(b.discover_docs(repo)) == [
        "README.md", "docs/guide/README.md", "docs/guide/02-b.md", "docs/guide/10-c.md",
        "samples/OxwaldShowcase/README.md",
        "docs/dev/ARCHITECTURE.md", "docs/dev/modules/a.md", "docs/dev/modules/z.md", "docs/dev/other.md",
        "docs/dev/reports/r.md", "CHANGELOG.md"]


def test_numeric_not_lexicographic(repo):
    (repo / "docs/guide/9-x.md").write_text("# x\n", encoding="utf-8")
    r = rels(b.discover_docs(repo))
    assert r.index("docs/guide/9-x.md") < r.index("docs/guide/10-c.md")


def test_exclude_and_include(repo):
    r = rels(b.discover_docs(repo, exclude=["docs/dev/*"]))
    assert not any(x.startswith("docs/dev") for x in r) and "CHANGELOG.md" in r
    assert rels(b.discover_docs(repo, include=["docs/guide/*.md"])) == [
        "docs/guide/README.md", "docs/guide/02-b.md", "docs/guide/10-c.md"]
    assert not any(".idea" in x for x in rels(b.discover_docs(repo)))


def test_cyrillic_anchor():
    assert b.slugify("Второй раздел") == "второй-раздел"
    assert b.slugify("Что дальше?") == "что-дальше"
    assert b.slugify("`oxdump`: архивы") == "oxdump-архивы"


def render_all(repo):
    docs = b.discover_docs(repo)
    by = {d.rel: d for d in docs}
    warns = []
    for d in docs:
        b.render_doc(d, repo.resolve(), by, warns.append)
    return by, warns


def test_cyrillic_heading_ids(repo):
    by, _ = render_all(repo)
    d = by["docs/guide/02-b.md"]
    assert f'id="{d.id}-второй-раздел"' in d.html
    assert d.title == "Глава два"


def test_link_rewriting(repo):
    by, warns = render_all(repo)
    g, root = by["docs/guide/02-b.md"], by["README.md"]
    assert f'href="#{by["docs/guide/10-c.md"].id}"' in g.html          # другая глава
    assert f'href="#{g.id}-второй-раздел"' in g.html                    # якорь внутри документа
    assert 'href="https://example.com"' in g.html                       # внешняя не трогается
    assert f'href="#{g.id}-второй-раздел"' in root.html                 # md + фрагмент
    assert 'href="engine' not in root.html and '<span class="srclink">код</span>' in root.html
    assert not warns


def test_image_embedding(repo):
    by, _ = render_all(repo)
    assert 'src="data:image/png;base64,' in by["docs/guide/02-b.md"].html
    assert b.resolve_image("images/pic.png", repo / "docs/guide") is not None
    assert b.resolve_image("nope.png", repo / "docs/guide") is None
    assert b.resolve_image("https://x/y.png", repo) is None


def test_missing_image_warns(repo):
    (repo / "docs/guide/10-c.md").write_text("# C\n\n![x](missing.png)\n", encoding="utf-8")
    _, warns = render_all(repo)
    assert any("missing.png" in w for w in warns)


def test_html_has_table_code_toc(repo):
    by, _ = render_all(repo)
    page = b.build_html(list(by.values()))
    assert "<table>" in page and 'class="hl"' in page and "Содержание" in page
    assert f'href="#{by["docs/guide/02-b.md"].id}"' in page


def test_end_to_end_pdf(repo, tmp_path):
    pypdf = pytest.importorskip("pypdf")
    browser = b.find_browser()
    if not browser:
        pytest.skip("no Chrome/Edge/Chromium found")
    out = tmp_path / "out" / "t.pdf"
    rc = b.main(["--root", str(repo), "--output", str(out), "--browser", browser])
    assert rc == 0 and out.is_file()
    rd = pypdf.PdfReader(str(out))
    assert len(rd.pages) >= 4
    text = "\n".join(p.extract_text() for p in rd.pages)
    for word in ("Содержание", "Глава два", "Второй раздел", "int x = 1;"):
        assert word in text
    assert len(rd.outline) > 0
