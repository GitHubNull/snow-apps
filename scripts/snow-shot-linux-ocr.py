#!/usr/bin/env python3
"""Stage and verify Linux OCR bundled runtimes and models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'snow_shot/packaging/snow-shot-ocr-asset-manifest.json'
RUNTIME_FILES = ('snow-ocr-process',)
ELF_MAGIC = bytes.fromhex('7f454c46')
ELF_CLASS_64 = 2
ELF_DATA_LSB = 1
ARCHITECTURES = {
    'x64': dict(machine=0x3e, worker='x86_64', label='x64'),
    'arm64': dict(machine=0xb7, worker='aarch64', label='ARM64'),
}


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT, timeout=60).strip()


def descriptor(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return dict(name=path.name, size=path.stat().st_size, sha256=digest.hexdigest())


def valid(path, expected):
    return (path.is_file() and not path.is_symlink()
            and path.stat().st_size == expected['size']
            and descriptor(path)['sha256'] == expected['sha256'])


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode()
    if path.exists() and path.read_bytes() == data:
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(data)
            output.close()
            temporary.chmod(0o644)
            temporary.replace(path)
        finally:
            output.close()
            temporary.unlink(missing_ok=True)


def fetch(expected, destination):
    if valid(destination, expected):
        return
    url = expected['url']
    if not url.startswith('https://'):
        raise ValueError('OCR models require HTTPS')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Unique partial files let independent build trees share the artifact cache.
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
        partial = Path(output.name)
        try:
            print(f'Downloading {expected["name"]}', flush=True)
            with urllib.request.urlopen(url, timeout=120) as response:
                if not response.url.startswith('https://'):
                    raise ValueError('OCR model redirect must use HTTPS')
                shutil.copyfileobj(response, output)
            output.close()
            if not valid(partial, expected):
                raise ValueError(f'OCR model integrity check failed: {expected["name"]}')
            partial.replace(destination)
        finally:
            output.close()
            partial.unlink(missing_ok=True)


def copy_changed(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() == destination.resolve():
        return
    if not valid(destination, descriptor(source)):
        shutil.copy2(source, destination)


def check_worker_header(path, architecture):
    """Reject wrong-architecture or wrong-endianness workers before launching."""
    with path.open('rb') as binary:
        header = binary.read(32)
    if (len(header) != 32 or header[:4] != ELF_MAGIC
            or header[4] != ELF_CLASS_64 or header[5] != ELF_DATA_LSB):
        raise ValueError(f'Expected a little-endian 64-bit ELF binary: {path}')
    machine = int.from_bytes(header[18:20], 'little')
    if machine != architecture['machine']:
        raise ValueError(f'Expected a thin {architecture["label"]} ELF binary: {path}')


def stage_models(manifest, cache, destination, all_models=False):
    for model in manifest['models']:
        if not all_models and model['type'] != 'small':
            continue
        for item in model['files']:
            cached = cache / ('ocr-models-' + model['id']) / item['name']
            fetch(item, cached)
            bundled = destination / model['id'] / item['name']
            copy_changed(cached, bundled)
            bundled.chmod(0o644)
        atomic_json(destination / model['id'] / '.complete.json',
                    dict(schema=1, component=model['id']))


def runtime_manifest(source, runtime, arch='x64'):
    architecture = ARCHITECTURES[arch]
    version = source['runtime']['version']
    for name in RUNTIME_FILES:
        path = runtime / name
        check_worker_header(path, architecture)
    if not os.access(runtime / RUNTIME_FILES[0], os.X_OK):
        raise ValueError('The OCR worker is not executable')
    expected = f'snow-ocr-process {version} linux-{architecture["worker"]} protocol 7'
    if run(str(runtime / RUNTIME_FILES[0]), '--version') != expected:
        raise ValueError('The OCR worker version/protocol does not match the application')
    return dict(schema=3, default_model='small', runtime=dict(
        version=version, platform=f'linux-{arch}', delivery='bundled', protocol=7,
        executable=RUNTIME_FILES[0],
        files=[descriptor(runtime / n) for n in RUNTIME_FILES]),
        models=source['models'])


def finalize(source, runtime, arch='x64'):
    manifest = runtime_manifest(source, runtime, arch)
    atomic_json(runtime / 'assets/ocr/asset-manifest.json', manifest)


def verify_assets(source, runtime, arch='x64'):
    actual = json.loads((runtime / 'assets/ocr/asset-manifest.json').read_text())
    if actual != runtime_manifest(source, runtime, arch):
        raise ValueError('Bundled OCR manifest does not match finalized runtime bytes')
    small = next(m for m in source['models'] if m['type'] == 'small')
    directory = runtime / 'assets/ocr/models' / small['id']
    for item in small['files']:
        if not valid(directory / item['name'], item):
            raise ValueError(f'Missing or corrupt bundled model: {item["name"]}')
    if json.loads((directory / '.complete.json').read_text()) != dict(schema=1, component=small['id']):
        raise ValueError('Invalid bundled model completion marker')
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('stage', 'finalize', 'verify', 'fetch-models'))
    parser.add_argument('--arch', choices=ARCHITECTURES, default='x64')
    parser.add_argument('--manifest', type=Path, default=MANIFEST)
    parser.add_argument('--runtime-dir', type=Path)
    parser.add_argument('--cache', type=Path, default=ROOT / 'artifacts')
    parser.add_argument('--worker', type=Path)
    parser.add_argument('--model-root', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.command == 'fetch-models':
        if args.model_root is None:
            parser.error('--model-root is required')
        source = json.loads(args.manifest.read_text())
        stage_models(source, args.cache, args.model_root, all_models=True)
        return
    if args.runtime_dir is None:
        parser.error('--runtime-dir is required')
    runtime = args.runtime_dir.resolve()
    if args.command == 'stage':
        if args.worker is None:
            parser.error('--worker is required')
        source = json.loads(args.manifest.read_text())
        copy_changed(args.worker, runtime / RUNTIME_FILES[0])
        stage_models(source, args.cache, runtime / 'assets/ocr/models')
        finalize(source, runtime, args.arch)
    elif args.command == 'finalize':
        finalize(json.loads(args.manifest.read_text()), runtime, args.arch)
    else:
        manifest = verify_assets(json.loads(args.manifest.read_text()), runtime, args.arch)
        if args.report:
            atomic_json(args.report, dict(platform=f'linux-{args.arch}', runtime=manifest['runtime'],
                        bundled_model=manifest['default_model']))
        print(f'Verified Linux {ARCHITECTURES[args.arch]["label"]} OCR assets')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f'Linux OCR packaging failed: {error}')
