#!/bin/bash
# Center function for text centering in columns
center() {
    # $1 = width, $2 = text
    local w=$1
    local t="$2"
    if ! [[ "$w" =~ ^[0-9]+$ ]]; then
        w=80
    fi

    local l=${#t}
    if (( l >= w )); then
        printf "%s" "$t"
    else
        local pad=$(( (w - l) / 2 ))
        local pad_r=$(( w - l - pad ))
        printf "%*s%s%*s" $pad "" "$t" $pad_r ""
    fi
}

# Print a line and clear to end-of-line to avoid leftover characters
print_line() {
    printf "%s" "$1"
    tput el
    printf "\n"
}

# Print centered title and clear to end-of-line
print_centered() {
    local cols=$(tput cols)
    local line
    line=$(center "$cols" "$1")
    printf "%s" "$line"
    tput el
    printf "\n"
}
# Usage:
#   cd <repo>/installation
#   bash ./calibrate.sh
#
# Calibration wizard for XPT2046 driver

# Resolve script directory and config path early (used by probe)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG="$SCRIPT_DIR/touch_config.txt"
SYSTEM_CONFIG="/etc/xpt2046/touch_config.txt"
INSTALL_MARKER_FILE="/etc/xpt2046/installed.marker"

# Calibration is only supported after running the installer.
if [ ! -f "$INSTALL_MARKER_FILE" ]; then
    echo "XPT2046_touchCalibrator is not installed on this system yet."
    echo "Run installation first:"
    echo "  cd \"$SCRIPT_DIR\" && bash ./install.sh"
    exit 1
fi

# Track whether probe ran to show post-probe messages
probe_done=0

# If we probe SPI device, keep it in-memory and only write it on explicit Save.
probed_spi_device=""


# Always restore cursor on exit; safely handle non-tty and stty errors
if [ -t 0 ]; then
    orig_stty=$(stty -g)
    trap 'if [ -t 0 ]; then stty "$orig_stty" 2>/dev/null || stty sane; fi; tput cnorm' EXIT
else
    trap 'tput cnorm' EXIT
fi
if [ -f "$HOME/.bash_profile" ]; then
    sed -i '/calibrate.sh/d' "$HOME/.bash_profile"
fi
echo "---Calibration Wizard---."

while true; do
    echo "Start calibration now? Type [yes/no]"
    read -r answer
    case "${answer,,}" in
        yes)
            break
            ;;
        no)
            echo "Calibration skipped."
            echo "You can run it later with:"
            echo "  cd \"$SCRIPT_DIR\" && bash ./xpt-calibrate.sh"
            echo "(or directly: bash ./calibrate.sh)"
            exit 0
            ;;
        *)
            echo "Invalid input. Please type yes or no."
            ;;
    esac
done

echo "Keep your finger at the center of the display, then press ENTER to start the 10-second SPI probe."
read -r

# Optional SPI probe: ask the user to keep a finger at the center for 10s
if [ -z "$CALIBRATION_RUNNING" ]; then
    # Run probe against a temporary config so we never modify an existing saved config.
    PROBE_CONFIG=""
    if command -v mktemp >/dev/null 2>&1; then
        PROBE_CONFIG="$(mktemp /tmp/xpt2046_probe.XXXXXX)"
    else
        PROBE_CONFIG="/tmp/xpt2046_probe_$$.txt"
        : > "$PROBE_CONFIG" 2>/dev/null || true
    fi

    # Pass CALIBRATION_RUNNING and explicit config path to driver.
    # Probe writes to a temp file; suppress any "saved" messaging to avoid confusion.
    # Also suppress any duplicate "hold finger" instructions (we show our own prompt above).
    # Stream output live (so the user sees per-device progress as it runs).
    if [ -x "$REPO_DIR/build/xpt2046_calibrator" ]; then
        if command -v stdbuf >/dev/null 2>&1; then
            CALIBRATION_RUNNING=1 TOUCH_CONFIG_PATH="$PROBE_CONFIG" \
                stdbuf -oL -eL "$REPO_DIR/build/xpt2046_calibrator" --probe 10 2>&1 | \
                grep -vi -e "saved" -e "press and hold finger" -e "hold your finger" || true
        else
            CALIBRATION_RUNNING=1 TOUCH_CONFIG_PATH="$PROBE_CONFIG" \
                "$REPO_DIR/build/xpt2046_calibrator" --probe 10 2>&1 | \
                grep -vi -e "saved" -e "press and hold finger" -e "hold your finger" || true
        fi
        probe_done=1
    else
        echo "No local driver found for SPI probe (skipping)."
    fi

    # If probe wrote a spi_device, keep it in memory; only write it on explicit Save.
    if [ -n "$PROBE_CONFIG" ] && [ -f "$PROBE_CONFIG" ]; then
        while IFS='=' read -r key value; do
            case "$key" in
                spi_device) probed_spi_device="$value";;
            esac
        done < "$PROBE_CONFIG"
        rm -f "$PROBE_CONFIG" >/dev/null 2>&1 || true
    fi

    if [ -n "$probed_spi_device" ]; then
        echo "SPI device probed: $probed_spi_device (will be saved only on 'Save & Exit')."
    fi
