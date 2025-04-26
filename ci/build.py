import os
import sys
import requests
import shutil
import subprocess
import glob

try:
    # https://docs.github.com/en/actions/learn-github-actions/variables
    if os.environ["GITHUB_ACTIONS"].lower() == "true":
        # 不知为何在 Github Actions 中运行时默认编码为 ANSI，并且 print 需刷新流才能正常显示
        for stream in [sys.stdout, sys.stderr]:
            stream.reconfigure(encoding="utf-8")
except:
    pass

platform = "x64"
if len(sys.argv) >= 2:
    platform = sys.argv[1]
    if not platform in ["x64", "ARM64"]:
        raise Exception("非法参数")

# 编译
cwd = os.getcwd()
if platform == "x64":
    if not os.path.exists("deps"):
        # 下载依赖
        response = requests.get(
            "https://github.com/Blinue/onnxruntime/releases/download/deps/deps.zip",
            stream=True,
        )
        with open("deps.zip", "wb") as fd:
            for chunk in response.iter_content(chunk_size=10240):
                fd.write(chunk)

        shutil.unpack_archive("deps.zip", "deps", "zip")
        os.remove("deps.zip")

    subprocess.run(
        f'python tools\\ci_build\\build.py --build_dir "{cwd}\\build" --config Release --build_shared_lib --parallel --compile_no_warning_as_error --skip_tests --enable_msvc_static_runtime --enable_lto --disable_rtti --use_dml --use_cuda --enable_cuda_minimal_build --cudnn_home "{cwd}\\deps\\cudnn" --cuda_home "{cwd}\\deps\\cuda" --use_tensorrt --tensorrt_home "{cwd}\\deps\\tensorrt',
        shell=True,
        text=True,
        check=True,
    )
else:
    subprocess.run(
        f'python tools\\ci_build\\build.py --arm64 --build_dir "{cwd}\\build" --config Release --build_shared_lib --parallel --compile_no_warning_as_error --skip_tests --enable_msvc_static_runtime --enable_lto --disable_rtti --use_dml',
        shell=True,
        text=True,
        check=True
    )

# 清理不需要的文件
os.chdir(f"build\\Release\\Release")

for pattern in ["*.pdb", "*.lib", "*.exp"]:
    for file in glob.glob(pattern):
        if file != "onnxruntime.lib":
            os.remove(file)

if platform == "ARM64":
    os.remove("onnxruntime_providers_shared.dll")
