FASTBOOT=$(which fastboot)
echo "Fastboot=$FASTBOOT"
FASTBOOT_VERSION=$(fastboot --version)
echo "version=$FASTBOOT_VERSION"

adb reboot bootloader

echo "Sleeping 10 minutes before rebooting..."
sleep 600

fastboot reboot