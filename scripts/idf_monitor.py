"""
PlatformIO extra_script: IDF Monitor with Auto-Reconnect
断开后自动重连，解决 ESP32-S3 USB-JTAG reset 后串口中断问题
用法: pio run -t idf_monitor  或 VSCode PlatformIO 侧边栏 Custom 区
退出: Ctrl+C (或直接关闭终端)
"""
Import("env")  # noqa: F821
import subprocess
import sys
import os
import time

# Windows Ctrl+C 退出码: STATUS_CONTROL_C_EXIT = 0xC000013A = -1073741510
_CTRL_C_EXIT = -1073741510


def start_idf_monitor(source, target, env):
    port = env.GetProjectOption("monitor_port")
    baud = str(env.GetProjectOption("monitor_speed"))

    print(f"\n=== Monitor: {port} @ {baud} baud (auto-reconnect) ===")
    print("Ctrl+C or close terminal to stop.\n")

    cmd = [
        sys.executable, "-m", "platformio", "device", "monitor",
        "-p", port, "-b", baud,
        "--filter", "direct",
        "--filter", "esp32_exception_decoder",
    ]

    # 清理可能导致子进程 Python 环境冲突的变量
    clean_env = os.environ.copy()
    clean_env.pop("PYTHONHOME", None)
    clean_env.pop("PYTHONPATH", None)

    while True:
        try:
            result = subprocess.run(cmd, check=False, env=clean_env)
            rc = result.returncode

            # Ctrl+C / 正常退出 → 停止
            if rc in (0, _CTRL_C_EXIT) or rc > 0:
                print("\nMonitor stopped.")
                break

            # 非正常断开 (串口掉线) → 重连
            print("\r[Port disconnected, reconnecting in 0.3s...]", flush=True)
            time.sleep(0.3)

        except KeyboardInterrupt:
            print("\nMonitor stopped.")
            break


env.AddCustomTarget(  # noqa: F821
    name="idf_monitor",
    dependencies=None,
    actions=start_idf_monitor,
    title="IDF Monitor (Auto-Reconnect)",
    description="reset 后自动重连，捕获所有开机日志",
)