fi

invert_x=0
invert_y=0
swap_xy=0
min_x=0
max_x=4095
min_y=0
max_y=4095

# Advanced (defaults; overridden by config)
screen_w=800
screen_h=480
offset_x=0
offset_y=0
scale_x=1.0
scale_y=1.0
poll_us=100000
deadzone_left=0
deadzone_right=0
deadzone_top=0
deadzone_bottom=0
median_window=3
iir_alpha=0.20
press_threshold=120
release_threshold=80
max_delta_px=0
tap_max_ms=250
tap_max_move_px=12
drag_start_px=18

# Captured SPI device from config (if present)
spi_device=""

if [ -f "$CONFIG" ]; then
    while IFS='=' read -r key value; do
        case "$key" in
            invert_x) invert_x=$value;;
            invert_y) invert_y=$value;;
            swap_xy) swap_xy=$value;;
            min_x) min_x=$value;;
            max_x) max_x=$value;;
            min_y) min_y=$value;;
            max_y) max_y=$value;;
            spi_device) spi_device=$value;;

            screen_w) screen_w=$value;;
            screen_h) screen_h=$value;;
            offset_x) offset_x=$value;;
            offset_y) offset_y=$value;;
            poll_us) poll_us=$value;;
            scale_x) scale_x=$value;;
            scale_y) scale_y=$value;;
            deadzone_left) deadzone_left=$value;;
            deadzone_right) deadzone_right=$value;;
            deadzone_top) deadzone_top=$value;;
            deadzone_bottom) deadzone_bottom=$value;;
            median_window) median_window=$value;;
            iir_alpha) iir_alpha=$value;;
            press_threshold) press_threshold=$value;;
            release_threshold) release_threshold=$value;;
            max_delta_px) max_delta_px=$value;;
            tap_max_ms) tap_max_ms=$value;;
            tap_max_move_px) tap_max_move_px=$value;;
            drag_start_px) drag_start_px=$value;;
        esac
    done < "$CONFIG"
fi

# If we probed a spi_device and the saved config doesn't already have one, use the probed value.
if [ -z "$spi_device" ] && [ -n "$probed_spi_device" ]; then
    spi_device="$probed_spi_device"
fi

set_advanced_defaults() {
    screen_w=800
    screen_h=480
    offset_x=0
    offset_y=0
    poll_us=100000
    scale_x=1.0
    scale_y=1.0
    deadzone_left=0
    deadzone_right=0
    deadzone_top=0
    deadzone_bottom=0
    median_window=3
    iir_alpha=0.20
    press_threshold=120
    release_threshold=80
    max_delta_px=0
    tap_max_ms=250
    tap_max_move_px=12
    drag_start_px=18
}

run_with_env() {
    # Usage: run_with_env <command...>
    env \
        CALIBRATION_RUNNING=1 \
        XPT_INVERT_X="$invert_x" XPT_INVERT_Y="$invert_y" XPT_SWAP_XY="$swap_xy" \
        XPT_MIN_X="$min_x" XPT_MAX_X="$max_x" XPT_MIN_Y="$min_y" XPT_MAX_Y="$max_y" \
        XPT_SCREEN_W="$screen_w" XPT_SCREEN_H="$screen_h" \
        XPT_OFFSET_X="$offset_x" XPT_OFFSET_Y="$offset_y" \
        XPT_POLL_US="$poll_us" \
        XPT_SCALE_X="$scale_x" XPT_SCALE_Y="$scale_y" \
        XPT_DEADZONE_LEFT="$deadzone_left" XPT_DEADZONE_RIGHT="$deadzone_right" \
        XPT_DEADZONE_TOP="$deadzone_top" XPT_DEADZONE_BOTTOM="$deadzone_bottom" \
        XPT_MEDIAN_WINDOW="$median_window" XPT_IIR_ALPHA="$iir_alpha" \
        XPT_PRESS_THRESHOLD="$press_threshold" XPT_RELEASE_THRESHOLD="$release_threshold" \
        XPT_MAX_DELTA_PX="$max_delta_px" \
        XPT_TAP_MAX_MS="$tap_max_ms" XPT_TAP_MAX_MOVE_PX="$tap_max_move_px" XPT_DRAG_START_PX="$drag_start_px" \
        TOUCH_CONFIG_PATH="$CONFIG" \
        "$@"
}

