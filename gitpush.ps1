$env:Path = "C:\Program Files\Git\bin;C:\Windows\System32;" + $env:Path
git -C "E:\CS\Desktop\ESP32-YG" add .
git -C "E:\CS\Desktop\ESP32-YG" commit -m "fix: 修复喂食按钮无法点击"
git -C "E:\CS\Desktop\ESP32-YG" push