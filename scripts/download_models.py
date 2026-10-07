"""
download_models.py — 下载 OHOSNativeAIEngine 内置语音模型

用法:
  python scripts/download_models.py               # 下载 ASR + TTS + VAD 模型
  python scripts/download_models.py --check        # 只检查哪些文件缺失，不下载

下载源（按优先级）:
  1. GitHub Releases（快）
  2. HuggingFace 镜像 hf-mirror.com（备选，较慢）

首次使用:
  1. 确保你已从 GitHub Releases 下载了模型包，或脚本会自动下载
  2. 模型文件会放置到 entry/src/main/resources/rawfile/models/
  3. 然后就可以用 DevEco Studio 编译了
"""

import os
import sys
import zipfile
import urllib.request
import shutil
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

# ============================================================
# 配置区 —— 发布 Release 时修改这里的 URL
# ============================================================

# GitHub 仓库信息（改成你自己的）
REPO_OWNER = "cl-2"
REPO_NAME = "OHOSNativeAIEngine"
RELEASE_TAG = "v1.0"

# 模型包下载地址（GitHub Releases）
# 你需要先手动上传模型包到 Releases，详见 package_models.py
CORE_MODELS_URL = (
    f"https://github.com/{REPO_OWNER}/{REPO_NAME}/releases/download/"
    f"{RELEASE_TAG}/models-core.zip"
)
# ============================================================
# 路径配置（不要改）
# ============================================================

PROJECT_DIR = Path(__file__).resolve().parent.parent
RAWFILE_DIR = PROJECT_DIR / "entry" / "src" / "main" / "resources" / "rawfile" / "models"

# 核心模型清单（打包进 HAP，安装即用）
CORE_MODEL_FILES = {
    # ASR 模型
    "asr/encoder-epoch-99-avg-1.int8.onnx": {
        "size": 161_141_793,
        "desc": "ASR 编码器",
    },
    "asr/decoder-epoch-99-avg-1.int8.onnx": {
        "size": 5_165_083,
        "desc": "ASR 解码器",
    },
    "asr/joiner-epoch-99-avg-1.onnx": {
        "size": 1_033_416,
        "desc": "ASR Joiner",
    },
    "asr/tokens.txt": {
        "size": 20_628,
        "desc": "ASR 词表",
    },
    # TTS 模型
    "tts/model.onnx": {
        "size": 170_429_550,
        "desc": "TTS 语音合成模型",
    },
    "tts/tokens.txt": {
        "size": 655,
        "desc": "TTS 词表",
    },
    "tts/lexicon.txt": {
        "size": 6_837_671,
        "desc": "TTS 词典",
    },
    # VAD 模型
    "vad/silero_vad.onnx": {
        "size": 643_854,
        "desc": "VAD 语音活动检测",
    },
}

# ============================================================
# 工具函数
# ============================================================

def format_size(size: int) -> str:
    """人性化显示文件大小"""
    if size > 1_000_000_000:
        return f"{size / 1_000_000_000:.1f}GB"
    elif size > 1_000_000:
        return f"{size / 1_000_000:.0f}MB"
    elif size > 1_000:
        return f"{size / 1_000:.0f}KB"
    return f"{size}B"


def download_file(url: str, dest: Path, desc: str = "") -> bool:
    """下载文件并显示进度条"""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp_dest = dest.with_suffix(".part")

    def report(block: int, blocksize: int, totalsize: int):
        if totalsize > 0:
            pct = min(100, block * blocksize * 100 / totalsize)
            speed = block * blocksize / 1024 / 1024  # MB
            bar_len = 30
            filled = int(pct / 100 * bar_len)
            bar = "█" * filled + "░" * (bar_len - filled)
            sys.stdout.write(
                f"\r  {desc[:30]:30s} |{bar}| {pct:5.1f}%  ({speed:.1f}MB)"
            )
            sys.stdout.flush()

    try:
        urllib.request.urlretrieve(url, str(tmp_dest), report)
        if tmp_dest.exists():
            shutil.move(str(tmp_dest), str(dest))
        sys.stdout.write(f"\r  ✅ {desc[:30]:30s} 完成\n")
        return True
    except Exception as e:
        sys.stdout.write(f"\r  ❌ {desc[:30]:30s} 失败: {e}\n")
        if tmp_dest.exists():
            tmp_dest.unlink()
        return False


def check_files(base_dir: Path, files: dict) -> tuple:
    """检查文件存在情况，返回 (已存在列表, 缺失列表)"""
    present = []
    missing = []
    for rel_path, info in files.items():
        full_path = base_dir / rel_path
        if full_path.exists() and full_path.stat().st_size > 0:
            actual_size = full_path.stat().st_size
            # 允许 ±5% 的大小偏差
            expected = info["size"]
            if abs(actual_size - expected) / expected < 0.05:
                present.append(rel_path)
            else:
                missing.append((rel_path, f"大小不匹配: 期望{format_size(expected)}, 实际{format_size(actual_size)}"))
        else:
            missing.append((rel_path, "文件不存在"))
    return present, missing


