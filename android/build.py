#!/usr/bin/env python3
"""Build with the installed SDK/JDK; no Gradle, downloads, or third-party app libs."""
import hashlib
import os
from pathlib import Path
import subprocess
import zipfile
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent
sdk = Path(os.environ["ANDROID_HOME"])
jdk = Path(os.environ["JAVA_HOME"])
bt = sdk / "build-tools/34.0.0"
android = sdk / "platforms/android-34/android.jar"
build_root = root / "build"
build_root.mkdir(parents=True, exist_ok=True)
build = Path(tempfile.mkdtemp(prefix="v2-", dir=build_root))
dist = root / "dist"
for directory in (build / "classes", build / "dex", dist):
    directory.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, JAVA_HOME=str(jdk), PATH=str(jdk / "bin") + os.pathsep + os.environ.get("PATH", ""))
libraries = []
for aar in sorted((root / "libs").glob("*-13.1.5.aar")):
    jar = build / (aar.stem + ".jar")
    with zipfile.ZipFile(aar) as archive:
        jar.write_bytes(archive.read("classes.jar"))
    libraries.append(jar)
if len(libraries) != 4:
    raise RuntimeError("Expected pinned Shizuku api/provider/aidl/shared 13.1.5 AARs in libs/")

def tool(base, name):
    for ext in ("", ".exe", ".bat", ".cmd"):
        p = base / (name + ext)
        if p.exists():
            return p
    return base / name

def run(*args, cwd=root):
    cmd = [str(arg) for arg in args]
    subprocess.run(cmd, cwd=cwd, env=env, check=True)

run(tool(jdk / "bin", "javac"), "-encoding", "UTF-8", "--release", "8",
    "-classpath", os.pathsep.join(str(p) for p in [android, *libraries]), "-d", build / "classes", *sorted((root / "src").rglob("*.java")))
run(tool(jdk / "bin", "jar"), "cf", build / "classes.jar", "-C", build / "classes", ".")
run(tool(bt, "d8"), "--min-api", "26", "--lib", android, "--output", build / "dex", build / "classes.jar", *libraries)
run(tool(bt, "aapt"), "package", "-f", "-M", root / "AndroidManifest.xml", "-S", root / "res",
    "-A", root / "assets", "-I", android, "-F", build / "unsigned.apk")
with zipfile.ZipFile(build / "unsigned.apk", "a", zipfile.ZIP_DEFLATED) as archive:
    archive.write(build / "dex/classes.dex", "classes.dex")
run(tool(bt, "zipalign"), "-f", "4", build / "unsigned.apk", build / "aligned.apk")
key = build_root / "debug.keystore"
if not key.exists():
    run(tool(jdk / "bin", "keytool"), "-genkeypair", "-keystore", key, "-storepass", "android", "-keypass", "android",
        "-alias", "qpolllab", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
        "-dname", "CN=QPoll Lab Local Debug", "-noprompt")
version = ET.parse(root / "AndroidManifest.xml").getroot().attrib["{http://schemas.android.com/apk/res/android}versionName"]
apk = dist / ("QPollLab-" + version + ".apk")
run(tool(bt, "apksigner"), "sign", "--ks", key, "--ks-key-alias", "qpolllab", "--ks-pass", "pass:android",
    "--key-pass", "pass:android", "--out", apk, build / "aligned.apk")
run(tool(bt, "apksigner"), "verify", "--verbose", apk)
run(tool(bt, "aapt"), "dump", "badging", apk)
digest = hashlib.sha256(apk.read_bytes()).hexdigest()
(dist / (apk.name + ".sha256")).write_text(digest + "  " + apk.name + "\n", encoding="utf-8")
print("APK:", apk)
print("SHA256:", digest)
