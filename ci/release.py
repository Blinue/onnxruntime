import os
import sys
import subprocess
import requests
import shutil

try:
    # https://docs.github.com/en/actions/learn-github-actions/variables
    if os.environ["GITHUB_ACTIONS"].lower() == "true":
        # 不知为何在 Github Actions 中运行时默认编码为 ANSI，并且 print 需刷新流才能正常显示
        for stream in [sys.stdout, sys.stderr]:
            stream.reconfigure(encoding="utf-8")
except:
    pass

# 下载依赖
response = requests.get(
    "https://github.com/Blinue/onnxruntime/releases/download/deps-250413/deps.zip",
    stream=True,
)
with open("deps.zip", "wb") as fd:
    for chunk in response.iter_content(chunk_size=10240):
        fd.write(chunk)

shutil.unpack_archive("deps.zip", "deps", "zip")

print("已下载依赖", flush=True)

# 打包
os.chdir("publish")

os.makedirs("onnxruntime-x64\\cuda")
os.rename("..\\deps\\cuda\\include", "onnxruntime-x64\\cuda\\include")
os.makedirs("onnxruntime-x64\\cuda\\lib")
os.rename("..\\deps\\cuda\\lib\\x64\\cudart.lib", "onnxruntime-x64\\cuda\\lib\\cudart.lib")

# tensorrt 拓展包
os.mkdir("ext-tensorrt-x64")
for provider in ["cuda", "shared", "tensorrt"]:
    os.rename(f"onnxruntime-x64\\onnxruntime_providers_{provider}.dll", f"ext-tensorrt-x64\\onnxruntime_providers_{provider}.dll")
os.rename("..\\deps\\cuda\\bin\\cudart64_12.dll", "ext-tensorrt-x64\\cudart64_12.dll")
for fileName in os.listdir("..\\deps\\tensorrt\\bin"):
    os.rename(f"..\\deps\\tensorrt\\bin\\{fileName}", f"ext-tensorrt-x64\\{fileName}")

for pkgName in ["onnxruntime-x64", "onnxruntime-ARM64", "ext-tensorrt-x64"]:
    shutil.make_archive(pkgName, "zip", pkgName)

print("打包完成", flush=True)

# 配置 git
tag = os.environ["TAG"]
githubAccessToken = os.environ["ACCESS_TOKEN"]
repo = os.environ["GITHUB_REPOSITORY"]
actor = os.environ["GITHUB_ACTOR"]

subprocess.run("git config user.name " + actor)
subprocess.run(f"git config user.email {actor}@users.noreply.github.com")

subprocess.run(
    f"git remote set-url origin https://{githubAccessToken}@github.com/{repo}.git"
)

# 打标签
if subprocess.run(f"git tag -a {tag} -m {tag}").returncode != 0:
    raise Exception("打标签失败")

if subprocess.run("git push origin " + tag).returncode != 0:
    raise Exception("推送标签失败")

print("已创建标签 " + tag, flush=True)

# 发布新版本
headers = {
    "Accept": "application/vnd.github+json",
    "Authorization": "Bearer " + githubAccessToken,
    "X-GitHub-Api-Version": "2022-11-28",
}

response = requests.post(
    f"https://api.github.com/repos/{repo}/releases",
    json={
        "tag_name": tag,
        "name": tag
    },
    headers=headers,
)
if not response.ok:
    raise Exception("发布失败")

uploadUrl = response.json()["upload_url"]
uploadUrl = uploadUrl[: uploadUrl.find("{")] + "?name="

# 上传资产
for pkgName in ["onnxruntime-x64.zip", "onnxruntime-ARM64.zip", "ext-tensorrt-x64.zip"]:
    with open(pkgName, "rb") as f:
        # 流式上传
        # https://requests.readthedocs.io/en/latest/user/advanced/#streaming-uploads
        response = requests.post(
            uploadUrl + pkgName,
            data=f,
            headers={**headers, "Content-Type": "application/zip"},
        )
        if not response.ok:
            raise Exception("上传失败")

print("已发布 " + tag, flush=True)
