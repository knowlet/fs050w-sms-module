#!/usr/bin/env python3
"""Embed SMS in the existing V2.1.0 menu and content area."""

import argparse
import base64
import hashlib
from pathlib import Path

ORIGINAL_SHA256 = "825029c449238980927c213f86e62dddf3be6d0a4e3802a38e54b034cb7d4815"
MARKER = "fs050w-sms-home-v1"


def patch(original: bytes, sms_page: bytes) -> bytes:
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("原廠首頁雜湊不符，停止修改")
    if MARKER.encode() in original:
        raise ValueError("首頁已含簡訊補丁")
    html = original.decode("utf-8")
    if (html.count('<li id="information_menu_left"') != 1
            or html.count('<div class="content" id="content_noRefresh"></div>') != 1
            or html.count("</body>") != 1):
        raise ValueError("原廠首頁結構與預期不符")
    entry = f'''<li id="fs050w-sms-menu" class="li-item" data-patch="{MARKER}">
        <a href="../common/home.html#fs050w-sms">簡訊中心</a>
    </li>
    '''
    html = html.replace('<li id="information_menu_left"', entry + '<li id="information_menu_left"')
    # The OEM page already supplies the header and breadcrumb around the frame.
    embedded = sms_page.decode("utf-8")
    if embedded.count("</head>") != 1:
        raise ValueError("簡訊頁面結構與預期不符")
    embedded = embedded.replace("</head>", """<style data-embed="fs050w-sms">
      header{display:none!important}
      main{max-width:none!important;padding:16px 0 28px!important}
    </style></head>""")
    encoded = base64.b64encode(embedded.encode("utf-8")).decode("ascii")
    addition = f'''<style data-patch="{MARKER}">
    #fs050w-sms-host[hidden],#content_noRefresh[hidden]{{display:none!important}}
    #fs050w-sms-host{{width:100%;min-width:0}}
    #fs050w-sms-frame{{display:block;width:100%;height:calc(100vh - 190px);min-height:500px;border:0;background:#fff}}
    @media(max-width:867px){{#fs050w-sms-frame{{height:calc(100dvh - 135px);min-height:400px}}}}
    </style>
    <div id="fs050w-sms-host" hidden><iframe id="fs050w-sms-frame" title="FS050W 簡訊中心"></iframe></div>
    <script id="fs050w-sms-document" type="application/octet-stream">{encoded}</script>
    <script data-patch="{MARKER}">(() => {{
      const menu = document.querySelector('#fs050w-sms-menu a');
      const host = document.getElementById('fs050w-sms-host');
      const content = document.getElementById('content_noRefresh');
      const frame = document.getElementById('fs050w-sms-frame');
      const title = document.querySelector('.content_header .first_title');
      const subtitle = document.querySelector('.content_header .second_title');
      const icon = document.getElementById('content_right_img');
      function close() {{
        if (host.hidden) return;
        host.hidden = true;
        content.hidden = false;
        frame.removeAttribute('srcdoc');
        if (typeof window.initPageResult === 'function' && location.hash !== '#login')
          window.initPageResult(location.hash, location.hash.slice(1));
      }}
      function open(event) {{
        event.preventDefault();
        event.stopPropagation();
        if (!host.hidden) return;
        const bytes = Uint8Array.from(atob(document.getElementById('fs050w-sms-document').textContent), c => c.charCodeAt(0));
        frame.srcdoc = new TextDecoder().decode(bytes);
        content.hidden = true;
        content.insertAdjacentElement('afterend', host);
        host.hidden = false;
        document.querySelectorAll('#menu-left-settings .li-item.click').forEach(item => item.classList.remove('click'));
        document.querySelectorAll('#menu-left-settings .ul-normal.click').forEach(item => item.classList.remove('click'));
        document.querySelectorAll('#menu-left-settings .menu-ul > li > ul').forEach(item => item.style.display = 'none');
        document.getElementById('fs050w-sms-menu').classList.add('click');
        const parent = menu.closest('ul');
        parent.classList.remove('hide');
        parent.style.display = 'block';
        parent.previousElementSibling.classList.add('click');
        title.textContent = document.getElementById('label-home').textContent;
        subtitle.textContent = '>簡訊中心';
        icon.className = 'active_home';
        if (typeof window.phone_cancel_menu === 'function') window.phone_cancel_menu();
      }}
      menu.addEventListener('click', open, true);
      document.getElementById('menu-left-settings').addEventListener('click', event => {{
        if (event.target.closest('a') && !menu.contains(event.target)) close();
      }}, true);
      window.addEventListener('hashchange', close);
    }})();</script>
    '''
    html = html.replace("</body>", addition + "</body>")
    return html.encode("utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("original", type=Path)
    parser.add_argument("sms_page", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = patch(args.original.read_bytes(), args.sms_page.read_bytes())
    args.output.write_bytes(result)
    print(f"{len(result)} bytes, SHA-256 {hashlib.sha256(result).hexdigest()}")


if __name__ == "__main__":
    main()