run_with_env_bg() {
    # Usage: run_with_env_bg <command...>
    # Runs with the same env vars as run_with_env, but in background.
    if command -v setsid >/dev/null 2>&1; then
        setsid env \
            CALIBRATION_RUNNING=1 \
            XPT_INVERT_X="$invert_x" XPT_INVERT_Y="$invert_y" XPT_SWAP_XY="$swap_xy" \
            XPT_MIN_X="$min_x" XPT_MAX_X="$max_x" XPT_MIN_Y="$min_y" XPT_MAX_Y="$max_y" \
            XPT_SCREEN_W="$screen_w" XPT_SCREEN_H="$screen_h" \
            XPT_OFFSET_X="$offset_x" XPT_OFFSET_Y="$offset_y" \
            XPT_POLL_US="$poll_us" \
            XPT_SCALE_X="$scale_x" XPT_SCALE_Y="$scale_y" \
            XPT_DEADZONE_LEFT="$deadzone_left" XPT_DEADZONE_RIGHT="$deadzone_right" \
            XPT_DEADZONE_TOP="$deadzone_top" XPT_DEADZONE_BOTTOM="$deadzone_bottom" \
            XPT_MEDIAN_WINDOW="$median_window" XPT_IIR_ALPHA="$iir_alpha" \
            XPT_PRESS_THRESHOLD="$press_threshold" XPT_RELEASE_THRESHOLD="$release_threshold" \
            XPT_MAX_DELTA_PX="$max_delta_px" \
            XPT_TAP_MAX_MS="$tap_max_ms" XPT_TAP_MAX_MOVE_PX="$tap_max_move_px" XPT_DRAG_START_PX="$drag_start_px" \
            TOUCH_CONFIG_PATH="$CONFIG" \
            "$@" &
    else
        run_with_env "$@" &
    fi
    echo $!
}

