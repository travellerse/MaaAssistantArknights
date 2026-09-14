# NCNN OCR model conversion

This tool ports the conversion recipe validated in
`MAA-Meow/scripts/convert_ocr_ncnn.py`. It converts OCR models in an installed
Linux resource tree; it must not be run against the repository's source
`resource/` directory.

The pinned FP32 recipes are:

- detector: `inputshape=[1,3,640,640] fp16=0`
- recognizer: `inputshape=[1,3,48,320] fp16=0`

Run conversion after `cmake --install` and before packaging:

```bash
python3 -m pip install --no-deps --require-hashes \
  -r tools/ConvertOcrNcnn/requirements.txt
python3 tools/ConvertOcrNcnn/convert_ocr_ncnn.py \
  --resource install/resource \
  --cache .cache/ncnn-models \
  --remove-onnx
python3 tools/ConvertOcrNcnn/verify_manifest.py \
  --resource install/resource
```

Conversion writes `det.ncnn.param/bin` or `rec.ncnn.param/bin` beside each OCR
ONNX model and records hashes in `ocr-ncnn-manifest.json`. Conversion preserves
ONNX by default. Packaging passes `--remove-onnx`, which removes only
`*/det/inference.onnx` and `*/rec/inference.onnx`. With `--remove-onnx` the
converter first verifies the staged artifacts against the manifest and only
deletes the source ONNX files after that check passes; if verification fails,
the source models are preserved so the tree can be fixed and retried.
The three models in `resource/onnx/` are preserved and checked by the verifier.

The cache identity includes the ONNX SHA-256, model kind, precision, recipe
version, and pnnx version.
