adb shell cmd connectivity airplane-mode enable
adb shell svc data disable
adb shell cmd phone data disable
adb shell svc wifi disable
adb shell cmd wifi set-scan-always-available disabled

adb shell cmd bluetooth_manager disable
adb shell cmd bluetooth_manager wait-for-state:STATE_OFF

adb shell svc nfc disable


# Check Output
adb shell cmd connectivity airplane-mode
adb shell dumpsys wifi | grep -m1 "Wi-Fi is"
adb shell settings get global wifi_scan_always_enabled
adb shell cmd bluetooth_manager wait-for-state:STATE_OFF \
    && echo "Bluetooth STATE_OFF"
adb shell settings get global ble_scan_always_enabled
adb shell ip route