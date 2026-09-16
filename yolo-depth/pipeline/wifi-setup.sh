#!/usr/bin/env bash
# Bring up WiFi (station mode) on the QCS8550 board.
#
# Run this ON THE BOARD, over UART or a still-working ethernet link -- it
# reconfigures networking, so running it over the very SSH session it might
# disturb is asking for a lockout. UART is /dev/ttyUSB0, 115200/8N1,
# root / oelinux123.
#
# The board has no NetworkManager and no connman; systemd-networkd is masked.
# What it does have is wpa_supplicant + dhcpcd, so this is the manual path.
# The qcmap_wpa_supplicant@.service units are for Qualcomm's QCMAP AP/router
# framework and expect /var/run/data/*.conf from its daemon -- not the way in
# for plain "join an access point".
#
# Usage:
#   ./wifi-setup.sh <SSID> <passphrase>        # connect, then show the IP
#   ./wifi-setup.sh --scan                     # list visible networks
#   ./wifi-setup.sh --status                   # current association + IP
#
# This connects once; it does not survive a reboot. Making it persistent is a
# deliberate act -- write a systemd unit for it -- because an AP that associates
# but never hands out a lease (as the office AP does) would otherwise be baked
# into boot, and every failed retry floods the UART console.
#
# Exit code 0 on success, non-zero on failure.
set -euo pipefail

IFACE=wlan0
CONF=/etc/wpa_supplicant/wpa_supplicant-$IFACE.conf

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
ip link show "$IFACE" >/dev/null 2>&1 || die "no $IFACE on this board"

case "${1:-}" in
--scan)
	ip link set "$IFACE" up
	sleep 2
	# Pair each SSID with the signal and frequency from its own BSS block.
	iw dev "$IFACE" scan 2>/dev/null | awk '
		/^BSS /      { bss = $2 }
		/^\tfreq:/   { freq = $2 }
		/^\tsignal:/ { sig = $2 }
		/^\tSSID:/   { $1 = ""; printf "%-32s %6s dBm  %5s MHz\n", $0, sig, freq }
	' | sort -u
	exit 0
	;;
--status)
	iw dev "$IFACE" link 2>/dev/null || echo "not associated"
	ip -4 addr show "$IFACE" | grep -oE 'inet [0-9.]+' || echo "no IPv4 address"
	exit 0
	;;
esac

SSID="${1:-}"
PSK="${2:-}"
[ -n "$SSID" ] && [ -n "$PSK" ] || die "usage: $0 <SSID> <passphrase> | --scan | --status"
# Otherwise an unknown flag is silently taken as the SSID and fails further
# down for the wrong reason. --persist used to exist here; say so plainly.
case "$SSID" in
--persist)
	die "--persist was removed: it could bake in an AP that associates but
       never leases. Connect with '$0 <SSID> <passphrase>'; for boot-time
       WiFi write a systemd unit explicitly."
	;;
-*)
	die "unknown option '$SSID' (expected an SSID) -- see $0 with no arguments"
	;;
esac
[ "${#PSK}" -ge 8 ] || die "WPA passphrase must be at least 8 characters"

# rfkill can soft-block the radio; unblocking a device that is not blocked is
# harmless, so this runs unconditionally rather than parsing rfkill output.
rfkill unblock wifi 2>/dev/null || true

# Read the AP's advertised security before writing a config for it. A
# WPA2/WPA3 mixed-mode AP offers "PSK SAE", and leaving key_mgmt unset lets
# wpa_supplicant negotiate -- which failed here with repeated auth timeouts
# (resultCode 510) against an Algoltek AP advertising exactly that. Pinning
# WPA-PSK keeps it on the WPA2 path.
ip link set "$IFACE" up
sleep 2
AUTH="$(iw dev "$IFACE" scan 2>/dev/null | awk -v want="$SSID" '
	/^BSS /                     { in_bss = 0 }
	$1 == "SSID:" && $2 == want { in_bss = 1 }
	in_bss && /Authentication suites:/ {
		sub(/.*Authentication suites: /, ""); print; exit
	}')"
