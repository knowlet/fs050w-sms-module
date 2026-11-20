"""Build a same-origin standalone document. No network or dependencies."""
from pathlib import Path


def build():
    root = Path(__file__).resolve().parent
    web = root / "web"
    scripts = [root / "auth-md5.js", root / "sms-data.js",
               web / "router.js", web / "api.js", web / "view.js"]
    code = "(() => {\n" + "\n".join(p.read_text("utf-8") for p in scripts) + "\n})();"
    if "</script" in code.lower():
        raise ValueError("Script contains an HTML delimiter.")
    page = (web / "page.html").read_text("utf-8")
    page = page.replace("/* INLINE_STYLE */", (web / "style.css").read_text("utf-8"))
    page = page.replace("/* INLINE_SCRIPT */", code)
    out = root / "dist" / "sms.html"
    out.parent.mkdir(exist_ok=True)
    out.write_text(page, "utf-8")
    return out


if __name__ == "__main__":
    print(build())
