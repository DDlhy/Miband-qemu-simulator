"""Export verified source and stock firmware, without build cache or runtime state."""
import argparse
import json
from pathlib import Path
import zipfile

from check_publication import ROOT, check, payload


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.is_relative_to(ROOT):
        raise ValueError('Write the source ZIP outside the project directory')
    if output.exists():
        raise FileExistsError(output)
    result = check()
    files = list(payload())
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, path in files:
            z.write(path, 'Miband-qemu-simulator/' + name)
    print(json.dumps(dict(result, output=str(output), zip_bytes=output.stat().st_size)))