stop_bg() {
    # Stop background job and any children, preventing terminal spam after returning to menu.
    local pid="$1"
    if [ -z "$pid" ]; then
        return
    fi

    # If launched via setsid, the process is the leader of its own session/process-group.
    # Killing the process-group is the most reliable way to stop any lingering output.
    kill -TERM -- -"$pid" 2>/dev/null || true
    kill -TERM "$pid" 2>/dev/null || true
    sleep 0.1
    kill -KILL -- -"$pid" 2>/dev/null || true
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# After probe: show selected SPI and launching message, then pause 5s
if [ "$probe_done" = "1" ]; then
    if [ -n "$spi_device" ]; then
		
        echo "Selected SPI device: $spi_device"
    else
        echo "Selected SPI device: auto-detect will run."
    fi
    echo "Launching the Calibration Wizard..."
    sleep 5
fi

tput civis
tput clear
if [ -t 0 ]; then
    stty -echo -icanon time 0 min 1
fi

selected=1

draw_header() {
    # Draw a left-aligned white rectangle around the title with 2 spaces padding, aligned with options
    local title="$1"
    local pad=2
    printf "   "
    tput setab 7; tput setaf 0
    printf "%s" "$(printf "%${pad}s" "")${title}$(printf "%${pad}s" "")"
    tput sgr0
    tput el
    printf "\n"
}

render_option() {
    local idx=$1
    local text="$2"
    if [ "$selected" -eq "$idx" ]; then
        printf "   "
        tput setab 7; tput setaf 0
        printf "%s" "$text"
        tput sgr0
        tput el
        printf "\n"
    else
        printf "   %s" "$text"
        tput el
        printf "\n"
    fi
}

draw_menu() {
    tput cup 0 0
    tput ed
    print_centered "XPT2046 Touch Calibration Wizard"
    print_line ""
    print_line "Use Up/Down to navigate, Enter to select."
    print_line "Press 's' to save, 'q' to exit without saving."
    print_line ""
    # BASIC section header and options
    draw_header "BASIC SETTINGS"
    render_option 1 "invert_x: $invert_x"
    render_option 2 "invert_y: $invert_y"
    render_option 3 "swap_xy: $swap_xy"
    render_option 4 "min_x: $min_x"
    render_option 5 "max_x: $max_x"
    render_option 6 "min_y: $min_y"
    render_option 7 "max_y: $max_y"
    print_line ""  # visual blank line, not an option
    render_option 8 "RAW Test (terminal)"
    render_option 9 "GUI Test (SDL2)"

    print_line ""
    print_line ""

    # Footer actions
    print_line ""
    render_option 10 "[Save & Exit]"
    render_option 11 "[Exit without saving]"
    render_option 12 "[Advanced Settings]"
    # Clear any remaining content below the menu
    tput ed
}

# Separate Advanced Settings screen (function must be defined before use)
advanced_screen() {
    tput smcup
    local adv_selected=1
    local saved_stty=""
    if [ -t 0 ]; then
        saved_stty=$(stty -g)
        stty sane
    fi
    local done=0
    while [ $done -eq 0 ]; do
        tput cup 0 0; tput ed
        print_centered "Advanced Settings"
        print_line ""
        draw_header "ADVANCED SETTINGS"
        print_line ""

        # Ranges shown inline (requested)
        local rows=25
        local i
        for i in $(seq 1 $rows); do
            local label=""
            case $i in
                1) label="[Advanced defaults]";;
                2) label="screen_w: $screen_w (1..4096)";;
                3) label="screen_h: $screen_h (1..4096)";;
                4) label="offset_x: $offset_x (-2000..2000)";;
                5) label="offset_y: $offset_y (-2000..2000)";;
                6) label="scale_x: $scale_x (0.01..10.0)";;
                7) label="scale_y: $scale_y (0.01..10.0)";;
                8) label="deadzone_left: $deadzone_left (0..1000)";;
                9) label="deadzone_right: $deadzone_right (0..1000)";;
                10) label="deadzone_top: $deadzone_top (0..1000)";;
                11) label="deadzone_bottom: $deadzone_bottom (0..1000)";;
                12) label="median_window: $median_window (0|3|5)";;
                13) label="iir_alpha: $iir_alpha (0..1)";;
                14) label="press_threshold: $press_threshold (0..4095)";;
                15) label="release_threshold: $release_threshold (0..press)";;
                16) label="max_delta_px: $max_delta_px (0 disables)";;
                17) label="poll_us: $poll_us (1000..1000000)";;
                18) label="tap_max_ms: $tap_max_ms (0..2000)";;
                19) label="tap_max_move_px: $tap_max_move_px (0..200)";;
                20) label="drag_start_px: $drag_start_px (0..400)";;
                21) label="Advanced RAW Test (terminal)";;
                22) label="Advanced GUI Test (SDL2)";;
                23) label="[Save & Exit]";;
                24) label="[Exit without saving]";;
                25) label="[Back]";;
            esac

            if [ "$adv_selected" -eq "$i" ]; then
                printf "   "
                tput setab 7; tput setaf 0
                printf "%s" "$label"
                tput sgr0
                tput el
                printf "\n"
            else
                printf "   %s" "$label"
                tput el
                printf "\n"
            fi
        done

        tput ed

        # Read key
        read -rsn1 key
        if [[ $key == $'\x1b' ]]; then
            read -rsn2 -t 0.05 rest
            if [[ $rest == "[A" || $rest == "OA" ]]; then
                adv_selected=$((adv_selected-1))
            elif [[ $rest == "[B" || $rest == "OB" ]]; then
                adv_selected=$((adv_selected+1))
            fi
        elif [[ -z "$key" ]]; then
            case $adv_selected in
                1)
                    set_advanced_defaults
                    ;;
                2) read -p "Enter screen_w: " screen_w;;
                3) read -p "Enter screen_h: " screen_h;;
                4) read -p "Enter offset_x: " offset_x;;
                5) read -p "Enter offset_y: " offset_y;;
                6) read -p "Enter scale_x (float): " scale_x;;
                7) read -p "Enter scale_y (float): " scale_y;;
                8) read -p "Enter deadzone_left: " deadzone_left;;
                9) read -p "Enter deadzone_right: " deadzone_right;;
                10) read -p "Enter deadzone_top: " deadzone_top;;
                11) read -p "Enter deadzone_bottom: " deadzone_bottom;;
                12) read -p "Enter median_window (0/3/5): " median_window;;
                13) read -p "Enter iir_alpha (0..1): " iir_alpha;;
                14) read -p "Enter press_threshold (0..4095): " press_threshold;;
                15) read -p "Enter release_threshold (0..press): " release_threshold;;
                16) read -p "Enter max_delta_px (0 disables): " max_delta_px;;
                17) read -p "Enter poll_us (1000..1000000): " poll_us;;
                18) read -p "Enter tap_max_ms: " tap_max_ms;;
                19) read -p "Enter tap_max_move_px: " tap_max_move_px;;
                20) read -p "Enter drag_start_px: " drag_start_px;;
                21)
                    # Advanced RAW Test
                    (
                        tput smcup
                        tput clear
                        echo "Advanced RAW Test (terminal)..."
                        echo "Press Ctrl+C to return."

                        saved_stty=""
                        if [ -t 0 ]; then
                            saved_stty=$(stty -g)
                            stty sane
                        fi

                        pid=""
                        watcher_pid=""
                        cleanup_adv_raw() {
                            local code=${1:-0}
                            if [ -n "$watcher_pid" ]; then
                                kill -TERM "$watcher_pid" 2>/dev/null || true
                                wait "$watcher_pid" 2>/dev/null || true
                                watcher_pid=""
                            fi
                            if [ -n "$pid" ]; then
                                kill -TERM "$pid" 2>/dev/null || true
                                sleep 0.1
                                kill -KILL "$pid" 2>/dev/null || true
                                wait "$pid" 2>/dev/null || true
                            fi
                            tput rmcup
                            if [ -t 0 ] && [ -n "$saved_stty" ]; then
                                stty "$saved_stty" 2>/dev/null || stty -echo -icanon time 0 min 1
                            fi
                            tput clear
                            exit "$code"
                        }
                        trap 'cleanup_adv_raw 130' INT TERM

                        if [ -x "$REPO_DIR/build/xpt2046_calibrator" ]; then
                            run_with_env "$REPO_DIR/build/xpt2046_calibrator" --advanced_raw --invert_x "$invert_x" --invert_y "$invert_y" --swap_xy "$swap_xy" &
                            pid=$!
                        else
                            echo "Advanced RAW test not available (no local binary)."
                        fi

                        if [ -n "$pid" ]; then
                            (
                                if [ -r /dev/tty ]; then
                                    read -r < /dev/tty
                                else
                                    read -r
                                fi
                                kill -INT "$pid" 2>/dev/null || true
                            ) &
                            watcher_pid=$!

                            wait "$pid" 2>/dev/null || true
                        else
                            if [ -r /dev/tty ]; then
                                read -r < /dev/tty
                            else
                                read -r
                            fi
                        fi
                        cleanup_adv_raw 0
                    )
                    ;;
                22)
                    # Advanced GUI Test
                    tput clear
                    echo "Launching Advanced GUI Test (SDL2)..."
                    tput smcup
                    if [ -t 0 ]; then
                        saved_stty=$(stty -g)
                        stty sane
                    fi
                    bin=""
                    if [ -x "$REPO_DIR/build/xpt_advanced_gui_sdl2" ]; then
                        bin="$REPO_DIR/build/xpt_advanced_gui_sdl2"
                    fi
                    if [ -n "$bin" ]; then
                        if [ -n "$DISPLAY" ]; then
                            run_with_env SDL_VIDEO_X11_XSHM=0 "$bin"
                        else
                            run_with_env SDL_VIDEODRIVER=kmsdrm "$bin" || run_with_env SDL_VIDEODRIVER=fbcon SDL_FBDEV=/dev/fb0 "$bin"
                        fi
                    else
                        echo "Advanced SDL2 GUI binary not found. Build the project first (cmake + make)."
                        echo "Press Enter to return."
                        read -r
                    fi
                    tput rmcup
                    if [ -t 0 ] && [ -n "$saved_stty" ]; then
                        stty "$saved_stty" 2>/dev/null || stty -echo -icanon time 0 min 1
                    fi
                    tput clear
                    ;;
                23)
                    selected=10; handle_action; done=1;;
                24)
                    selected=11; handle_action; done=1;;
                25)
                    done=1;;
                *) :;;
            esac
        elif [[ $key == 'q' || $key == $'\e' ]]; then
            done=1
        elif [[ $key == 'k' ]]; then
            adv_selected=$((adv_selected-1))
        elif [[ $key == 'j' ]]; then
            adv_selected=$((adv_selected+1))
        fi
        if [ $adv_selected -lt 1 ]; then adv_selected=1; fi
        if [ $adv_selected -gt 25 ]; then adv_selected=25; fi
    done
    tput rmcup
    if [ -t 0 ] && [ -n "$saved_stty" ]; then
        stty "$saved_stty" 2>/dev/null || stty -echo -icanon time 0 min 1
    fi
    tput clear
}

