"""
package_models.py — 打包模型文件，准备上传到 GitHub Releases

用法:
  python scripts/package_models.py          # 打包 ASR + TTS + VAD 模型 → models-core.zip

上传到 GitHub Releases:
  1. 在 GitHub 仓库页面点击 Releases → Create a new release
  2. 填写版本号（如 v1.0）和说明
  3. 把生成的 zip 文件拖到附件区上传
  4. 发布 Release

模型包结构:
  models-core.zip
  ├── asr/
  │   ├── encoder-epoch-99-avg-1.int8.onnx
  │   ├── decoder-epoch-99-avg-1.int8.onnx
  │   ├── joiner-epoch-99-avg-1.onnx
  │   └── tokens.txt
  ├── tts/
  │   ├── model.onnx
  │   ├── tokens.txt
  │   ├── lexicon.txt
  │   └── dict/...          # jieba 分词词典
  └── vad/
      └── silero_vad.onnx
"""

import os
import sys
import zipfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

PROJECT_DIR = Path(__file__).resolve().parent.parent

# 核心模型来源目录（HAP 打包用的）
RAWFILE_MODELS = PROJECT_DIR / "entry" / "src" / "main" / "resources" / "rawfile" / "models"

# 打包输出目录
OUTPUT_DIR = PROJECT_DIR


def package_core():
    """打包核心模型（ASR + TTS + VAD）"""
    output_path = OUTPUT_DIR / "models-core.zip"

    if not RAWFILE_MODELS.exists():
        print(f"❌ 核心模型目录不存在: {RAWFILE_MODELS}")
        print("   请先确认模型文件已放置到位")
        return False

    print(f"📦 打包核心模型...")
    print(f"   来源: {RAWFILE_MODELS}")
    print(f"   输出: {output_path}")

    files_added = 0
    try:
        with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for root, dirs, files in os.walk(str(RAWFILE_MODELS)):
                for fname in files:
                    full_path = Path(root) / fname
                    # 计算相对路径（相对于 RAWFILE_MODELS）
                    rel_path = full_path.relative_to(RAWFILE_MODELS)
                    zf.write(full_path, str(rel_path))
                    files_added += 1

        size_mb = output_path.stat().st_size / 1_000_000
        print(f"✅ 打包完成: {files_added} 个文件, {size_mb:.0f}MB")
        print(f"   文件: {output_path.name}")
        return True

    except Exception as e:
        print(f"❌ 打包失败: {e}")
        return False


def main():
    print("=" * 60)
    print("  OHOSNativeAIEngine — 模型打包工具")
    print("=" * 60)
    print()

    if package_core():
        print("\n✅ 核心模型打包完成！")


if __name__ == "__main__":
    main()
