"""Runs CoD3.exe under a minimal debugger and reports exceptions.

A vectored handler inside the process sees most faults, but not the one
that ends the process without dispatch: this sees every one from outside,
first and second chance, with the thread, the registers and the host frames
relative to the executable, so llvm-symbolizer can name them:

    python scripts/debugrun.py [seconds] [-- extra env assignments K=V ...]

The game's stdout goes to debugrun.out beside this script's temp files.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time

k32 = ctypes.windll.kernel32

DEBUG_ONLY_THIS_PROCESS = 0x2
CREATE_NEW_CONSOLE = 0x10
EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
OUTPUT_DEBUG_STRING_EVENT = 8
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
INFINITE = 0xFFFFFFFF
CONTEXT_FULL = 0x10000B
THREAD_GET_CONTEXT = 0x8
THREAD_QUERY_INFORMATION = 0x40


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [("ExceptionCode", wt.DWORD), ("ExceptionFlags", wt.DWORD),
                ("ExceptionRecord", ctypes.c_void_p), ("ExceptionAddress", ctypes.c_void_p),
                ("NumberParameters", wt.DWORD), ("ExceptionInformation", ctypes.c_ulonglong * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wt.DWORD)]


class CREATE_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wt.HANDLE), ("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("lpBaseOfImage", ctypes.c_void_p), ("dwDebugInfoFileOffset", wt.DWORD),
                ("nDebugInfoSize", wt.DWORD), ("lpThreadLocalBase", ctypes.c_void_p),
                ("lpStartAddress", ctypes.c_void_p), ("lpImageName", ctypes.c_void_p),
                ("fUnicode", wt.WORD)]


class LOAD_DLL_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wt.HANDLE), ("lpBaseOfDll", ctypes.c_void_p),
                ("dwDebugInfoFileOffset", wt.DWORD), ("nDebugInfoSize", wt.DWORD),
                ("lpImageName", ctypes.c_void_p), ("fUnicode", wt.WORD)]


class EXIT_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("dwExitCode", wt.DWORD)]


class DEBUG_EVENT_UNION(ctypes.Union):
    _fields_ = [("Exception", EXCEPTION_DEBUG_INFO),
                ("CreateProcessInfo", CREATE_PROCESS_DEBUG_INFO),
                ("LoadDll", LOAD_DLL_DEBUG_INFO),
                ("ExitProcess", EXIT_PROCESS_DEBUG_INFO),
                ("raw", ctypes.c_byte * 164)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD), ("u", DEBUG_EVENT_UNION)]


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
                ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
                ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD), ("lpReserved2", ctypes.c_void_p),
                ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


# The x64 CONTEXT, 1232 bytes, 16 byte aligned; only Rip and Rsp are read.
class CONTEXT(ctypes.Structure):
    _align_ = 16
    _fields_ = [("raw", ctypes.c_byte * 1232)]


def context_regs(hthread):
    ctx = CONTEXT()
    struct.pack_into("<I", ctx.raw, 48, CONTEXT_FULL)
    if not k32.GetThreadContext(hthread, ctypes.byref(ctx)):
        return None
    rsp = struct.unpack_from("<Q", ctx.raw, 0x98)[0]
    rip = struct.unpack_from("<Q", ctx.raw, 0xF8)[0]
    rbp = struct.unpack_from("<Q", ctx.raw, 0xA0)[0]
    return rip, rsp, rbp


def read_mem(hprocess, address, size):
    buffer = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(hprocess, ctypes.c_void_p(address), buffer, size, ctypes.byref(got)):
        return b""
    return buffer.raw[:got.value]


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 60
    env = dict(os.environ)
    if "--" in sys.argv:
        for assignment in sys.argv[sys.argv.index("--") + 1:]:
            key, _, value = assignment.partition("=")
            env[key] = value
    # A process started under a debugger gets Windows' debug heap, which
    # checks every allocation: the host's frame rate measured that way was
    # a different one from the player's. _NO_DEBUG_HEAP=0 keeps it.
    env.setdefault("_NO_DEBUG_HEAP", "1")

    # The build's executable and its symbols, installed next to the game.
    # A test installs and runs them as CoD3-test.exe, so it never replaces
    # the player's CoD3.exe nor touches their saves; DEBUGRUN_EXE=CoD3.exe
    # installs the build as the game itself.
    game_dir = r"D:\Games\x360"
    name = os.environ.get("DEBUGRUN_EXE", "CoD3-test.exe")
    stem = os.path.splitext(name)[0]
    keeps = stem.lower() in ("cod3", "cod3-test")
    # A probe (any other name) runs from a folder of its own on the system
    # drive: the game's drive was found full, and a probe there could not
    # even be copied. game.path sends it to the game; the player's settings
    # are copied beside it (so a probe that changes them changes its copy),
    # and the sound decoder's libraries too.
    import shutil
    if keeps:
        install_dir = game_dir
    else:
        install_dir = os.path.join(os.environ.get("TEMP", "."), "claude", "debugrun-game", "bin")
        os.makedirs(install_dir, exist_ok=True)
        with open(os.path.join(install_dir, "game.path"), "w") as f:
            f.write(os.path.join(game_dir, "game") + "\n")
        # DEBUGRUN_INI: settings of the test's own instead of the player's.
        settings = os.environ.get("DEBUGRUN_INI")
        for extra in ("CoD3Recomp.ini", "avcodec-62.dll", "avutil-60.dll", "swresample-6.dll"):
            source = settings if extra.endswith(".ini") and settings else os.path.join(game_dir, extra)
            target = os.path.join(install_dir, extra)
            if os.path.exists(source) and (extra.endswith(".ini") or not os.path.exists(target)):
                shutil.copy2(source, target)
    exe = os.path.join(install_dir, name)
    # A run under another name is a test beside the installed game: its
    # saves and its log go to a folder of their own, so the player's saves
    # and CoD3.log are left alone (COD3_SAVES / COD3_LOG given override it).
    if os.path.basename(exe).lower() != "cod3.exe":
        scratch = os.path.join(os.environ.get("TEMP", "."), "claude", "debugrun-game")
        os.makedirs(scratch, exist_ok=True)
        env.setdefault("COD3_SAVES", os.path.join(scratch, "saves"))
        env.setdefault("COD3_LOG", os.path.join(scratch, "CoD3.log"))
    build = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "CoD3Host")
    # The symbols go with an install that stays (CoD3.exe, CoD3-test.exe);
    # a probe finds them where the build left them, by the path inside the
    # executable.
    for name, target in (("CoD3.exe", stem + ".exe"), ("CoD3.pdb", stem + ".pdb")):
        source = os.path.join(build, name)
        if name.endswith(".pdb") and not keeps:
            stale = os.path.join(os.path.dirname(exe), target)
            if os.path.exists(stale):
                os.remove(stale)
            continue
        if os.path.exists(source):
            shutil.copy2(source, os.path.join(os.path.dirname(exe), target))
    out_path = os.path.join(os.environ.get("TEMP", "."), "claude", "debugrun.out")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    out = open(out_path, "wb")

    # Redirect stdout and stderr of the child into the file.
    sa = (ctypes.c_byte * 24)()
    struct.pack_into("<IQI", sa, 0, 24, 0, 1)   # nLength, lpSecurityDescriptor, bInheritHandle
    import msvcrt
    handle = msvcrt.get_osfhandle(out.fileno())
    k32.SetHandleInformation(wt.HANDLE(handle), 1, 1)   # HANDLE_FLAG_INHERIT

    si = STARTUPINFO()
    si.cb = ctypes.sizeof(si)
    si.dwFlags = 0x100   # STARTF_USESTDHANDLES
    si.hStdOutput = handle
    si.hStdError = handle
    si.hStdInput = k32.GetStdHandle(-10)
    pi = PROCESS_INFORMATION()
    envblock = "".join(f"{k}={v}\0" for k, v in env.items()) + "\0"
    ok = k32.CreateProcessW(exe, None, None, None, True,
                            DEBUG_ONLY_THIS_PROCESS | 0x400,   # CREATE_UNICODE_ENVIRONMENT
                            ctypes.c_wchar_p(envblock), r"D:\Games\x360",
                            ctypes.byref(si), ctypes.byref(pi))
    if not ok:
        print("CreateProcess failed", k32.GetLastError())
        return 1

    threads = {}
    exe_base = 0
    started = time.time()
    event = DEBUG_EVENT()
    exceptions = 0
    recent = []
    ending = False
    while True:
        # Looked at between events as well: a run that raises them all the
        # time (a sampling profiler's stale stack walks) never waits a second.
        if not ending and time.time() - started > seconds:
            print(f"debugrun: {seconds} s elapsed, ending the process")
            k32.TerminateProcess(pi.hProcess, 0)
            ending = True
        if not k32.WaitForDebugEvent(ctypes.byref(event), 1000):
            continue
        code = event.dwDebugEventCode
        status = DBG_CONTINUE
        if code == CREATE_PROCESS_DEBUG_EVENT:
            exe_base = event.u.CreateProcessInfo.lpBaseOfImage
            threads[event.dwThreadId] = event.u.CreateProcessInfo.hThread
            print(f"debugrun: process started, image base {exe_base:#x}")
        elif code == CREATE_THREAD_DEBUG_EVENT:
            h = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, event.dwThreadId)
            threads[event.dwThreadId] = h
        elif code == EXIT_THREAD_DEBUG_EVENT:
            threads.pop(event.dwThreadId, None)
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            print(f"debugrun: process exited with {event.u.ExitProcess.dwExitCode:#x} after {time.time() - started:.1f} s")
            k32.ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE)
            break
        elif code == EXCEPTION_DEBUG_EVENT:
            record = event.u.Exception.ExceptionRecord
            first = event.u.Exception.dwFirstChance
            xcode = record.ExceptionCode & 0xFFFFFFFF
            # Demand commit faults are the runtime's own business: only the
            # ones that are not access violations, or second chance, or the
            # last few first chance ones are printed.
            interesting = (xcode != 0xC0000005) or not first
            if xcode == 0xC0000005 and first:
                exceptions += 1
                regs = context_regs(threads.get(event.dwThreadId, 0))
                recent.append((time.time() - started, event.dwThreadId, (record.ExceptionAddress or 0) - exe_base,
                               record.ExceptionInformation[0], record.ExceptionInformation[1], regs))
                del recent[:-12]
            if not first:
                print("debugrun: the last first chance faults before this one:")
                for at, tid, rel, kind, address, r in recent:
                    print(f"  {at:6.2f}s thread {tid} exe+{rel:x} kind {kind} address {address:#x}"
                          + (f" rsp {r[1]:#x}" if r else ""))
            if interesting:
                info = [record.ExceptionInformation[i] for i in range(min(record.NumberParameters, 4))]
                regs = context_regs(threads.get(event.dwThreadId, 0))
                where = record.ExceptionAddress or 0
                rel = where - exe_base
                print(f"debugrun: {'first' if first else 'SECOND'} chance exception {xcode:08X} on thread {event.dwThreadId} "
                      f"at {where:#x} (exe+{rel:x}) info {[hex(i) for i in info]} after {time.time() - started:.1f} s")
                if regs:
                    rip, rsp, rbp = regs
                    print(f"  rip exe+{rip - exe_base:x} rsp {rsp:#x} rbp {rbp:#x}")
                    # Return addresses on the stack that point into the exe.
                    stack = read_mem(pi.hProcess, rsp, 4096)
                    frames = []
                    for i in range(0, len(stack) - 8, 8):
                        value = struct.unpack_from("<Q", stack, i)[0]
                        if exe_base <= value < exe_base + 0x4000000:
                            frames.append(f"{value - exe_base:x}")
                    print("  exe addresses on the stack:", " ".join(frames[:40]))
                sys.stdout.flush()
            if xcode == 0x80000003 and first:   # breakpoint: the initial one, or a DbgBreakPoint
                status = DBG_CONTINUE
            else:
                status = DBG_EXCEPTION_NOT_HANDLED
        k32.ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status)

    out.close()
    print(f"debugrun: {exceptions} first chance access violations (demand commits and the like); output in {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
