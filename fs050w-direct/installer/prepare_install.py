#!/usr/bin/env python3
"""Build an owner-specific FS050W V2.1.0 install directory offline."""

import argparse
import hashlib
from pathlib import Path
from zipfile import ZipFile

from patch_home import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MODULE_SHA256 = 'fda2b5885897876bf47f76f58a10b2d33f1b72aa46a3a8f40855b46629a25558'
FIRMWARE_MANIFEST_SHA256 = 'e82e4eebc58cdfb6ccd14550187a6bd1da05afbe5fde882047b33c28ea7f2fc9'
SMS_PAGE_SHA256 = '1a3826091813484c48cb030ecd63c20d8ccc53e80c0bdad440e0753eb3a6bb66'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def packaged_asset(name, source_tree_path):
    """The public ZIP is flat; the source tree keeps build assets elsewhere."""
    public = HERE / name
    return public if public.is_file() else source_tree_path


def prepare(original_home, destination, sms_page=None, repair_kit=None):
    if destination.exists():
        raise ValueError('輸出資料夾已存在，請選擇新的位置。')
    page = (sms_page or packaged_asset('sms.html', ROOT / 'dist/sms.html')).read_bytes()
    if sha256(page) != SMS_PAGE_SHA256:
        raise ValueError('簡訊介面不是已驗證版本。')
    kit_path = repair_kit or packaged_asset('repair-kit.zip', ROOT.parent / 'fs050w-sms-ui/repair-kit.zip')
    home = patch(original_home.read_bytes(), page)
    with ZipFile(kit_path) as kit:
        if kit.testzip():
            raise ValueError('修復套件損壞。')
        binary = kit.read('repair-kit/bin/fs050w-sms')
        entries = dict(line.split('  ', 1) for line in kit.read('repair-kit/BIN.sha256').decode().splitlines())
        if sha256(binary) != MODULE_SHA256 or entries.get(MODULE_SHA256) != 'bin/fs050w-sms':
            raise ValueError('修復套件的模組雜湊不符。')
        firmware_manifest = kit.read('repair-kit/V2.1.0.sha256')
        if sha256(firmware_manifest) != FIRMWARE_MANIFEST_SHA256:
            raise ValueError('韌體雜湊清單不是已驗證版本。')
    destination.mkdir(mode=0o700, parents=True)
    files = {'home.html': home, 'sm3': binary,
             'V2.1.0.sha256': firmware_manifest,
             'install_on_router.sh': (HERE / 'install_on_router.sh').read_bytes(),
             'rollback_on_router.sh': (HERE / 'rollback_on_router.sh').read_bytes()}
    for name, data in files.items():
        (destination / name).write_bytes(data)
    (destination / 'sm3').chmod(0o755)
    for name in ('install_on_router.sh', 'rollback_on_router.sh'):
        (destination / name).chmod(0o755)
    (destination / 'PAYLOAD.sha256').write_text(
        '\n'.join(f'{sha256(files[name])}  {name}' for name in sorted(files)) + '\n', encoding='ascii')
    (destination / 'PAYLOAD.sha256').chmod(0o600)
    print(f'已建立 {destination}。請透過既有維護管道傳到設備。')
    print(f'首頁 SHA-256 {sha256(home)}；模組 SHA-256 {sha256(binary)}')


def main():
    parser = argparse.ArgumentParser(description='準備 FS050W V2.1.0 SMS 安裝資料夾')
    parser.add_argument('original_home', type=Path, help='從自己的 FS050W 取得的原廠 home.html')
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    prepare(args.original_home, args.output_directory)


if __name__ == '__main__':
    main()
