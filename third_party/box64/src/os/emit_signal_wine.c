/*
 * Copyright 2022-2025 André Zwing
 * Copyright 2023 Alexandre Julliard
 */
#include <windows.h>
#include <winternl.h>

#include "x64_signals.h"
#include "x64emu.h"
#include "debug.h"
#include "custommem.h"
#include "os.h"
#include "emu/x64int_private.h"

void EmitSignal(x64emu_t* emu, int sig, void* addr, int code)
{
    EXCEPTION_RECORD rec;

    switch (sig) {
        case X64_SIGILL:
            printf_log(LOG_DEBUG, "SIGILL at %p with code %d\n", addr, code);
            rec.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
            break;
        case X64_SIGSEGV:
            printf_log(LOG_DEBUG, "SIGSEGV at %p with code %d\n", addr, code);
            rec.ExceptionCode = STATUS_ACCESS_VIOLATION;
            break;
        default:
            printf_log(LOG_INFO, "Warning, unknown signal %d at %p with code %d\n", sig, addr, code);
            rec.ExceptionCode = STATUS_ACCESS_VIOLATION;
            break;
    }
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.ExceptionRecord = NULL;
    rec.ExceptionAddress = addr;
    rec.NumberParameters = 0;
    RtlRaiseException(&rec);
}

void CheckExec(x64emu_t* emu, uintptr_t addr)
{
}

void EmitInterruption(x64emu_t* emu, int num, void* addr)
{
    printf_log(LOG_DEBUG, "Emit Interruption 0x%x at addr=%p\n", num, addr);
    x86Int(emu, num);
}

void EmitDiv0(x64emu_t* emu, void* addr, int code)
{
    EXCEPTION_RECORD rec;
    rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.ExceptionRecord = NULL;
    rec.ExceptionAddress = addr;
    rec.NumberParameters = 0;
    RtlRaiseException(&rec);
}

void EmuInt3(void* emu, void* addr)
{
    x64Int3(emu, addr);
}
