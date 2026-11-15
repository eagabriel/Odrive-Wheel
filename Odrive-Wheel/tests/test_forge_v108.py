"""Run the actual FFB sources with hardware/RTOS/flash replaced by test doubles.

Native: python Odrive-Wheel/tests/test_forge_v108.py --compiler g++
WASI:   python Odrive-Wheel/tests/test_forge_v108.py --zig path/to/zig.exe
The WASI route uses Node's built-in WASI runtime; no board is connected.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define CPPMAIN_H_
using hid_report_type_t = int;
constexpr int HID_REPORT_TYPE_INPUT=1, HID_REPORT_TYPE_OUTPUT=2, HID_REPORT_TYPE_FEATURE=3;
static uint32_t testTick = 0;
uint32_t HAL_GetTick() { return testTick; }
uint32_t micros() { return testTick * 1000u; }
template<class T> int sgn(T v) { return (T(0)<v)-(v<T(0)); }
template<class T, class C> T clip(T v, C lo, C hi) { return v>hi ? hi : v<lo ? lo : v; }
#define FLASH_HELPERS_H_
bool Flash_Read(uint16_t, uint16_t*, bool=true) { return false; }
bool Flash_Write(uint16_t, uint16_t) { assert(false && "tests must not save flash"); return false; }
uint16_t pack(uint8_t high, uint8_t low) { return (high<<8)|low; }
#define PERSISTENTSTORAGE_H_
struct PersistentStorage { virtual ~PersistentStorage() = default; };
#define THREAD_HPP_
#define MUTEX_HPP_
namespace cpp_freertos {
struct Thread {
  Thread(const char*, int, int) {}
  virtual ~Thread() = default;
  virtual void Run() = 0;
  static void Delay(unsigned) {}
};
struct MutexStandard {
  bool held = false;
  bool Lock() { assert(!held && "nonrecursive lock reentry"); held=true; return true; }
  bool Unlock() { assert(held); held=false; return true; }
};
}
#define USBHIDHANDLER_H_
struct UsbHidHandler {
  inline static UsbHidHandler* globalHidHandler = nullptr;
  virtual ~UsbHidHandler() = default;
  virtual void hidOut(uint8_t,int,const uint8_t*,uint16_t) {}
  virtual uint16_t hidGet(uint8_t,int,uint8_t*,uint16_t) { return 0; }
  void registerHidCallback() { globalHidHandler=this; }
};
bool tud_hid_report(uint8_t,const void*,uint16_t) { return true; }
#include "AxisSafety.h"
#include "OscillationGuard.h"
#define private public
#include "HidFFB.h"
#undef private
#include "Axis.h"
static float testPos=0, testVel=0, testTorque=0;
float odrive_bridge_get_pos_turns() { return testPos; }
float odrive_bridge_get_vel_estimate() { return testVel; }
void odrive_bridge_set_input_torque(float value) { testTorque=value; }
static uint32_t s_diag_set_eff_torque=0;
static int32_t s_diag_last_torque_int32=0;
'''


def source():
    # Include complete calculator/HID/filters, not rewritten mathematical models.
    parts = [PRELUDE]
    for name in ['Filters.cpp', 'EqCascade.cpp', 'EffectsCalculator.cpp', 'HidFFB.cpp']:
        text = (ROOT / 'src' / name).read_text(encoding='utf-8')
        text = text.replace('#include "hid_device.h"', '')
        parts.append(text)
    axis = (ROOT / 'src/ffb_task.cpp').read_text(encoding='utf-8')
    parts.append(axis[axis.index('class ODriveLocalAxis :'):axis.index('// s_hidffb e s_axis_raw já declarados')])
    parts.append(Path(__file__).with_name('forge_v108_cases.cpp').read_text(encoding='utf-8'))
    return '\n'.join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--zig')
    parser.add_argument('--wasi-sysroot', help='Optional prebuilt WASI sysroot for Zig')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='odrive-v108-') as tmp:
        tmp = Path(tmp)
        cpp = tmp / 'test.cpp'
        cpp.write_text(source(), encoding='utf-8')
        output = tmp / ('test.wasm' if args.zig else 'test.exe')
        command = [args.zig, 'c++', '-target', 'wasm32-wasi'] if args.zig else [args.compiler]
        command += ['-std=c++17', '-O2', '-ffast-math', '-fno-finite-math-only',
                    '-fno-math-errno', '-Wno-macro-redefined', '-I', str(ROOT / 'inc'),
                    str(cpp), '-o', str(output)]
        if args.zig and args.wasi_sysroot:
            sysroot = Path(args.wasi_sysroot).resolve()
            libs = sysroot / 'lib/wasm32-wasi'
            command += ['-fno-exceptions', '-nostdlib', '-nostdlibinc', '-nostdinc++',
                        '-I', str(sysroot / 'include/wasm32-wasi/c++/v1'),
                        '-isystem', str(sysroot / 'include/wasm32-wasi'),
                        str(libs / 'crt1-command.o'), str(libs / 'libc++.a'),
                        str(libs / 'libc++abi.a'), str(libs / 'libc.a'), str(libs / 'libm.a')]
        subprocess.run(command, check=True)
        if args.zig:
            runner = tmp / 'run.cjs'
            runner.write_text('''const fs=require('node:fs');
const {WASI}=require('node:wasi');
const wasi=new WASI({version:'preview1',args:[],env:{},preopens:{}});
const module_=new WebAssembly.Module(fs.readFileSync(process.argv[2]));
wasi.start(new WebAssembly.Instance(module_,{wasi_snapshot_preview1:wasi.wasiImport}));
''', encoding='utf-8')
            subprocess.run(['node', str(runner), str(output)], check=True)
        else:
            subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    main()
