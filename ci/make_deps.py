import os
import sys
import requests
import subprocess
import shutil
import zipfile
import threading

try:
    # https://docs.github.com/en/actions/learn-github-actions/variables
    if os.environ["GITHUB_ACTIONS"].lower() == "true":
        # 不知为何在 Github Actions 中运行时默认编码为 ANSI，并且 print 需刷新流才能正常显示
        for stream in [sys.stdout, sys.stderr]:
            stream.reconfigure(encoding="utf-8")
except:
    pass

cudaVer = os.environ["CUDA_VER"]
cudnnVer = os.environ["CUDNN_VER"]
trtVer = os.environ["TRT_VER"]

if len(cudaVer.split(".")) != 3:
    raise Exception("CUDA 版本号应为 x.y.z")
if len(cudnnVer.split(".")) != 3:
    raise Exception("cuDNN 版本号应为 x.y.z")
if len(trtVer.split(".")) != 4:
    raise Exception("TensorRT 版本号应为 x.y.z.w")

cudaMajorMinor = ".".join(cudaVer.split(".")[0:2])
cudnnMajorMinor = ".".join(cudnnVer.split(".")[0:2])
trtMajorMinorPatch = ".".join(trtVer.split(".")[0:3])
trtMajor = trtVer.split(".")[0]

os.mkdir("deps")

# CUDA 和 cuDNN 不能同时安装
lock = threading.Lock()
fail = False


def deploy_cuda():
    try:
        # 下载 CUDA
        response = requests.get(
            f"https://developer.download.nvidia.com/compute/cuda/{cudaVer}/network_installers/cuda_{cudaVer}_windows_network.exe",
            stream=True,
        )
        with open("cuda_installer.exe", "wb") as fd:
            for chunk in response.iter_content(chunk_size=1024):
                fd.write(chunk)

        with lock:
            # 静默安装，命令行参数参见 https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/#install-the-cuda-software
            subprocess.run(
                f"cuda_installer.exe -s cudart_{cudaMajorMinor} nvcc_{cudaMajorMinor} cublas_{cudaMajorMinor} cublas_dev_{cudaMajorMinor} visual_studio_integration_{cudaMajorMinor}",
                shell=True,
                check=True,
            )

        # 复制文件
        shutil.copytree(
            f"C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v{cudaMajorMinor}",
            "deps\\cuda",
            # 跳过几个不需要且体积较大的文件
            ignore=shutil.ignore_patterns(
                "cublas*.dll", "nvptxcompiler_static.lib"),
        )

        print("已部署 CUDA", flush=True)
    except:
        global fail
        fail = True


def deploy_cudnn():
    try:
        # 下载 cuDNN
        response = requests.get(
            f"https://developer.download.nvidia.com/compute/cudnn/{cudnnVer}/local_installers/cudnn_{cudnnVer}_windows.exe",
            stream=True,
        )
        with open("cudnn_installer.exe", "wb") as fd:
            for chunk in response.iter_content(chunk_size=10240):
                fd.write(chunk)

        with lock:
            # 静默安装
            subprocess.run(
                "cudnn_installer.exe -s",
                shell=True,
                check=True,
            )

        # 复制文件
        os.mkdir("deps\\cudnn")
        shutil.copytree(
            f"C:\\Program Files\\NVIDIA\\CUDNN\\v{cudnnMajorMinor}\\include\\{cudaMajorMinor}",
            "deps\\cudnn\\include",
        )
        os.mkdir("deps\\cudnn\\lib")
        shutil.copy2(
            f"C:\\Program Files\\NVIDIA\\CUDNN\\v{cudnnMajorMinor}\\lib\\{cudaMajorMinor}\\x64\\cudnn.lib",
            "deps\\cudnn\\lib\\cudnn.lib",
        )

        print("已部署 cuDNN", flush=True)
    except:
        global fail
        fail = True