if iw dev "$IFACE" scan 2>/dev/null | grep -q "MFP-capable"; then
	# MFP optional, not required: a mixed-mode AP accepts WPA2 clients without
	# management-frame protection, and demanding it is another way to fail auth.
	MFP_LINE="ieee80211w=1"
else
	MFP_LINE=""
fi

case "$AUTH" in
*802.1X*)
	die "'$SSID' is WPA-Enterprise (802.1X: $AUTH) -- needs an EAP identity
       and certificate, which this script does not handle"
	;;
*SAE*)
	echo "note: '$SSID' advertises '$AUTH' (WPA2/WPA3 mixed); pinning WPA-PSK"
	KEY_MGMT="WPA-PSK"
	;;
*PSK*)
	KEY_MGMT="WPA-PSK"
	;;
"")
	echo "note: '$SSID' not seen in scan -- assuming WPA-PSK (run --scan to check)"
	KEY_MGMT="WPA-PSK"
	;;
*)
	die "'$SSID' advertises an unsupported auth suite: $AUTH"
	;;
esac

mkdir -p "$(dirname "$CONF")"
# wpa_passphrase hashes the PSK so the plaintext does not land in the file.
# It writes a commented copy of the plaintext too, hence the grep -v. The
# generated network block is then extended with the pinned key_mgmt.
{
	echo "ctrl_interface=/var/run/wpa_supplicant"
	echo "update_config=1"
	wpa_passphrase "$SSID" "$PSK" | grep -v '^\s*#psk=' | while IFS= read -r line; do
		# Inject the pinned settings just before the network block's closing brace.
		if [ "$line" = "}" ]; then
			printf '\tkey_mgmt=%s\n' "$KEY_MGMT"
			[ -n "$MFP_LINE" ] && printf '\t%s\n' "$MFP_LINE"
		fi
		printf '%s\n' "$line"
	done
} > "$CONF"
chmod 600 "$CONF"
echo "wrote $CONF (key_mgmt=$KEY_MGMT)"

pkill -f "wpa_supplicant.*$IFACE" 2>/dev/null || true
sleep 1
ip link set "$IFACE" up

wpa_supplicant -B -i "$IFACE" -c "$CONF" -D nl80211 \
	|| die "wpa_supplicant failed to start"

# Association is not instant and dhcpcd has nothing to ask until it completes.
for _ in $(seq 20); do
	if iw dev "$IFACE" link 2>/dev/null | grep -q "Connected to"; then
		break
	fi
	sleep 1
done
if ! iw dev "$IFACE" link 2>/dev/null | grep -q "Connected to"; then
	# Leave no retry loop behind: the driver logs every failed attempt to the
	# kernel ring buffer, which floods the UART console indefinitely.
	pkill -f "wpa_supplicant.*$IFACE" 2>/dev/null || true
	SIG="$(iw dev "$IFACE" scan 2>/dev/null | awk -v want="$SSID" '
		/^BSS /                     { in_bss = 0 }
		$1 == "SSID:" && $2 == want { in_bss = 1 }
		in_bss && /signal:/         { print $2; exit }')"
	die "did not associate with '$SSID' (auth suite '$AUTH', signal ${SIG:-unknown} dBm)
       -- wrong passphrase, or the signal is too weak to complete auth"
fi

echo "associated with $SSID"

# dhcpcd is already running as a daemon; -n asks it to renew this interface
# rather than starting a second instance.
dhcpcd -n "$IFACE" 2>/dev/null || dhcpcd "$IFACE" 2>/dev/null || true
for _ in $(seq 15); do
	if ip -4 addr show "$IFACE" | grep -q 'inet '; then
		break
	fi
	sleep 1
done

ADDR="$(ip -4 addr show "$IFACE" | grep -oE 'inet [0-9.]+' | awk '{print $2}')"
[ -n "$ADDR" ] || die "associated but no DHCP lease on $IFACE"

echo "$IFACE address: $ADDR"
echo
echo "Point the pipeline at it with either:"
echo "  BOARD=$ADDR ./run.sh 384 live"
echo "  # or edit BOARD_DEFAULT in board.env"
echo
echo "This does not survive a reboot. Re-run the same command after one."
