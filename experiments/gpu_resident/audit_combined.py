"""Audit saved fixture artifacts without loading a model or starting GPU work."""
from array import array
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pixel_metrics(actual, reference, extent=1920 * 1080 * 4):
    if extent <= 0 or extent % 4 or len(actual) != extent or len(reference) != extent:
        raise ValueError('Pixel extent mismatch')
    maximum = 0
    for index, (a, b) in enumerate(zip(actual, reference)):
        maximum = max(maximum, abs(a - b))
        if index % 4 == 3 and (a != 255 or b != 255):
            raise ValueError('Nonopaque pixel')
    return maximum


def field_metrics(actual, reference, count=921600):
    if count <= 0 or len(actual) != count * 4 or len(reference) != count * 4:
        raise ValueError('Field extent mismatch')
    a, b = array('f'), array('f')
    if a.itemsize != 4:
        raise ValueError('32-bit float required')
    a.frombytes(actual)
    b.frombytes(reference)
    if sys.byteorder != 'little':
        a.byteswap()
        b.byteswap()
    maximum = 0.0
    for x, y in zip(a, b):
        if not math.isfinite(x) or not math.isfinite(y):
            raise ValueError('Nonfinite field')
        maximum = max(maximum, abs(x - y) / (1 + abs(y)))
    if maximum > 1e-4:
        raise ValueError('Field parity exceeded')
    return maximum


def audit_artifacts(collection):
    """Numerical audit only; not proof of execution provenance or preprocessing."""
    collection = Path(collection)
    expected, rows, hashes = set(), [], {}
    for case in range(3):
        def read(suffix):
            name = f'case_{case}_{suffix}'
            expected.add(name)
            data = (collection / name).read_bytes()
            hashes[name] = hashlib.sha256(data).hexdigest()
            return data

        scaled = field_metrics(read('actual.f32'), read('reference.f32'))
        candidate, draw = read('candidate.bgra'), read('draw.bgra')
        matched = pixel_metrics(candidate, read('matched_reference.bgra'))
        combined = pixel_metrics(candidate, read('end_to_end_reference.bgra'))
        if draw != candidate or matched > 1 or combined > 1:
            raise ValueError('Pixel parity exceeded')
        rows.append(dict(case=case, field_max_scaled=scaled, matched_max=matched,
                         combined_max=combined, draw_exact=True))
    actual = {p.name for p in collection.iterdir() if p.suffix in ('.bgra', '.f32')}
    if actual != expected:
        raise ValueError('Unexpected or missing artifacts')
    for name, digest in hashes.items():
        if sha(collection / name) != digest:
            raise ValueError('Artifact changed during audit')
    return dict(complete=True, native_reruns=0, sealed=False,
                provenance_verified=False, cases=rows, artifact_hashes=hashes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('collection', type=Path)
    parser.add_argument('output', type=Path, help='New JSON file; never overwritten')
    args = parser.parse_args()
    with args.output.open('x', encoding='utf-8') as stream:
        try:
            result = audit_artifacts(args.collection)
        except Exception as error:
            result = dict(complete=False, native_reruns=0, error=str(error))
        json.dump(result, stream, indent=2)
    return 0 if result['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())