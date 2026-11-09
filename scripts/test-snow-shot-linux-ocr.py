#!/usr/bin/env python3
"""Deterministic tests for the Linux OCR staging and integrity contract."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ocr', Path(__file__).with_name('snow-shot-linux-ocr.py'))
ocr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ocr)

X64_ELF_HEADER = (ocr.ELF_MAGIC + bytes([ocr.ELF_CLASS_64, ocr.ELF_DATA_LSB])
                  + bytes(12) + (0x3e).to_bytes(2, 'little') + bytes(12))
ARM64_ELF_HEADER = (ocr.ELF_MAGIC + bytes([ocr.ELF_CLASS_64, ocr.ELF_DATA_LSB])
                    + bytes(12) + (0xb7).to_bytes(2, 'little') + bytes(12))


class LinuxOcrAssets(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='snow OCR 空间 ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        for name in ocr.RUNTIME_FILES:
            path = self.runtime / name
            path.write_bytes(X64_ELF_HEADER + name.encode())
            path.chmod(0o755)
        self.model = self.runtime / 'assets/ocr/models/small-id'
        self.model.mkdir(parents=True)
        files = []
        for name in ('det.onnx', 'rec.onnx', 'dict.txt'):
            path = self.model / name
            path.write_bytes(name.encode())
            files.append(dict(ocr.descriptor(path), url=f'https://example.invalid/{name}'))
        self.source = dict(runtime=dict(version='1.2.0'), models=[dict(type='small', id='small-id', files=files)])
        ocr.atomic_json(self.model / '.complete.json', dict(schema=1, component='small-id'))
        self.run = patch.object(ocr, 'run', return_value='snow-ocr-process 1.2.0 linux-x86_64 protocol 7').start()
        self.addCleanup(patch.stopall)

    def command(self, command, *arguments):
        manifest = self.root / 'source-manifest.json'
        ocr.atomic_json(manifest, self.source)
        argv = ['snow-shot-linux-ocr.py', command, '--manifest', str(manifest),
                '--runtime-dir', str(self.runtime), *map(str, arguments)]
        with patch.object(ocr.sys, 'argv', argv):
            ocr.main()

    def test_manifest_pins_final_bytes_and_preserves_models(self):
        ocr.finalize(self.source, self.runtime)
        result = ocr.verify_assets(self.source, self.runtime)
        self.assertEqual(result['schema'], 3)
        self.assertEqual(result['runtime']['platform'], 'linux-x64')
        self.assertEqual(result['runtime']['delivery'], 'bundled')
        self.assertEqual(result['runtime']['protocol'], 7)
        self.assertNotIn('archive', result['runtime'])
        self.assertEqual([item['name'] for item in result['runtime']['files']],
                         ['snow-ocr-process'])
        self.assertEqual(result['models'], self.source['models'])
        with (self.runtime / 'snow-ocr-process').open('ab') as binary:
            binary.write(b'new signature')
        with self.assertRaisesRegex(ValueError, 'finalized runtime bytes'):
            ocr.verify_assets(self.source, self.runtime)
        ocr.finalize(self.source, self.runtime)
        ocr.verify_assets(self.source, self.runtime)

    def test_packaging_rejects_same_size_runtime_and_model_corruption(self):
        ocr.finalize(self.source, self.runtime)
        for path in (self.runtime / 'snow-ocr-process', self.model / 'det.onnx',
                     self.model / 'rec.onnx', self.model / 'dict.txt'):
            with self.subTest(file=path.name):
                original = path.read_bytes()
                path.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
                with self.assertRaisesRegex(
                        ValueError, 'finalized runtime bytes|Missing or corrupt bundled model'):
                    ocr.verify_assets(self.source, self.runtime)
                path.write_bytes(original)
        ocr.verify_assets(self.source, self.runtime)

    def test_stage_invocation_verifies_and_reports(self):
        worker = self.root / 'cargo-worker/snow-ocr-process'
        worker.parent.mkdir()
        worker.write_bytes(X64_ELF_HEADER + b'upstream bytes')
        worker.chmod(0o755)
        cached = self.root / 'cache/ocr-models-small-id'
        for item in self.source['models'][0]['files']:
            ocr.copy_changed(self.model / item['name'], cached / item['name'])
        with patch.object(ocr.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            self.command('stage', '--worker', worker, '--cache', cached.parent)
        self.assertEqual((self.runtime / 'snow-ocr-process').read_bytes(), worker.read_bytes())
        ocr.verify_assets(self.source, self.runtime)
        report = self.root / 'report.json'
        self.command('verify', '--report', report)
        payload = json.loads(report.read_text())
        self.assertEqual(payload['platform'], 'linux-x64')
        self.assertEqual(payload['bundled_model'], 'small')

    def test_wrong_architecture_headers_are_rejected(self):
        ocr.check_worker_header(self.runtime / 'snow-ocr-process', ocr.ARCHITECTURES['x64'])
        with self.assertRaisesRegex(ValueError, 'thin ARM64'):
            ocr.check_worker_header(
                self.runtime / 'snow-ocr-process', ocr.ARCHITECTURES['arm64'])
        (self.runtime / 'snow-ocr-process').write_bytes(ARM64_ELF_HEADER)
        with self.assertRaisesRegex(ValueError, 'thin x64'):
            ocr.finalize(self.source, self.runtime)
        (self.runtime / 'snow-ocr-process').write_bytes(b'MZ fake windows binary')
        with self.assertRaisesRegex(ValueError, 'ELF binary'):
            ocr.finalize(self.source, self.runtime)
        (self.runtime / 'snow-ocr-process').write_bytes(X64_ELF_HEADER)
        (self.runtime / 'snow-ocr-process').chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'not executable'):
            ocr.finalize(self.source, self.runtime)
        (self.runtime / 'snow-ocr-process').chmod(0o755)

    def test_worker_identity_must_match_the_application(self):
        self.run.return_value = 'snow-ocr-process 1.2.0 linux-x86_64 protocol 6'
        with self.assertRaisesRegex(ValueError, 'version/protocol'):
            ocr.finalize(self.source, self.runtime)
        self.run.return_value = 'snow-ocr-process 1.2.0 windows-x86_64 protocol 7'
        with self.assertRaisesRegex(ValueError, 'version/protocol'):
            ocr.finalize(self.source, self.runtime)

    def test_cached_models_work_without_network(self):
        cached = self.root / 'cache/ocr-models-small-id'
        cached.mkdir(parents=True)
        for item in self.source['models'][0]['files']:
            ocr.copy_changed(self.model / item['name'], cached / item['name'])
        with patch.object(ocr.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            ocr.stage_models(self.source, self.root / 'cache', self.root / 'staged')
        for item in self.source['models'][0]['files']:
            self.assertTrue(ocr.valid(self.root / 'staged/small-id' / item['name'], item))

    def test_failed_or_corrupt_download_never_promotes(self):
        item = self.source['models'][0]['files'][0]
        destination = self.root / 'downloads/model'
        with patch.object(ocr.urllib.request, 'urlopen', side_effect=OSError('interrupted')):
            with self.assertRaises(OSError):
                ocr.fetch(item, destination)
        self.assertFalse(destination.exists())
        self.assertEqual(list(destination.parent.iterdir()), [])
        class Response:
            url = item['url']
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def read(self, size):
                if getattr(self, 'sent', False):
                    return b''
                self.sent = True
                return b'wrong bytes'
        with patch.object(ocr.urllib.request, 'urlopen', return_value=Response()):
            with self.assertRaisesRegex(ValueError, 'integrity'):
                ocr.fetch(item, destination)
        self.assertEqual(list(destination.parent.iterdir()), [])

    def test_http_and_symlink_assets_are_rejected(self):
        item = dict(self.source['models'][0]['files'][0], url='http://example.invalid/model')
        with self.assertRaisesRegex(ValueError, 'HTTPS'):
            ocr.fetch(item, self.root / 'model')
        link = self.root / 'link'
        link.symlink_to(self.model / item['name'])
        self.assertFalse(ocr.valid(link, item))

    def test_fetch_models_still_requests_all_downloadable_model_variants(self):
        with patch.object(ocr, 'stage_models') as stage:
            self.command('fetch-models', '--model-root', self.root / 'fetched')
        stage.assert_called_once_with(self.source, ocr.ROOT / 'artifacts',
                                      self.root / 'fetched', all_models=True)

    def test_missing_model_or_marker_prevents_release(self):
        ocr.finalize(self.source, self.runtime)
        (self.model / 'det.onnx').write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'corrupt bundled model'):
            ocr.verify_assets(self.source, self.runtime)
        (self.model / 'det.onnx').write_bytes(b'det.onnx')
        ocr.atomic_json(self.model / '.complete.json', dict(schema=1, component='wrong'))
        with self.assertRaisesRegex(ValueError, 'completion marker'):
            ocr.verify_assets(self.source, self.runtime)

    def test_unchanged_staging_does_not_rewrite_files(self):
        path = self.root / 'manifest.json'
        ocr.atomic_json(path, self.source)
        before = path.stat().st_mtime_ns
        ocr.atomic_json(path, self.source)
        self.assertEqual(before, path.stat().st_mtime_ns)


if __name__ == '__main__':
    unittest.main()
