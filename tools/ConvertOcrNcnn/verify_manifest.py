#!/usr/bin/env python3
"""Validate staged NCNN OCR resources before packaging."""

from __future__ import annotations

import argparse
import hashlib
import json
import string
from collections.abc import Collection
from pathlib import Path, PurePosixPath
from typing import Any

MANIFEST_NAME = "ocr-ncnn-manifest.json"
EXPECTED_PNNX_VERSION = "20260526"
EXPECTED_RECIPE_VERSION = "1"
REC_INPUTSHAPE = "[1,3,48,320]"
DET_INPUTSHAPE = "[1,3,640,640]"
EXPECTED_MODEL_SOURCES = {
    "PaddleOCR/det/inference.onnx",
    "PaddleOCR/rec/inference.onnx",
    "PaddleCharOCR/det/inference.onnx",
    "PaddleCharOCR/rec/inference.onnx",
    "global/YoStarEN/resource/PaddleOCR/rec/inference.onnx",
    "global/YoStarJP/resource/PaddleOCR/rec/inference.onnx",
    "global/YoStarKR/resource/PaddleOCR/rec/inference.onnx",
    "global/txwy/resource/PaddleOCR/rec/inference.onnx",
}
REQUIRED_AUXILIARY_MODELS = {
    "onnx/operators_det.onnx",
    "onnx/skill_ready_cls.onnx",
    "onnx/deploy_direction_cls.onnx",
}


class VerificationError(RuntimeError):
    pass


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _resource_path(resource_dir: Path, relative: Any) -> Path:
    if not isinstance(relative, str) or not relative:
        raise VerificationError(f"invalid manifest path: {relative!r}")
    path = (resource_dir / relative).resolve()
    try:
        path.relative_to(resource_dir)
    except ValueError as error:
        raise VerificationError(
            f"manifest path escapes resource tree: {relative}"
        ) from error
    return path


def _is_sha256(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(char in string.hexdigits for char in value)
    )


def verify_resource(
    resource_dir: Path,
    expected_sources: Collection[str] = EXPECTED_MODEL_SOURCES,
    *,
    require_onnx_removed: bool = True,
    required_auxiliary_models: Collection[str] = REQUIRED_AUXILIARY_MODELS,
) -> dict[str, Any]:
    resource_dir = resource_dir.resolve()
    manifest_path = resource_dir / MANIFEST_NAME
    if not manifest_path.is_file():
        raise VerificationError(f"manifest not found: {manifest_path}")

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"invalid manifest: {error}") from error

    if not isinstance(manifest, dict):
        raise VerificationError("manifest root must be an object")
    if manifest.get("schema_version") != 1:
        raise VerificationError("unsupported manifest schema_version")
    if manifest.get("pnnx_version") != EXPECTED_PNNX_VERSION:
        raise VerificationError("unexpected pnnx_version")
    if manifest.get("recipe_version") != EXPECTED_RECIPE_VERSION:
        raise VerificationError("unexpected recipe_version")

    models = manifest.get("models")
    if not isinstance(models, list):
        raise VerificationError("manifest models must be a list")
    if not all(isinstance(model, dict) for model in models):
        raise VerificationError("manifest model entries must be objects")
    artifact_paths: set[Path] = set()
    for model in models:
        for field in ("source", "param", "bin"):
            if not isinstance(model.get(field), str) or not model[field]:
                raise VerificationError(f"manifest model has invalid {field}")

    sources = {model["source"] for model in models}
    if sources != set(expected_sources) or len(models) != len(sources):
        raise VerificationError(
            f"unexpected OCR model set: expected {sorted(expected_sources)}, got {sorted(sources)}"
        )

    for model in models:
        source_path = PurePosixPath(model["source"])
        kind = source_path.parent.name
        if kind not in {"det", "rec"} or model.get("kind") != kind:
            raise VerificationError(f"unexpected model kind for {model['source']}")
        if model.get("precision") != "fp32":
            raise VerificationError(f"unexpected precision for {model['source']}")
        expected_shape = DET_INPUTSHAPE if kind == "det" else REC_INPUTSHAPE
        if model.get("input_shape") != expected_shape:
            raise VerificationError(f"unexpected input_shape for {model['source']}")
        for hash_field in (
            "source_sha256",
            "cache_key",
            "param_sha256",
            "bin_sha256",
        ):
            if not _is_sha256(model.get(hash_field)):
                raise VerificationError(
                    f"manifest model has invalid {hash_field}: {model['source']}"
                )

        expected_param = (source_path.parent / f"{kind}.ncnn.param").as_posix()
        expected_bin = (source_path.parent / f"{kind}.ncnn.bin").as_posix()
        if model["param"] != expected_param:
            raise VerificationError(f"unexpected param path for {model['source']}")
        if model["bin"] != expected_bin:
            raise VerificationError(f"unexpected bin path for {model['source']}")

        identity = "\0".join(
            (
                model["source_sha256"],
                kind,
                model["precision"],
                EXPECTED_PNNX_VERSION,
                EXPECTED_RECIPE_VERSION,
            )
        )
        expected_cache_key = hashlib.sha256(identity.encode("ascii")).hexdigest()
        if model["cache_key"] != expected_cache_key:
            raise VerificationError(f"cache_key mismatch for {model['source']}")

        for artifact_key in ("param", "bin"):
            artifact_path = _resource_path(resource_dir, model[artifact_key])
            if artifact_path in artifact_paths:
                raise VerificationError(
                    f"duplicate {artifact_key} path: {model[artifact_key]}"
                )
            artifact_paths.add(artifact_path)

        source = _resource_path(resource_dir, model["source"])
        if require_onnx_removed and source.exists():
            raise VerificationError(
                f"OCR ONNX source was not removed: {model['source']}"
            )
        for artifact_key in ("param", "bin"):
            artifact = _resource_path(resource_dir, model[artifact_key])
            if not artifact.is_file() or artifact.stat().st_size == 0:
                raise VerificationError(
                    f"missing or empty NCNN artifact: {model[artifact_key]}"
                )
            expected_hash = model.get(f"{artifact_key}_sha256")
            if _sha256(artifact) != expected_hash:
                raise VerificationError(f"hash mismatch for {model[artifact_key]}")

    if require_onnx_removed:
        remaining_ocr_onnx = [
            path
            for path in resource_dir.rglob("inference.onnx")
            if path.parent.name in {"det", "rec"}
        ]
        if remaining_ocr_onnx:
            raise VerificationError(
                f"unconverted OCR ONNX remains: {remaining_ocr_onnx[0]}"
            )
    for relative in required_auxiliary_models:
        if not (resource_dir / relative).is_file():
            raise VerificationError(f"required auxiliary ONNX is missing: {relative}")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = verify_resource(args.resource)
    except VerificationError as error:
        parser.exit(1, f"NCNN OCR resource verification failed: {error}\n")
    print(f"NCNN OCR resource verification passed: {len(manifest['models'])} models")


if __name__ == "__main__":
    main()
