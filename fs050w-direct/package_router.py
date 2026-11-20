"""Package the router-hosted SMS UI and vetted repair kit without local secrets."""
import hashlib
import sys
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
FILES = [
    (ROOT / 'ROUTER.md', 'README.md'),
    (ROOT / 'dist/sms.html', 'sms.html'),
    (ROOT / 'installer/patch_home.py', 'patch_home.py'),
    (ROOT / 'installer/prepare_install.py', 'prepare_install.py'),
    (ROOT / 'installer/ntp_shell.py', 'ntp_shell.py'),
    (ROOT / 'installer/install_via_ntp.py', 'install_via_ntp.py'),
    (ROOT / 'installer/install_on_router.sh', 'install_on_router.sh'),
    (ROOT / 'installer/rollback_on_router.sh', 'rollback_on_router.sh'),
    (ROOT / 'build_web.py', 'src/build_web.py'),
    (ROOT / 'auth-md5.js', 'src/auth-md5.js'),
    (ROOT / 'sms-data.js', 'src/sms-data.js'),
    *[(p, 'src/web/' + p.name) for p in sorted((ROOT / 'web').iterdir()) if p.is_file()],
    (PROJECT / 'fs050w-sms-ui/repair-kit.zip', 'repair-kit.zip'),
    (PROJECT / 'fs050w-sms-ui/REPAIR.md', 'REPAIR.md'),
]

def build(out):
    def add(archive, name, data):
        # Fixed metadata keeps the published download reproducible.
        item = ZipInfo('FS050W-SMS/' + name, (2026, 11, 20, 0, 0, 0))
        item.create_system = 3
        item.external_attr = 0o100644 << 16
        archive.writestr(item, data, compress_type=ZIP_DEFLATED)

    with NamedTemporaryFile(dir=out.parent, delete=False) as temporary:
        path = Path(temporary.name)
    try:
        with ZipFile(path, 'w', ZIP_DEFLATED) as archive:
            sums = []
            for source, name in FILES:
                data = source.read_bytes()
                add(archive, name, data)
                sums.append(f'{hashlib.sha256(data).hexdigest()}  {name}')
            add(archive, 'SHA256SUMS', ('\n'.join(sums) + '\n').encode('ascii'))
        path.replace(out)
    finally:
        path.unlink(missing_ok=True)
    print(out, hashlib.sha256(out.read_bytes()).hexdigest())

if __name__ == '__main__':
    build(Path(sys.argv[1]).resolve() if len(sys.argv)>1 else PROJECT/'FS050W-SMS.zip')
