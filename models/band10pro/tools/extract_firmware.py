"""Extract runtime images from a model-matching ZIP OTA without executing ota.sh."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
ROOT = Path(__file__).resolve().parents[1]
NAMES = ('vela_ap.bin', 'vela_resource.bin', 'vela_ota.bin', 'vela_bl2.bin')
def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('package', type=Path)
    a = p.parse_args()
    out = ROOT / 'firmware'
    out.mkdir(parents=True, exist_ok=True)
    report = []
    with zipfile.ZipFile(a.package) as z:
        for name in NAMES:
            matches = [i for i in z.infolist() if Path(i.filename).name == name and not i.is_dir()]
            if not matches:
                if name == 'vela_bl2.bin': continue
                raise ValueError('Missing runtime image: ' + name)
            if len(matches) != 1: raise ValueError('Ambiguous image: ' + name)
            raw = z.read(matches[0])
            target = out / name
            if target.exists() and target.read_bytes() != raw:
                raise FileExistsError('Preserve the current firmware before replacing: ' + name)
            target.write_bytes(raw)
            report.append(dict(file=target.relative_to(ROOT).as_posix(), size=len(raw),
                               sha256=hashlib.sha256(raw).hexdigest()))
    print(json.dumps(report, indent=2))
if __name__ == '__main__': main()