handle_action() {
    case $selected in
        1) invert_x=$((1-invert_x));;
        2) invert_y=$((1-invert_y));;
        3) swap_xy=$((1-swap_xy));;
        4) read -p "Enter min_x: " min_x;;
        5) read -p "Enter max_x: " max_x;;
        6) read -p "Enter min_y: " min_y;;
        7) read -p "Enter max_y: " max_y;;
        8)
            (
                # Use alternate screen to avoid mixing with menu
                tput smcup
                tput clear

                echo "RAW Test (terminal)..."
                echo "Press Ctrl+C to return to the menu."

                saved_stty=""
                if [ -t 0 ]; then
                    saved_stty=$(stty -g)
                    # Use canonical mode here so Enter works reliably over SSH.
                    # Keep ISIG so Ctrl+C triggers the trap.
                    stty sane
                fi

                pid=""
                watcher_pid=""
                cleanup_raw() {
                    local code=${1:-0}
                    if [ -n "$watcher_pid" ]; then
                        kill -TERM "$watcher_pid" 2>/dev/null || true
                        wait "$watcher_pid" 2>/dev/null || true
                        watcher_pid=""
                    fi
                    if [ -n "$pid" ]; then
                        kill -TERM "$pid" 2>/dev/null || true
                        sleep 0.1
                        kill -KILL "$pid" 2>/dev/null || true
                        wait "$pid" 2>/dev/null || true
                    fi
                    tput rmcup
                    if [ -t 0 ] && [ -n "$saved_stty" ]; then
                        stty "$saved_stty" 2>/dev/null || stty -echo -icanon time 0 min 1
                    fi
                    tput clear
                    exit "$code"
                }
                trap 'cleanup_raw 130' INT TERM

                if [ -x "$REPO_DIR/build/xpt2046_calibrator" ]; then
                    run_with_env "$REPO_DIR/build/xpt2046_calibrator" --invert_x "$invert_x" --invert_y "$invert_y" --swap_xy "$swap_xy" &
                    pid=$!
                elif [ -x "$REPO_DIR/1Tools/runtest.sh" ]; then
                    XPT_MIN_X="$min_x" XPT_MAX_X="$max_x" XPT_MIN_Y="$min_y" XPT_MAX_Y="$max_y" \
                    "$REPO_DIR/1Tools/runtest.sh" "$invert_x" "$invert_y" "$swap_xy" &
                    pid=$!
                else
                    echo "RAW test not available (no local binary or script)."
                fi

                if [ -n "$pid" ]; then
                    # Wait for Enter, then SIGINT the test so it exits cleanly.
                    (
                        if [ -r /dev/tty ]; then
                            read -r < /dev/tty
                        else
                            read -r
                        fi
                        kill -INT "$pid" 2>/dev/null || true
                    ) &
                    watcher_pid=$!

                    wait "$pid" 2>/dev/null || true
                else
                    # Nothing running; just wait for Enter.
                    if [ -r /dev/tty ]; then
                        read -r < /dev/tty
                    else
                        read -r
                    fi
                fi

                cleanup_raw 0
            )
            ;;
        9)
            # Use alternate screen
            tput smcup
            tput clear
            echo "Launching GUI Test (SDL2)..."

            # Restore sane input for child app
            saved_stty=""
            if [ -t 0 ]; then
                saved_stty=$(stty -g)
                stty sane
            fi

            bin=""
            if [ -x "$REPO_DIR/build/xpt_basic_gui_sdl2" ]; then
                bin="$REPO_DIR/build/xpt_basic_gui_sdl2"
            fi

            if [ -n "$bin" ]; then
                if [ -n "$DISPLAY" ]; then
                    run_with_env SDL_VIDEO_X11_XSHM=0 "$bin"
                else
                    run_with_env SDL_VIDEODRIVER=kmsdrm "$bin" || run_with_env SDL_VIDEODRIVER=fbcon SDL_FBDEV=/dev/fb0 "$bin"
                fi
            else
                echo "SDL2 GUI binary not found. Build the project first (cmake + make)."
                echo "Press Enter to return."
                read -r
            fi

            tput rmcup
            if [ -t 0 ] && [ -n "$saved_stty" ]; then
                stty "$saved_stty" 2>/dev/null || stty -echo -icanon time 0 min 1
            fi
            tput clear
            ;;
        10)
            echo "invert_x=$invert_x" > "$CONFIG"
            echo "invert_y=$invert_y" >> "$CONFIG"
            echo "swap_xy=$swap_xy" >> "$CONFIG"
            echo "min_x=$min_x" >> "$CONFIG"
            echo "max_x=$max_x" >> "$CONFIG"
            echo "min_y=$min_y" >> "$CONFIG"
            echo "max_y=$max_y" >> "$CONFIG"

            echo "screen_w=$screen_w" >> "$CONFIG"
            echo "screen_h=$screen_h" >> "$CONFIG"
            echo "offset_x=$offset_x" >> "$CONFIG"
            echo "offset_y=$offset_y" >> "$CONFIG"
            echo "poll_us=$poll_us" >> "$CONFIG"
            echo "scale_x=$scale_x" >> "$CONFIG"
            echo "scale_y=$scale_y" >> "$CONFIG"
            echo "deadzone_left=$deadzone_left" >> "$CONFIG"
            echo "deadzone_right=$deadzone_right" >> "$CONFIG"
            echo "deadzone_top=$deadzone_top" >> "$CONFIG"
            echo "deadzone_bottom=$deadzone_bottom" >> "$CONFIG"
            echo "median_window=$median_window" >> "$CONFIG"
            echo "iir_alpha=$iir_alpha" >> "$CONFIG"
            echo "press_threshold=$press_threshold" >> "$CONFIG"
            echo "release_threshold=$release_threshold" >> "$CONFIG"
            echo "max_delta_px=$max_delta_px" >> "$CONFIG"
            echo "tap_max_ms=$tap_max_ms" >> "$CONFIG"
            echo "tap_max_move_px=$tap_max_move_px" >> "$CONFIG"
            echo "drag_start_px=$drag_start_px" >> "$CONFIG"
            # (Intentionally no message about the repo-local config path.)

            # Also persist for system services (e.g., xpt-uinputd) which typically run as root.
            if command -v sudo >/dev/null 2>&1; then
                sudo mkdir -p "$(dirname "$SYSTEM_CONFIG")" >/dev/null 2>&1 || true
                sudo cp "$CONFIG" "$SYSTEM_CONFIG" >/dev/null 2>&1 || true
                sudo chmod 0644 "$SYSTEM_CONFIG" >/dev/null 2>&1 || true
                if [[ -f "$SYSTEM_CONFIG" ]]; then
                    echo "Saved to: $SYSTEM_CONFIG"
                fi
            fi
            echo "Calibration complete."
            exit 0
            ;;
        11)
            echo "Exit without saving: no changes were written."
            exit 0
            ;;
        12)
            # Open advanced settings in a separate window
            advanced_screen
            # Return to main screen at Basic section
            selected=1
            ;;
        *) :;;
    esac

}

while true; do
    draw_menu
    read -rsn1 key
    if [[ $key == $'\x1b' ]]; then
        read -rsn2 -t 0.05 rest
        if [[ $rest == "[A" || $rest == "OA" ]]; then
            selected=$((selected-1))
        elif [[ $rest == "[B" || $rest == "OB" ]]; then
            selected=$((selected+1))
        fi
    elif [[ -z "$key" ]]; then
        handle_action
    elif [[ $key == 'q' ]]; then
        selected=11
        handle_action
    elif [[ $key == 's' ]]; then
        selected=10
        handle_action
    elif [[ $key == 'k' ]]; then
        selected=$((selected-1))
    elif [[ $key == 'j' ]]; then
        selected=$((selected+1))
    fi

    if [ $selected -lt 1 ]; then selected=1; fi
    if [ $selected -gt 12 ]; then selected=12; fi
    prev_selected=$selected
done
