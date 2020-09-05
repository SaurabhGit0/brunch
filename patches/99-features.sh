# This patch activates specific ChromeOS flags in /etc/chrome_dev.conf:
# - Enable debug keyboard shortcuts
# - Enable external storage ui for arc (without this ChromeOS creates .android folders on all partitions considered external)
# - Allow gpu sandbox failures, without this some devices will fail to boot
# - Specify hardware overlays to avoid android apps flickering on certain devices
# - Add support for acpi power buttons (disabled by default in ChromeOS) through the use of "acpi_power_button" option

cros_debug=0
for i in $(cat /proc/cmdline)
do
	if [ "$i" == "cros_debug" ]; then cros_debug=1; fi
done

acpi_power_button=0
for i in $(echo "$1" | sed 's#,# #g')
do
	if [ "$i" == "acpi_power_button" ]; then acpi_power_button=1; fi
done

acpi_power_button_sleep=0
for i in $(echo "$1" | sed 's#,# #g')
do
	if [ "$i" == "acpi_power_button_sleep" ]; then acpi_power_button_sleep=1; fi
done

ret=0

if [ ! -f "/system/etc/chrome_dev.conf" ]; then ret=$((ret + (2 ** 0))); fi
echo '--ash-debug-shortcuts' > /system/etc/chrome_dev.conf
if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 1))); fi
echo '--enable-features=ArcUsbStorageUI' >> /system/etc/chrome_dev.conf
if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 2))); fi
echo '--gpu-sandbox-failures-fatal=no' >> /system/etc/chrome_dev.conf
if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 3))); fi
echo '--enable-hardware-overlays="single-fullscreen,single-on-top"' >> /system/etc/chrome_dev.conf
if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 4))); fi

if [ "$acpi_power_button" -eq 1 ]; then
	echo '--aura-legacy-power-button' >> /system/etc/chrome_dev.conf
	if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 5))); fi
	echo 1 > /system/usr/share/power_manager/board_specific/legacy_power_button
	if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 6))); fi
elif [ "$acpi_power_button_sleep" -eq 1 ]; then
	echo '--force-tablet-power-button' >> /system/etc/chrome_dev.conf
	if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 7))); fi
	echo 1 > /system/usr/share/power_manager/board_specific/legacy_power_button
	if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 8))); fi
else
	echo '--force-tablet-power-button' >> /system/etc/chrome_dev.conf
	if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 9))); fi
fi

if [ "$cros_debug" -eq 0 ]; then

grep -q '\${CHROME_COMMAND_FLAG}' /system/etc/init/ui.conf
if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 10))); fi
flags="$(echo $(cat /system/etc/chrome_dev.conf))"
sed -i "s#\${CHROME_COMMAND_FLAG}#--chrome-command=/opt/google/chrome/chrome $flags#g" /system/etc/init/ui.conf
if [ ! "$?" -eq 0 ]; then ret=$((ret + (2 ** 11))); fi

fi

exit $ret
