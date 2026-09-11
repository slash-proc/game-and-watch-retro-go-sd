#!/usr/bin/env bash
# gwemu_run.sh - Launch QEMU and poll logbuf via short-lived GDB connections.
# Detects faults, streams logs, kills QEMU on exception.
set -uo pipefail
cd "$(dirname "$0")/.."

ELF="build/gw_retro_go.elf"
LOG_FILE="retro_go.log"

# Get symbol addresses
LOGBUF_ADDR=0x$(arm-none-eabi-nm "$ELF" | awk '/\blogbuf$/{print $1}')
LOG_IDX_ADDR=0x$(arm-none-eabi-nm "$ELF" | awk '/\blog_idx$/{print $1}')

echo "logbuf=${LOGBUF_ADDR}  log_idx=${LOG_IDX_ADDR}"
> "$LOG_FILE"

# Launch QEMU without GDB attached
build/gwemu_bin -M gnw-h7b0 \
    -global gnw-h7b0-soc.bank1-image=build/qemu_bank1.bin \
    -global gnw-h7b0-soc.bank2-image=build/qemu_bank2.bin \
    -global gnw-h7b0-soc.extflash-image=build/extflash.bin \
    -drive if=sd,file=build/sdcard.img \
    -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
    -display gwemu \
    -s &
QEMU_PID=$!

cleanup() { kill $QEMU_PID 2>/dev/null || true; }
trap cleanup EXIT INT TERM

sleep 2

# Poll loop
LAST_DUMP=""
while kill -0 $QEMU_PID 2>/dev/null; do
    # Short-lived GDB: connect, halt, read, resume, disconnect
    OUT=$(arm-none-eabi-gdb "$ELF" -batch -nx \
        -ex "set confirm off" \
        -ex "set pagination off" \
        -ex "target extended-remote :1234" \
        -ex "printf \"PC=0x%08x\n\", \$pc" \
        -ex "printf \"IDX=%u\n\", *(unsigned int *)${LOG_IDX_ADDR}" \
        -ex "printf \"LOG_START>>>\"\n" \
        -ex "printf \"%s\", (char *)${LOGBUF_ADDR}" \
        -ex "printf \"<<<LOG_END\n\"" \
        2>/dev/null) || { sleep 1; continue; }

    PC=$(echo "$OUT" | grep -oP 'PC=\K0x[0-9a-f]+' 2>/dev/null || echo "unknown")
    IDX=$(echo "$OUT" | grep -oP 'IDX=\K[0-9]+' 2>/dev/null || echo "0")
    LOG_CONTENT=$(echo "$OUT" | sed -n 's/.*LOG_START>>>//;/<<<LOG_END/!{//!p;H;};/<<<LOG_END/{x;p;}' 2>/dev/null || echo "")
    # Simpler extraction:
    LOG_CONTENT=$(echo "$OUT" | sed -n '/LOG_START>>>/,/<<<LOG_END/p' | head -n -1 | tail -n +1 | sed 's/LOG_START>>>//' || echo "")

    # Print new content
    if [ -n "$LOG_CONTENT" ] && [ "$LOG_CONTENT" != "$LAST_DUMP" ]; then
        echo "$LOG_CONTENT" | tee "$LOG_FILE"
        LAST_DUMP="$LOG_CONTENT"
    fi

    # Check PC against fault handlers
    case "$PC" in
        *0800da78*|*0800da90*|*0800daa8*|*080233ca*)
            echo ""
            echo "!!! FAULT DETECTED at PC=${PC} !!!"
            echo ""
            # Full dump
            arm-none-eabi-gdb "$ELF" -batch -nx \
                -ex "set confirm off" \
                -ex "set pagination off" \
                -ex "target extended-remote :1234" \
                -ex "printf \"%s\", (char *)${LOGBUF_ADDR}" \
                -ex "bt" \
                -ex "info registers" \
                -ex "kill" \
                2>/dev/null | tee -a "$LOG_FILE"
            echo "=== QEMU killed ==="
            exit 1
            ;;
    esac

    sleep 1
done

echo "QEMU exited."
