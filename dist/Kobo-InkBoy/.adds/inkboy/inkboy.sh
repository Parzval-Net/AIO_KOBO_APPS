#!/bin/sh
# ==============================================================================
# Ink-boy Launcher for Kobo Libra Colour (MT8113T) & Kobo Clara HD (i.MX6ULL)
# Triggered via NickelMenu (/bin/sh /mnt/onboard/.adds/inkboy/inkboy.sh).
# Copies the ARMhf binary to /tmp/inkboy (bypassing FAT32 noexec/fmask on
# /mnt/onboard), pauses Nickel (SIGSTOP), runs Ink-boy with real FBInk, and
# resumes Nickel (SIGCONT) on exit, or reboots cleanly if exit_mode = reboot.
#
# The session log lives on the user partition (.adds/inkboy/inkboy.log) so it
# survives a reboot and can be read over USB; the previous one is kept as
# inkboy.prev.log.
# ==============================================================================

INKBOY_DIR="/mnt/onboard/.adds/inkboy"
INKBOY_SRC="${INKBOY_DIR}/inkboy"
INKBOY_TMP="/tmp/inkboy"
ROMS_DIR="${INKBOY_DIR}/roms"
CONFIG="${INKBOY_DIR}/config.ini"
LOG_FILE="${INKBOY_DIR}/inkboy.log"

mkdir -p "${ROMS_DIR}" "${INKBOY_DIR}/saves"
[ -f "${LOG_FILE}" ] && mv -f "${LOG_FILE}" "${INKBOY_DIR}/inkboy.prev.log"

log() {
    echo "[$(date +%H:%M:%S)] $*" >> "${LOG_FILE}"
}

# Read "key = value" from config.ini (last occurrence wins, comments ignored)
cfg() {
    [ -f "${CONFIG}" ] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^[:space:]#;]*\).*/\1/p" "${CONFIG}" | tail -n 1
}

proc_state() {
    pid="$(pidof "$1" 2>/dev/null | cut -d ' ' -f 1)"
    if [ -z "${pid}" ]; then
        echo "absent"
    else
        echo "pid ${pid} state $(cut -d ' ' -f 3 "/proc/${pid}/stat" 2>/dev/null)"
    fi
}

EXIT_MODE="$(cfg exit_mode)"
[ -n "${EXIT_MODE}" ] || EXIT_MODE="resume"
PAUSE_WATCHDOG="$(cfg pause_watchdog)"
[ -n "${PAUSE_WATCHDOG}" ] || PAUSE_WATCHDOG="no"

CLEANED_UP=0
cleanup() {
    [ "${CLEANED_UP}" = "1" ] && return
    CLEANED_UP=1
    sync
    rm -f "${INKBOY_TMP}" 2>/dev/null || true

    if [ "${EXIT_MODE}" = "reboot" ]; then
        log "exit_mode=reboot: rebooting cleanly"
        sync
        reboot
        return
    fi

    log "resuming nickel (before: $(proc_state nickel))"
    killall -CONT nickel >/dev/null 2>&1 || true

    # Detached monitor: records how Nickel (and its watchdog) behave after the hand-back.
    # Each line is synced, so if the device hangs or reboots the last state is still on disk.
    (
        i=0
        while [ "${i}" -lt 15 ]; do
            sleep 1
            i=$((i + 1))
            if [ "${i}" = "3" ] && [ "${PAUSE_WATCHDOG}" = "yes" ]; then
                killall -CONT sickel >/dev/null 2>&1 || true
                log "resumed sickel"
            fi
            log "t+${i}s nickel: $(proc_state nickel) | sickel: $(proc_state sickel)"
            if [ "${i}" = "2" ] || [ "${i}" = "10" ]; then
                dmesg 2>/dev/null | tail -n 25 >> "${LOG_FILE}"
            fi
            sync
        done
        if [ -z "$(pidof nickel 2>/dev/null)" ]; then
            # Nickel is gone: the screen would stay frozen, so reboot cleanly instead of hanging
            log "nickel not running after resume: rebooting cleanly"
            sync
            reboot
        fi
        log "monitor finished"
        sync
    ) </dev/null >/dev/null 2>&1 &
}
trap cleanup EXIT INT TERM HUP QUIT

log "=== Ink-boy session started: $(date) ==="
log "firmware: $(cut -d ',' -f 3 /mnt/onboard/.kobo/version 2>/dev/null) | exit_mode=${EXIT_MODE} pause_watchdog=${PAUSE_WATCHDOG}"
log "before launch nickel: $(proc_state nickel) | sickel: $(proc_state sickel) | bluetoothd: $(proc_state bluetoothd)"

if [ ! -f "${INKBOY_SRC}" ]; then
    log "[ERROR] Binary not found at ${INKBOY_SRC}"
    exit 1
fi

# Copy binary to /tmp (tmpfs in RAM) so FAT32 mount options (noexec/fmask) never block execution
cp -f "${INKBOY_SRC}" "${INKBOY_TMP}"
chmod 755 "${INKBOY_TMP}"

if [ ! -x "${INKBOY_TMP}" ]; then
    log "[ERROR] /tmp/inkboy is not executable"
    exit 1
fi

# Pause the watchdog first (opt-in), then Nickel, only after /tmp/inkboy is staged
if [ "${PAUSE_WATCHDOG}" = "yes" ]; then
    killall -STOP sickel >/dev/null 2>&1 || true
fi
killall -STOP nickel >/dev/null 2>&1 || true
log "paused nickel: $(proc_state nickel)"

"${INKBOY_TMP}" --no-suspend-nickel --roms-dir "${ROMS_DIR}" "$@" >> "${LOG_FILE}" 2>&1
log "ink-boy exited with code $?"

exit 0
