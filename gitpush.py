import subprocess
import os

os.chdir(r"E:\CS\Desktop\ESP32-YG")

subprocess.run(["git", "add", "."])
subprocess.run(["git", "commit", "-m", "fix: 修复喂食按钮无法点击"])
subprocess.run(["git", "push"])

print("Done!")
