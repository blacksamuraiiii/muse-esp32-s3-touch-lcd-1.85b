$env:PYTHONPATH = ""
$env:TEMP = "C:\Users\black_samurai\AppData\Local\Temp"
$env:TMP = "C:\Users\black_samurai\AppData\Local\Temp"
$env:IDF_COMPONENT_STORAGE_URL = "https://components-file.espressif.cn/"
$env:PATH = "C:\Users\black_samurai\.espressif\python_env\idf6.0_py3.14_env\Scripts;" + $env:PATH
$log = "C:\Users\black_samurai\Documents\project\202610Muse-Gadget-185B\esp32\flash_alarm.log"
Set-Location "C:\Users\black_samurai\Documents\project\202610Muse-Gadget-185B\esp32"
. "C:\Users\black_samurai\esp\esp-idf-v6\export.ps1" 2>&1 | Out-File -Encoding utf8 $log
"=== flash start $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" | Out-File -Encoding utf8 -Append $log
idf.py -B build-muse-waveshare-s3-touch-lcd-185b -p COM3 flash 2>&1 | Out-File -Encoding utf8 -Append $log
$c = $LASTEXITCODE
"=== flash end $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') exit=$c ===" | Out-File -Encoding utf8 -Append $log
exit 0