def deploy_tensorrt():
    try:
        # 下载 TensorRT
        response = requests.get(
            f"https://developer.nvidia.com/downloads/compute/machine-learning/tensorrt/{trtMajorMinorPatch}/zip/TensorRT-{trtVer}.Windows.win10.cuda-{cudaMajorMinor}.zip",
            stream=True,
        )
        with open("tensorrt.zip", "wb") as fd:
            for chunk in response.iter_content(chunk_size=10240):
                fd.write(chunk)

        os.mkdir("deps\\tensorrt")
        os.mkdir("deps\\tensorrt\\include")
        os.mkdir("deps\\tensorrt\\lib")
        os.mkdir("deps\\tensorrt\\bin")

        # 只解压需要的文件
        with zipfile.ZipFile("tensorrt.zip", "r") as zipFile:
            for fileInfo in zipFile.infolist():
                prefix = f"TensorRT-{trtVer}/"

                if fileInfo.filename.startswith(f"{prefix}include/"):
                    # 创建中间路径
                    relativePath = os.path.relpath(
                        fileInfo.filename, f"{prefix}include/")
                    fullPath = os.path.join(
                        "deps\\tensorrt\\include", relativePath)
                    os.makedirs(os.path.dirname(fullPath), exist_ok=True)

                    if not fileInfo.filename.endswith("/"):
                        with zipFile.open(fileInfo) as source, open(fullPath, "wb") as target:
                            target.write(source.read())
                elif fileInfo.filename.endswith(".lib"):
                    for fileName in [
                        f"nvinfer_{trtMajor}.lib",
                        f"nvinfer_plugin_{trtMajor}.lib",
                        f"nvonnxparser_{trtMajor}.lib",
                    ]:
                        if fileInfo.filename == f"{prefix}lib/{fileName}":
                            with zipFile.open(fileInfo) as source, open(
                                    f"deps\\tensorrt\\lib\\{fileName}", "wb"
                            ) as target:
                                target.write(source.read())

                            break
                elif fileInfo.filename.endswith(".dll"):
                    for fileName in [
                        f"nvinfer_{trtMajor}.dll",
                        f"nvinfer_builder_resource_{trtMajor}.dll",
                        f"nvinfer_plugin_{trtMajor}.dll",
                        f"nvonnxparser_{trtMajor}.dll",
                    ]:
                        if fileInfo.filename == f"{prefix}lib/{fileName}":
                            with zipFile.open(fileInfo) as source, open(
                                f"deps\\tensorrt\\bin\\{fileName}", "wb"
                            ) as target:
                                target.write(source.read())

                            break

        print("已部署 TensorRT", flush=True)
    except:
        global fail
        fail = True


# 并发部署三个依赖
threads = []
for func in [deploy_cuda, deploy_cudnn, deploy_tensorrt]:
    t = threading.Thread(target=func)
    t.start()
    threads.append(t)

for t in threads:
    t.join()

if fail:
    raise Exception("部署依赖失败")

# 打包
shutil.make_archive("deps", "zip", "deps")
print("已打包", flush=True)

# 更新资产
repo = os.environ["GITHUB_REPOSITORY"]
githubAccessToken = os.environ["ACCESS_TOKEN"]

headers = {
    "Accept": "application/vnd.github+json",
    "Authorization": "Bearer " + githubAccessToken,
    "X-GitHub-Api-Version": "2022-11-28",
}

# 检索版本 id
response = requests.get(
    f"https://api.github.com/repos/{repo}/releases/tags/deps",
    headers=headers,
)
if not response.ok:
    raise Exception("获取版本失败")

responseJson = response.json()
releaseId = responseJson["id"]

uploadUrl = responseJson["upload_url"]
uploadUrl = uploadUrl[: uploadUrl.find("{")] + "?name="

for asset in responseJson["assets"]:
    if asset["name"] == "deps.zip":
        assetId = asset["id"]
        break
else:
    raise Exception("检索资产 id 失败")

# 删除资产
response = requests.delete(
    f"https://api.github.com/repos/{repo}/releases/assets/{assetId}",
    headers=headers
)
if not response.ok:
    raise Exception("删除资产失败")

# 上传新资产
with open("deps.zip", "rb") as f:
    # 流式上传，见 https://requests.readthedocs.io/en/latest/user/advanced/#streaming-uploads
    response = requests.post(
        uploadUrl + "deps.zip",
        data=f,
        headers={**headers, "Content-Type": "application/zip"},
    )
    if not response.ok:
        raise Exception("上传资产失败")

# 更新版本说明
body = f"""| SDK | 版本 |
|--------|--------|
| CUDA | {cudaVer} |
| cuDNN | {cudnnVer} |
| TensorRT | {trtVer} |"""

response = requests.patch(
    f"https://api.github.com/repos/{repo}/releases/{releaseId}",
    json={"body": body},
    headers=headers
)
if not response.ok:
    raise Exception("更新版本说明失败")

print("已更新资产和版本说明", flush=True)
