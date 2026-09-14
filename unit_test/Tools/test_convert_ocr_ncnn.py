from __future__ import annotations

import importlib.metadata
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT_PATH = (
    Path(__file__).resolve().parents[2]
    / "tools"
    / "ConvertOcrNcnn"
    / "convert_ocr_ncnn.py"
)
VERIFY_SCRIPT_PATH = SCRIPT_PATH.with_name("verify_manifest.py")


def load_converter(test_case: unittest.TestCase):
    test_case.assertTrue(SCRIPT_PATH.is_file(), f"converter not found: {SCRIPT_PATH}")
    spec = importlib.util.spec_from_file_location("convert_ocr_ncnn", SCRIPT_PATH)
    test_case.assertIsNotNone(spec)
    test_case.assertIsNotNone(spec.loader)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_verifier(test_case: unittest.TestCase):
    test_case.assertTrue(
        VERIFY_SCRIPT_PATH.is_file(),
        f"manifest verifier not found: {VERIFY_SCRIPT_PATH}",
    )
    spec = importlib.util.spec_from_file_location(
        "verify_ocr_ncnn_manifest", VERIFY_SCRIPT_PATH
    )
    test_case.assertIsNotNone(spec)
    test_case.assertIsNotNone(spec.loader)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DiscoverModelsTest(unittest.TestCase):
    def test_discovers_only_ocr_inference_models(self) -> None:
        converter = load_converter(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            resource = Path(temp_dir)
            expected = {
                resource / "PaddleOCR" / "det" / "inference.onnx": "det",
                resource / "PaddleOCR" / "rec" / "inference.onnx": "rec",
                resource
                / "global"
                / "YoStarEN"
                / "resource"
                / "PaddleOCR"
                / "rec"
                / "inference.onnx": "rec",
            }
            ignored = [
                resource / "onnx" / "operators_det.onnx",
                resource / "other" / "inference.onnx",
                resource / "PaddleOCR" / "det" / "other.onnx",
            ]
            for model in [*expected, *ignored]:
                model.parent.mkdir(parents=True, exist_ok=True)
                model.write_bytes(b"onnx")

            discovered = converter.discover_models(resource)

            self.assertEqual(
                [(model.path, model.kind) for model in discovered],
                list(expected.items()),
            )


class CacheKeyTest(unittest.TestCase):
    def test_includes_every_conversion_input(self) -> None:
        converter = load_converter(self)
        self.assertTrue(hasattr(converter, "cache_key"), "cache_key is not implemented")
        with tempfile.TemporaryDirectory() as temp_dir:
            onnx_path = Path(temp_dir) / "inference.onnx"
            onnx_path.write_bytes(b"model-a")

            base = converter.cache_key(onnx_path, "rec", "fp32", "pnnx-a", "recipe-a")

            self.assertNotEqual(
                base,
                converter.cache_key(onnx_path, "det", "fp32", "pnnx-a", "recipe-a"),
            )
            self.assertNotEqual(
                base,
                converter.cache_key(onnx_path, "rec", "fp16", "pnnx-a", "recipe-a"),
            )
            self.assertNotEqual(
                base,
                converter.cache_key(onnx_path, "rec", "fp32", "pnnx-b", "recipe-a"),
            )
            self.assertNotEqual(
                base,
                converter.cache_key(onnx_path, "rec", "fp32", "pnnx-a", "recipe-b"),
            )

            onnx_path.write_bytes(b"model-b")
            self.assertNotEqual(
                base,
                converter.cache_key(onnx_path, "rec", "fp32", "pnnx-a", "recipe-a"),
            )


class PnnxDiscoveryTest(unittest.TestCase):
    def test_prefers_wheel_binary_that_does_not_import_torch(self) -> None:
        converter = load_converter(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            binary = Path(temp_dir) / "pnnx" / "pnnx"
            binary.parent.mkdir()
            binary.write_bytes(b"binary")

            class Distribution:
                version = "20260526"

                @staticmethod
                def locate_file(relative: str) -> Path:
                    self.assertEqual(relative, "pnnx/pnnx")
                    return binary

            with (
                mock.patch.object(
                    importlib.metadata,
                    "distribution",
                    return_value=Distribution(),
                ),
                mock.patch.object(
                    converter.shutil, "which", return_value="/venv/bin/pnnx"
                ),
            ):
                self.assertEqual(converter._find_pnnx(), str(binary))

    def test_rejects_unversioned_path_binary(self) -> None:
        converter = load_converter(self)
        with (
            mock.patch.object(
                importlib.metadata,
                "distribution",
                side_effect=importlib.metadata.PackageNotFoundError,
            ),
            mock.patch.object(converter.shutil, "which", return_value="/usr/bin/pnnx"),
            self.assertRaisesRegex(SystemExit, "pinned pnnx package not found"),
        ):
            converter._find_pnnx()


class ConvertTreeTest(unittest.TestCase):
    def test_rejects_repository_source_resource_tree(self) -> None:
        converter = load_converter(self)
        source_resource = SCRIPT_PATH.parents[2] / "resource"

        unsafe_paths = (
            source_resource,
            source_resource / "PaddleOCR",
            source_resource.parent,
        )
        for unsafe_path in unsafe_paths:
            with (
                self.subTest(path=unsafe_path),
                self.assertRaisesRegex(SystemExit, "source resource tree"),
            ):
                converter._ensure_staged_resource(unsafe_path)

        converter._ensure_staged_resource(
            source_resource.parent / "install" / "resource"
        )

    def test_preserves_onnx_by_default(self) -> None:
        converter = load_converter(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            model = resource / "PaddleOCR" / "rec" / "inference.onnx"
            model.parent.mkdir(parents=True)
            model.write_bytes(b"rec")

            def fake_run(command: list[str], cwd: Path) -> None:
                (cwd / "rec.ncnn.param").write_text("param", encoding="utf-8")
                (cwd / "rec.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                stats = converter.convert_tree(resource, cache)

            self.assertTrue(model.is_file())
            self.assertEqual(stats["onnx_removed"], 0)

    def test_converts_ocr_models_and_preserves_auxiliary_onnx(self) -> None:
        converter = load_converter(self)
        self.assertTrue(
            hasattr(converter, "convert_tree"), "convert_tree is not implemented"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            det = resource / "PaddleOCR" / "det" / "inference.onnx"
            rec = resource / "PaddleOCR" / "rec" / "inference.onnx"
            auxiliary = resource / "onnx" / "operators_det.onnx"
            for path, content in ((det, b"det"), (rec, b"rec"), (auxiliary, b"aux")):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)

            commands: list[list[str]] = []

            def fake_run(command: list[str], cwd: Path) -> None:
                commands.append(command)
                kind = Path(command[1]).stem
                (cwd / f"{kind}.ncnn.param").write_text("param", encoding="utf-8")
                (cwd / f"{kind}.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                stats = converter.convert_tree(resource, cache, keep_onnx=False)

            self.assertEqual(stats, {"converted": 2, "cached": 0, "onnx_removed": 2})
            self.assertFalse(det.exists())
            self.assertFalse(rec.exists())
            self.assertTrue(auxiliary.exists())
            self.assertTrue((det.parent / "det.ncnn.param").is_file())
            self.assertTrue((det.parent / "det.ncnn.bin").is_file())
            self.assertTrue((rec.parent / "rec.ncnn.param").is_file())
            self.assertTrue((rec.parent / "rec.ncnn.bin").is_file())
            self.assertIn("inputshape=[1,3,640,640]", commands[0])
            self.assertIn("inputshape=[1,3,48,320]", commands[1])
            self.assertTrue(all("fp16=0" in command for command in commands))

            manifest = json.loads(
                (resource / "ocr-ncnn-manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["pnnx_version"], "20260526")
            self.assertEqual(len(manifest["models"]), 2)
            self.assertEqual(
                {model["source"] for model in manifest["models"]},
                {"PaddleOCR/det/inference.onnx", "PaddleOCR/rec/inference.onnx"},
            )

    def test_convert_removes_onnx_only_after_successful_verification(self) -> None:
        converter = load_converter(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            model = resource / "PaddleOCR" / "rec" / "inference.onnx"
            model.parent.mkdir(parents=True)
            model.write_bytes(b"rec")

            def fake_run(command: list[str], cwd: Path) -> None:
                # 空 param：产物校验会失败（empty NCNN artifact），源 ONNX 必须保留
                (cwd / "rec.ncnn.param").write_text("", encoding="utf-8")
                (cwd / "rec.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                with self.assertRaisesRegex(SystemExit, "verification failed"):
                    converter.convert_tree(resource, cache, keep_onnx=False)

            self.assertTrue(
                model.is_file(),
                "source ONNX must survive a failed pre-removal verification",
            )

    def test_reuses_verified_cache_and_rebuilds_incomplete_artifacts(self) -> None:
        converter = load_converter(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            model = resource / "PaddleOCR" / "rec" / "inference.onnx"
            model.parent.mkdir(parents=True)
            model.write_bytes(b"rec")
            run_count = 0

            def fake_run(command: list[str], cwd: Path) -> None:
                nonlocal run_count
                run_count += 1
                (cwd / "rec.ncnn.param").write_text("param", encoding="utf-8")
                (cwd / "rec.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                first = converter.convert_tree(resource, cache, keep_onnx=True)
                second = converter.convert_tree(resource, cache, keep_onnx=True)
                cached_bin = next(cache.glob("*/rec.ncnn.bin"))
                (cached_bin.parent / converter.CACHE_MANIFEST).unlink()
                third = converter.convert_tree(resource, cache, keep_onnx=True)

            self.assertEqual(first["converted"], 1)
            self.assertEqual(second["cached"], 1)
            self.assertEqual(third["converted"], 1)
            self.assertEqual(run_count, 2)

    def test_rebuilds_cache_when_artifact_hash_does_not_match(self) -> None:
        converter = load_converter(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            model = resource / "PaddleOCR" / "rec" / "inference.onnx"
            model.parent.mkdir(parents=True)
            model.write_bytes(b"rec")
            run_count = 0

            def fake_run(command: list[str], cwd: Path) -> None:
                nonlocal run_count
                run_count += 1
                (cwd / "rec.ncnn.param").write_text("param", encoding="utf-8")
                (cwd / "rec.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                converter.convert_tree(resource, cache, keep_onnx=True)
                next(cache.glob("*/rec.ncnn.bin")).write_bytes(b"corrupt")
                rebuilt = converter.convert_tree(resource, cache, keep_onnx=True)

            self.assertEqual(rebuilt["converted"], 1)
            self.assertEqual(run_count, 2)


class VerifyManifestTest(unittest.TestCase):
    def test_rejects_multiple_sources_mapped_to_one_artifact_pair(self) -> None:
        verifier = load_verifier(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            resource = Path(temp_dir)
            shared_param = resource / "PaddleOCR" / "rec" / "rec.ncnn.param"
            shared_bin = resource / "PaddleOCR" / "rec" / "rec.ncnn.bin"
            shared_param.parent.mkdir(parents=True)
            shared_param.write_text("param", encoding="utf-8")
            shared_bin.write_bytes(b"bin")
            sources = {
                "PaddleOCR/rec/inference.onnx",
                "PaddleOCR/rec/../rec/inference.onnx",
            }
            models = []
            for source in sorted(sources):
                kind = Path(source).parent.name
                source_sha256 = "0" * 64
                identity = (
                    f"{source_sha256}\0{kind}\0fp32\0"
                    f"{verifier.EXPECTED_PNNX_VERSION}\0"
                    f"{verifier.EXPECTED_RECIPE_VERSION}"
                )
                source_parent = Path(source).parent
                models.append(
                    {
                        "source": source,
                        "source_sha256": source_sha256,
                        "kind": kind,
                        "precision": "fp32",
                        "input_shape": "[1,3,48,320]",
                        "cache_key": verifier.hashlib.sha256(
                            identity.encode("ascii")
                        ).hexdigest(),
                        "param": (source_parent / "rec.ncnn.param").as_posix(),
                        "param_sha256": verifier._sha256(shared_param),
                        "bin": (source_parent / "rec.ncnn.bin").as_posix(),
                        "bin_sha256": verifier._sha256(shared_bin),
                    }
                )
            manifest = {
                "schema_version": 1,
                "pnnx_version": verifier.EXPECTED_PNNX_VERSION,
                "recipe_version": verifier.EXPECTED_RECIPE_VERSION,
                "models": models,
            }
            (resource / verifier.MANIFEST_NAME).write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            for name in verifier.REQUIRED_AUXILIARY_MODELS:
                auxiliary = resource / name
                auxiliary.parent.mkdir(parents=True, exist_ok=True)
                auxiliary.write_bytes(b"aux")

            with self.assertRaisesRegex(verifier.VerificationError, "duplicate param"):
                verifier.verify_resource(resource, expected_sources=sources)

    def test_rejects_non_object_manifest_root(self) -> None:
        verifier = load_verifier(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            resource = Path(temp_dir)
            (resource / verifier.MANIFEST_NAME).write_text("[]", encoding="utf-8")

            with self.assertRaisesRegex(
                verifier.VerificationError, "manifest root must be an object"
            ):
                verifier.verify_resource(resource)

    def test_validates_hashes_removal_and_auxiliary_models(self) -> None:
        converter = load_converter(self)
        verifier = load_verifier(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            model = resource / "PaddleOCR" / "rec" / "inference.onnx"
            model.parent.mkdir(parents=True)
            model.write_bytes(b"rec")
            for name in verifier.REQUIRED_AUXILIARY_MODELS:
                auxiliary = resource / name
                auxiliary.parent.mkdir(parents=True, exist_ok=True)
                auxiliary.write_bytes(b"aux")

            def fake_run(command: list[str], cwd: Path) -> None:
                (cwd / "rec.ncnn.param").write_text("param", encoding="utf-8")
                (cwd / "rec.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                converter.convert_tree(resource, cache, keep_onnx=False)

            expected_sources = {"PaddleOCR/rec/inference.onnx"}
            verifier.verify_resource(resource, expected_sources=expected_sources)

            (resource / "PaddleOCR" / "rec" / "rec.ncnn.bin").write_bytes(b"corrupt")
            with self.assertRaisesRegex(verifier.VerificationError, "hash mismatch"):
                verifier.verify_resource(resource, expected_sources=expected_sources)

            (resource / "PaddleOCR" / "rec" / "rec.ncnn.bin").write_bytes(b"bin")
            manifest_path = resource / verifier.MANIFEST_NAME
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["models"][0]["cache_key"] = "0" * 64
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                verifier.VerificationError, "cache_key mismatch"
            ):
                verifier.verify_resource(resource, expected_sources=expected_sources)

    def test_verify_allows_onnx_present_when_removal_not_required(self) -> None:
        converter = load_converter(self)
        verifier = load_verifier(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            resource = root / "resource"
            cache = root / "cache"
            model = resource / "PaddleOCR" / "rec" / "inference.onnx"
            model.parent.mkdir(parents=True)
            model.write_bytes(b"rec")
            for name in verifier.REQUIRED_AUXILIARY_MODELS:
                auxiliary = resource / name
                auxiliary.parent.mkdir(parents=True, exist_ok=True)
                auxiliary.write_bytes(b"aux")

            def fake_run(command: list[str], cwd: Path) -> None:
                (cwd / "rec.ncnn.param").write_text("param", encoding="utf-8")
                (cwd / "rec.ncnn.bin").write_bytes(b"bin")

            with (
                mock.patch.object(converter, "_find_pnnx", return_value="pnnx"),
                mock.patch.object(converter, "_run", side_effect=fake_run),
            ):
                converter.convert_tree(resource, cache, keep_onnx=True)

            expected_sources = {"PaddleOCR/rec/inference.onnx"}
            verifier.verify_resource(
                resource,
                expected_sources=expected_sources,
                require_onnx_removed=False,
            )
            with self.assertRaisesRegex(verifier.VerificationError, "was not removed"):
                verifier.verify_resource(resource, expected_sources=expected_sources)

    def test_rejects_malformed_model_entries(self) -> None:
        verifier = load_verifier(self)
        with tempfile.TemporaryDirectory() as temp_dir:
            resource = Path(temp_dir)
            manifest = {
                "schema_version": 1,
                "pnnx_version": verifier.EXPECTED_PNNX_VERSION,
                "recipe_version": verifier.EXPECTED_RECIPE_VERSION,
                "models": [{"source": "PaddleOCR/rec/inference.onnx"}],
            }
            (resource / verifier.MANIFEST_NAME).write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            with self.assertRaisesRegex(verifier.VerificationError, "invalid param"):
                verifier.verify_resource(
                    resource,
                    expected_sources={"PaddleOCR/rec/inference.onnx"},
                )


if __name__ == "__main__":
    unittest.main()
