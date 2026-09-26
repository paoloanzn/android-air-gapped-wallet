FASTBOOT=$(which fastboot)
echo "Fastboot=$FASTBOOT"
FASTBOOT_VERSION=$(fastboot --version)
echo "version=$FASTBOOT_VERSION"

adb shell am start -a android.settings.APPLICATION_DEVELOPMENT_SETTINGS
echo "Enable OEM unlocking on the phone"
echo "Sleeping for 30 seconds..."
sleep 30

adb reboot bootloader

echo "Sleeping 5 minutes before rebooting..."
sleep 300

fastboot reboot