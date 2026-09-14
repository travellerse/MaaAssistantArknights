#!/usr/bin/env python3
"""Convert staged PaddleOCR ONNX models to NCNN artifacts.

The conversion recipe originates from MAA-Meow's scripts/convert_ocr_ncnn.py.
It is intentionally restricted to an installed resource tree.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

try:
    from tools.ConvertOcrNcnn.verify_manifest import verify_resource
except ModuleNotFoundError:
    from verify_manifest import verify_resource

PNNX_VERSION = "20260526"
RECIPE_VERSION = "1"
CACHE_MANIFEST = "complete.json"
REC_INPUTSHAPE = "[1,3,48,320]"
DET_INPUTSHAPE = "[1,3,640,640]"
SOURCE_RESOURCE_DIR = Path(__file__).resolve().parents[2] / "resource"


class OcrModel(NamedTuple):
    path: Path
    kind: str


def _ensure_staged_resource(resource_dir: Path) -> None:
    resource_dir = resource_dir.resolve()
    source_resource_dir = SOURCE_RESOURCE_DIR.resolve()
    if resource_dir.is_relative_to(
        source_resource_dir
    ) or source_resource_dir.is_relative_to(resource_dir):
        raise SystemExit("refusing to convert the repository source resource tree")


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def cache_key(
    onnx_path: Path,
    kind: str,
    precision: str = "fp32",
    pnnx_version: str = PNNX_VERSION,
    recipe_version: str = RECIPE_VERSION,
) -> str:
    identity = "\0".join(
        (sha256_file(onnx_path), kind, precision, pnnx_version, recipe_version)
    )
    return hashlib.sha256(identity.encode("ascii")).hexdigest()


def discover_models(resource_dir: Path) -> list[OcrModel]:
    models: list[OcrModel] = []
    for onnx_path in resource_dir.rglob("inference.onnx"):
        kind = onnx_path.parent.name
        if kind in {"det", "rec"}:
            models.append(OcrModel(onnx_path, kind))
    return sorted(models, key=lambda model: model.path.as_posix())


def _find_pnnx() -> str:
    try:
        distribution = importlib.metadata.distribution("pnnx")
    except importlib.metadata.PackageNotFoundError as error:
        raise SystemExit(
            "pinned pnnx package not found; install "
            "tools/ConvertOcrNcnn/requirements.txt"
        ) from error
    if distribution.version != PNNX_VERSION:
        raise SystemExit(
            f"pnnx {distribution.version} is installed; expected {PNNX_VERSION}"
        )
    for relative in ("pnnx/pnnx", "pnnx/pnnx.exe"):
        binary = Path(distribution.locate_file(relative))
        if binary.is_file():
            return str(binary)
    raise SystemExit(f"pnnx {PNNX_VERSION} package does not contain its executable")


def _run(command: list[str], cwd: Path) -> None:
    result = subprocess.run(command, cwd=cwd, check=False)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def _pnnx_outputs(work_dir: Path) -> tuple[Path, Path]:
    params = list(work_dir.glob("*.ncnn.param"))
    if len(params) != 1:
        raise SystemExit(
            f"expected one *.ncnn.param in {work_dir}, found {len(params)}"
        )
    param = params[0]
    binary = param.with_name(param.name.removesuffix(".param") + ".bin")
    if not binary.is_file():
        raise SystemExit(f"pnnx output is incomplete: {binary} not found")
    return param, binary


def _write_cache_manifest(cache_param: Path, cache_bin: Path) -> None:
    manifest = {
        "param_sha256": sha256_file(cache_param),
        "bin_sha256": sha256_file(cache_bin),
    }
    manifest_path = cache_param.parent / CACHE_MANIFEST
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=manifest_path.parent, delete=False
    ) as stream:
        json.dump(manifest, stream, sort_keys=True)
        stream.write("\n")
        temporary_manifest = Path(stream.name)
    temporary_manifest.replace(manifest_path)


def _verify_staged_resource(
    resource_dir: Path,
    *,
    expected_sources: set[str],
    require_onnx_removed: bool,
) -> None:
    """校验 staged 资源树；失败时抛出 SystemExit（此时源 ONNX 尚未删除）。"""
    try:
        verify_resource(
            resource_dir,
            expected_sources=expected_sources,
            required_auxiliary_models=(),
            require_onnx_removed=require_onnx_removed,
        )
    except Exception as error:
        raise SystemExit(f"NCNN OCR resource verification failed: {error}") from error


def _atomic_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        shutil.copy2(source, temporary)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def _atomic_write_text(destination: Path, content: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=destination.parent, delete=False
    ) as stream:
        stream.write(content)
        temporary = Path(stream.name)
    try:
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def _is_complete_cache(cache_param: Path, cache_bin: Path) -> bool:
    manifest_path = cache_param.parent / CACHE_MANIFEST
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        return (
            cache_param.is_file()
            and cache_param.stat().st_size > 0
            and cache_bin.is_file()
            and cache_bin.stat().st_size > 0
            and manifest.get("param_sha256") == sha256_file(cache_param)
            and manifest.get("bin_sha256") == sha256_file(cache_bin)
        )
    except (OSError, json.JSONDecodeError, AttributeError):
        return False


def _convert_to_cache(model: OcrModel, cache_param: Path, cache_bin: Path) -> None:
    input_shape = DET_INPUTSHAPE if model.kind == "det" else REC_INPUTSHAPE
    with tempfile.TemporaryDirectory(prefix=f"maa_ncnn_{model.kind}_") as temp_dir:
        work_dir = Path(temp_dir)
        work_onnx = work_dir / f"{model.kind}.onnx"
        shutil.copy2(model.path, work_onnx)
        _run(
            [_find_pnnx(), work_onnx.name, f"inputshape={input_shape}", "fp16=0"],
            cwd=work_dir,
        )
        param, binary = _pnnx_outputs(work_dir)
        cache_param.parent.mkdir(parents=True, exist_ok=True)
        (cache_param.parent / CACHE_MANIFEST).unlink(missing_ok=True)
        with tempfile.NamedTemporaryFile(
            dir=cache_param.parent, delete=False
        ) as stream:
            temporary_param = Path(stream.name)
        with tempfile.NamedTemporaryFile(dir=cache_bin.parent, delete=False) as stream:
            temporary_bin = Path(stream.name)
        try:
            shutil.copy2(param, temporary_param)
            shutil.copy2(binary, temporary_bin)
            temporary_param.replace(cache_param)
            temporary_bin.replace(cache_bin)
            _write_cache_manifest(cache_param, cache_bin)
        finally:
            temporary_param.unlink(missing_ok=True)
            temporary_bin.unlink(missing_ok=True)


def _convert_one(model: OcrModel, cache_dir: Path) -> tuple[bool, Path, Path, str]:
    key = cache_key(model.path, model.kind)
    cache_param = cache_dir / key / f"{model.kind}.ncnn.param"
    cache_bin = cache_dir / key / f"{model.kind}.ncnn.bin"
    cache_hit = _is_complete_cache(cache_param, cache_bin)
    if not cache_hit:
        _convert_to_cache(model, cache_param, cache_bin)

    output_param = model.path.parent / f"{model.kind}.ncnn.param"
    output_bin = model.path.parent / f"{model.kind}.ncnn.bin"
    _atomic_copy(cache_param, output_param)
    _atomic_copy(cache_bin, output_bin)
    return cache_hit, output_param, output_bin, key


def convert_tree(
    resource_dir: Path, cache_dir: Path, keep_onnx: bool = True
) -> dict[str, int]:
    resource_dir = resource_dir.resolve()
    cache_dir = cache_dir.resolve()
    _ensure_staged_resource(resource_dir)
    models = discover_models(resource_dir)
    if not models:
        raise SystemExit(f"no OCR inference.onnx models found under {resource_dir}")

    stats = {"converted": 0, "cached": 0, "onnx_removed": 0}
    manifest_models: list[dict[str, str]] = []
    for model in models:
        source_hash = sha256_file(model.path)
        cache_hit, output_param, output_bin, key = _convert_one(model, cache_dir)
        stats["cached" if cache_hit else "converted"] += 1
        manifest_models.append(
            {
                "source": model.path.relative_to(resource_dir).as_posix(),
                "source_sha256": source_hash,
                "kind": model.kind,
                "precision": "fp32",
                "input_shape": DET_INPUTSHAPE
                if model.kind == "det"
                else REC_INPUTSHAPE,
                "cache_key": key,
                "param": output_param.relative_to(resource_dir).as_posix(),
                "param_sha256": sha256_file(output_param),
                "bin": output_bin.relative_to(resource_dir).as_posix(),
                "bin_sha256": sha256_file(output_bin),
            }
        )

    manifest = {
        "schema_version": 1,
        "recipe_version": RECIPE_VERSION,
        "pnnx_version": PNNX_VERSION,
        "models": manifest_models,
    }
    _atomic_write_text(
        resource_dir / "ocr-ncnn-manifest.json",
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    )

    if not keep_onnx:
        sources = {model.path.relative_to(resource_dir).as_posix() for model in models}
        # 先校验产物与 manifest 完整、一致，再删除源 ONNX；
        # 校验失败时源模型保留，安装树仍可重试/修复。
        _verify_staged_resource(
            resource_dir,
            expected_sources=sources,
            require_onnx_removed=False,
        )
        for model in models:
            model.path.unlink()
            stats["onnx_removed"] += 1
        _verify_staged_resource(
            resource_dir,
            expected_sources=sources,
            require_onnx_removed=True,
        )
    return stats


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert staged PaddleOCR ONNX models to NCNN"
    )
    parser.add_argument("--resource", required=True, type=Path)
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument(
        "--remove-onnx",
        action="store_true",
        help="remove converted OCR ONNX files from the staged resource tree",
    )
    args = parser.parse_args()

    if not args.resource.is_dir():
        parser.error(f"resource directory not found: {args.resource}")
    stats = convert_tree(args.resource, args.cache, keep_onnx=not args.remove_onnx)
    print(
        "NCNN OCR conversion complete: "
        f"converted={stats['converted']} cached={stats['cached']} "
        f"onnx_removed={stats['onnx_removed']}"
    )


if __name__ == "__main__":
    if sys.platform == "win32":
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    main()
