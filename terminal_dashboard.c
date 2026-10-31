#!/usr/bin/env bash

cd "$(dirname "$0")" || exit 1

API="http://localhost:8000/api/data"
CPU_WORKLOAD="./workloads/cpu_workload"
MEM_WORKLOAD="./workloads/memory_workload"

CPU_PID=""
MEM_PID=""

cleanup() {
    [[ -n "$CPU_PID" ]] && kill "$CPU_PID" 2>/dev/null
    [[ -n "$MEM_PID" ]] && kill "$MEM_PID" 2>/dev/null
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

pause_screen() {
    echo
    read -r -p "Press Enter to return to the main menu..."
}

show_system_info() {
    echo
    curl -fsS "$API" | jq '{
        system: .sys,
        processes: (.procs | length)
    }' 2>/dev/null || echo "Unable to read API data."
    pause_screen
}

launch_workload() {
    local type="$1"
    local default_path="$2"
    local input_path

    read -r -p "Workload executable [$default_path]: " input_path
    input_path="${input_path:-$default_path}"

    if [[ "$input_path" != /* ]]; then
        input_path="$PWD/${input_path#./}"
    fi

    if [[ ! -f "$input_path" ]]; then
        echo "File not found: $input_path"
        pause_screen
        return
    fi

    chmod +x "$input_path"

    if [[ "$type" == "cpu" ]]; then
        if [[ -n "$CPU_PID" ]] && kill -0 "$CPU_PID" 2>/dev/null; then
            echo "CPU workload is already running (PID $CPU_PID)."
        else
            "$input_path" &
            CPU_PID=$!
            echo "CPU workload started. PID: $CPU_PID"
        fi
    else
        if [[ -n "$MEM_PID" ]] && kill -0 "$MEM_PID" 2>/dev/null; then
            echo "Memory workload is already running (PID $MEM_PID)."
        else
            "$input_path" &
            MEM_PID=$!
            echo "Memory workload started. PID: $MEM_PID"
        fi
    fi

    pause_screen
}

show_workloads() {
    while true; do
        clear
        echo "========== LIVE WORKLOAD MONITOR =========="
        echo "CPU workload PID: ${CPU_PID:-Not launched}"
        echo "Memory workload PID: ${MEM_PID:-Not launched}"
        echo
        echo "1. Refresh monitor"
        echo "2. Return to main menu"
        echo "3. Stop workloads and return to main menu"
        echo "4. Exit dashboard"
        echo

        read -r -p "Choose an option: " choice

        case "$choice" in
            1) continue ;;
            2) return ;;
            3)
                [[ -n "$CPU_PID" ]] && kill "$CPU_PID" 2>/dev/null
                [[ -n "$MEM_PID" ]] && kill "$MEM_PID" 2>/dev/null
                CPU_PID=""
                MEM_PID=""
                echo "Workloads stopped."
                return
                ;;
            4) exit 0 ;;
            *) echo "Invalid choice. Enter 1, 2, 3, or 4." ;;
        esac
    done
}

stop_workloads() {
    [[ -n "$CPU_PID" ]] && kill "$CPU_PID" 2>/dev/null
    [[ -n "$MEM_PID" ]] && kill "$MEM_PID" 2>/dev/null

    CPU_PID=""
    MEM_PID=""

    echo "Stop signal sent to dashboard-launched workloads."
    pause_screen
}

main_menu() {
    while true; do
        clear
        echo "=========================================="
        echo "       KERNEL MONITOR DASHBOARD"
        echo "=========================================="
        echo "1. View system information"
        echo "2. Launch CPU workload"
        echo "3. Launch memory workload"
        echo "4. Launch both workloads"
        echo "5. Live workload monitor"
        echo "6. Stop workloads"
        echo "7. Exit"
        echo "=========================================="

        read -r -p "Choose an option: " choice

        case "$choice" in
            1) show_system_info ;;
            2) launch_workload cpu "$CPU_WORKLOAD" ;;
            3) launch_workload memory "$MEM_WORKLOAD" ;;
            4)
                launch_workload cpu "$CPU_WORKLOAD"
                launch_workload memory "$MEM_WORKLOAD"
                ;;
            5) show_workloads ;;
            6) stop_workloads ;;
            7) exit 0 ;;
            *)
                echo "Invalid choice. Please select 1-7."
                sleep 1
                ;;
        esac
    done
}

if ! command -v jq >/dev/null 2>&1; then
    echo "The jq utility is required. Install it with: sudo apt install jq"
    exit 1
fi

main_menu