# ============================================================
# 核心逻辑
# ============================================================

def check_core_models() -> bool:
    """检查核心模型是否齐全"""
    print("\n📦 检查核心模型（ASR + TTS + VAD）...")
    present, missing = check_files(RAWFILE_DIR, CORE_MODEL_FILES)

    for p in present:
        print(f"  ✅ {p}")

    if not missing:
        total = sum(f["size"] for f in CORE_MODEL_FILES.values())
        print(f"\n✅ 核心模型齐全 ({format_size(total)})")
        return True

    print(f"\n❌ 缺失 {len(missing)} 个文件:")
    for path, reason in missing:
        print(f"   - {path}: {reason}")
    return False


def download_core_models() -> bool:
    """从 GitHub Releases 下载核心模型包并解压"""
    zip_path = PROJECT_DIR / "models-core.zip"

    print(f"\n📥 下载核心模型包...")
    print(f"   源: {CORE_MODELS_URL}")
    print(f"   大小: ~250MB")

    # 先清理旧的下载
    if zip_path.exists():
        zip_path.unlink()

    ok = download_file(CORE_MODELS_URL, zip_path, "models-core.zip")
    if not ok:
        print("\n⚠️  GitHub Releases 下载失败，尝试备选源...")
        return download_core_fallback()

    # 解压到 rawfile 目录
    print("\n📂 解压到 entry/src/main/resources/rawfile/models/ ...")
    RAWFILE_DIR.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(zip_path, "r") as zf:
            zf.extractall(RAWFILE_DIR)
        zip_path.unlink()  # 清理压缩包
        print("✅ 解压完成")
        return True
    except Exception as e:
        print(f"❌ 解压失败: {e}")
        return False


def download_core_fallback() -> bool:
    """备选：从 HuggingFace 逐个下载模型文件"""
    print("\n📥 备选下载（从 hf-mirror.com，可能较慢）...")
    print("   建议优先使用 GitHub Releases 方式\n")

    # 构造备选下载 URL
    # 注意：这里需要根据实际模型来源填写正确的 URL
    fallback_urls = {
        "asr/encoder-epoch-99-avg-1.int8.onnx":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/encoder-epoch-99-avg-1.int8.onnx",
        "asr/decoder-epoch-99-avg-1.int8.onnx":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/decoder-epoch-99-avg-1.int8.onnx",
        "asr/joiner-epoch-99-avg-1.onnx":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/joiner-epoch-99-avg-1.onnx",
        "asr/tokens.txt":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/tokens.txt",
        "vad/silero_vad.onnx":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/silero_vad.onnx",
    }

    # TTS 模型需要从其他源下载
    tts_fallback = {
        "tts/model.onnx":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-vits-zh-ll/resolve/main/model.onnx",
        "tts/tokens.txt":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-vits-zh-ll/resolve/main/tokens.txt",
        "tts/lexicon.txt":
            "https://hf-mirror.com/k2-fsa/sherpa-onnx-vits-zh-ll/resolve/main/lexicon.txt",
    }

    all_ok = True

    for rel_path, url in {**fallback_urls, **tts_fallback}.items():
        dest = RAWFILE_DIR / rel_path
        if dest.exists() and dest.stat().st_size > 0:
            print(f"  ✅ {rel_path} (已存在)")
            continue
        desc = CORE_MODEL_FILES.get(rel_path, {}).get("desc", rel_path)
        ok = download_file(url, dest, desc)
        if not ok:
            all_ok = False

    return all_ok


# ============================================================
# 主入口
# ============================================================

def main():
    print("=" * 60)
    print("  OHOSNativeAIEngine — 模型下载工具")
    print("=" * 60)

    # 解析参数
    check_only = "--check" in sys.argv

    # --- 核心模型 ---
    core_ok = check_core_models()

    if check_only:
        print("\n" + "=" * 60)
        print(f"  核心模型: {'✅ 齐全' if core_ok else '❌ 缺失'}")
        print("=" * 60)
        return

    if not core_ok:
        print("\n📥 需要下载核心模型...")
        core_ok = download_core_models()
        if not core_ok:
            print("\n❌ 核心模型下载失败。请检查网络后重试。")
            print("   或手动从 Release 页面下载 models-core.zip")
            print(f"   {CORE_MODELS_URL}")
            sys.exit(1)
    else:
        print("\n✅ 核心模型已就绪，跳过下载")

    # --- 总结 ---
    print("\n" + "=" * 60)
    print("  📋 模型状态总结")
    print("=" * 60)
    check_core_models()

    print(f"\n  📁 核心模型位置: entry/src/main/resources/rawfile/models/")
    print()
    print(f"  下一步:")
    print(f"    用 DevEco Studio 打开项目，同步并编译即可")
    print(f"    HAP 将包含所有核心模型，安装即用 🎯")

    print()


if __name__ == "__main__":
    main()
