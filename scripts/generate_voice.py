#!/usr/bin/env python3
"""
generate_voice.py - Generate voice prompts for Box2Robot CAM

Uses edge-tts (Microsoft Azure TTS) to generate Chinese/English audio.
Output: 16-bit PCM, 16000Hz, Mono in main/voice_data/*.pcm
Plus:  main/voice_clips.h (index only, extern symbols from EMBED_FILES)

Usage:
    pip install edge-tts pydub imageio-ffmpeg
    python scripts/generate_voice.py
"""

import asyncio
import os
import edge_tts

# Find ffmpeg via imageio_ffmpeg if not in PATH
try:
    import imageio_ffmpeg
    ffmpeg_path = imageio_ffmpeg.get_ffmpeg_exe()
    os.environ["PATH"] = os.path.dirname(ffmpeg_path) + os.pathsep + os.environ.get("PATH", "")
    from pydub import AudioSegment
    AudioSegment.converter = ffmpeg_path
except ImportError:
    pass

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main", "voice_data")
HEADER_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main", "voice_clips.h")
CMAKE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main", "CMakeLists.txt")

SAMPLE_RATE = 16000
# Unified voice: YunxiaNeural (cute boy) for both Chinese and English
VOICE = "zh-CN-YunxiaNeural"

# ============ All clips ============

DIGITS = {f"digit_{i}": text for i, text in enumerate("零一二三四五六七八九")}

# Letters read by Chinese voice (sounds natural + unified tone)
LETTERS = {
    "letter_A": "A", "letter_B": "B", "letter_C": "C", "letter_D": "D",
    "letter_E": "E", "letter_F": "F", "letter_G": "G", "letter_H": "H",
    "letter_I": "I", "letter_J": "J", "letter_K": "K", "letter_L": "L",
    "letter_M": "M", "letter_N": "N", "letter_O": "O", "letter_P": "P",
    "letter_Q": "Q", "letter_R": "R", "letter_S": "S", "letter_T": "T",
    "letter_U": "U", "letter_V": "V", "letter_W": "W", "letter_X": "X",
    "letter_Y": "Y", "letter_Z": "Z",
}

PROMPTS = {
    "prompt_wifi_ap":     "请用手机连接WiFi",
    "prompt_wifi_name":   "WiFi名称是",
    "prompt_wifi_pwd":    "密码是一二三四五六七八",
    "prompt_wifi_open":   "请访问WIFI认证页面，进行配网操作",
    "prompt_wifi_ok":     "WiFi连接成功",
    "prompt_wifi_fail":   "WiFi连接失败，请重试",
    "prompt_wifi_config": "正在进入配网模式",
    "prompt_wifi_disc":   "WiFi连接已断开，正在重新连接",
    "prompt_bind_code":   "绑定码是",
    "prompt_bind_wait":   "请在服务器网站上添加设备输入绑定码完成绑定",
    "prompt_bind_ok":     "设备绑定成功",
    "prompt_bind_fail":   "绑定失败，请重试",
    "prompt_boot":        "设备启动中，请稍候",
    "prompt_ready":       "设备已就绪",
    "prompt_error":       "出现错误，正在重启",
    "prompt_btn_reset":   "已清除WiFi设置，正在重启",
    "prompt_camera_ok":   "摄像头初始化成功",
    "prompt_audio_ok":    "音频模块初始化成功",
    "prompt_server_ok":   "已连接到服务器",
    "prompt_server_fail": "无法连接服务器",
    "prompt_server_disc": "与服务器连接已断开",
    "prompt_listen":      "请说",
    "prompt_thinking":    "正在思考",
    "prompt_bye":           "再见",
    "prompt_ota_download":  "更新下载中，请不要断电",
    "prompt_ota_flash":     "更新下载完成，正在更新",
    "prompt_ota_reboot":    "更新完毕，正在重启",
    "word_mingle":          "Mingle",
    "word_underscore":      "下划线",
}

async def generate_one(name, text, voice, out_dir):
    from pydub import AudioSegment
    from pydub.silence import detect_leading_silence

    mp3_path = os.path.join(out_dir, f"{name}.mp3")
    pcm_path = os.path.join(out_dir, f"{name}.pcm")

    comm = edge_tts.Communicate(text, voice, rate="-10%")
    await comm.save(mp3_path)

    audio = AudioSegment.from_mp3(mp3_path)
    audio = audio.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)

    # Trim silence
    start = detect_leading_silence(audio, silence_threshold=-40)
    end = detect_leading_silence(audio.reverse(), silence_threshold=-40)
    audio = audio[start:len(audio) - end]

    # Pad 50ms
    pad = AudioSegment.silent(duration=50, frame_rate=SAMPLE_RATE)
    audio = pad + audio + pad

    with open(pcm_path, "wb") as f:
        f.write(audio.raw_data)
    os.remove(mp3_path)

    sz = len(audio.raw_data)
    ms = sz * 1000 // (SAMPLE_RATE * 2)
    print(f"  {name}: \"{text}\" -> {sz} bytes ({ms}ms)")
    return name, sz


def pcm_to_c_array(pcm_path, var_name):
    """Convert PCM binary to C const array declaration."""
    with open(pcm_path, "rb") as f:
        data = f.read()
    lines = []
    for i in range(0, len(data), 32):
        chunk = data[i:i+32]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk))
    return f"extern const uint8_t {var_name}[] = {{\n" + ",\n".join(lines) + "\n};\n"


def generate_data_files(all_clips):
    """Generate .cpp data files with const arrays (split into groups to avoid huge files)."""
    groups = {"voice_digits": {}, "voice_letters": {}, "voice_prompts": {}}
    for name, sz in all_clips.items():
        if name.startswith("digit_"):
            groups["voice_digits"][name] = sz
        elif name.startswith("letter_"):
            groups["voice_letters"][name] = sz
        else:
            groups["voice_prompts"][name] = sz

    cpp_files = []
    for group_name, clips in groups.items():
        cpp_path = os.path.join(os.path.dirname(HEADER_FILE), f"{group_name}.cpp")
        cpp_files.append(f"{group_name}.cpp")
        with open(cpp_path, "w", encoding="utf-8") as f:
            f.write(f'// Auto-generated by generate_voice.py — DO NOT EDIT\n')
            f.write(f'#include <stdint.h>\n\n')
            f.write(f'extern "C" {{\n\n')
            for name in clips:
                pcm_path = os.path.join(OUT_DIR, f"{name}.pcm")
                var = f"VDATA_{name.upper()}"
                f.write(pcm_to_c_array(pcm_path, var))
                f.write(f"extern const unsigned int {var}_SIZE = sizeof({var});\n\n")
            # Close extern "C"
            f.write(f'}} // extern "C"\n')
        sz = sum(clips.values())
        print(f"  {group_name}.cpp: {len(clips)} clips, {sz} bytes ({sz/1024:.0f}KB)")

    return cpp_files


def generate_header(all_clips):
    """Generate voice_clips.h with extern declarations + index arrays."""
    lines = [
        '/**',
        ' * voice_clips.h - Voice prompt index for Box2Robot CAM',
        ' * Auto-generated by scripts/generate_voice.py — DO NOT EDIT',
        ' */',
        '#pragma once',
        '#include <stdint.h>',
        '#include <stddef.h>',
        '#include <string.h>',
        '',
        '#ifdef __cplusplus',
        'extern "C" {',
        '#endif',
        '',
        'typedef struct { const char* name; const uint8_t* data; uint32_t size; } voice_clip_t;',
        '',
    ]

    # Extern declarations
    for name in all_clips:
        var = f"VDATA_{name.upper()}"
        lines.append(f'extern const uint8_t {var}[];')
        lines.append(f'extern const unsigned int {var}_SIZE;')
    lines.append('')

    # Digit array
    lines.append('static const voice_clip_t VOICE_DIGITS[] = {')
    for i in range(10):
        n = f"digit_{i}"
        var = f"VDATA_{n.upper()}"
        lines.append(f'    {{"{i}", {var}, {all_clips[n]}}},')
    lines.append('};')
    lines.append('#define VOICE_DIGITS_COUNT 10')
    lines.append('')

    # Letter array
    lines.append('static const voice_clip_t VOICE_LETTERS[] = {')
    for c in range(ord('A'), ord('Z') + 1):
        n = f"letter_{chr(c)}"
        var = f"VDATA_{n.upper()}"
        lines.append(f'    {{"{chr(c)}", {var}, {all_clips[n]}}},')
    lines.append('};')
    lines.append('#define VOICE_LETTERS_COUNT 26')
    lines.append('')

    # Prompt array
    lines.append('static const voice_clip_t VOICE_PROMPTS[] = {')
    for name in PROMPTS:
        var = f"VDATA_{name.upper()}"
        lines.append(f'    {{"{name}", {var}, {all_clips[name]}}},')
    lines.append('};')
    lines.append(f'#define VOICE_PROMPTS_COUNT {len(PROMPTS)}')
    lines.append('')

    lines.append('static inline const voice_clip_t* voice_find_prompt(const char* name) {')
    lines.append('    for (int i = 0; i < VOICE_PROMPTS_COUNT; i++)')
    lines.append('        if (strcmp(VOICE_PROMPTS[i].name, name) == 0) return &VOICE_PROMPTS[i];')
    lines.append('    return NULL;')
    lines.append('}')
    lines.append('')
    lines.append('#ifdef __cplusplus')
    lines.append('}')
    lines.append('#endif')

    with open(HEADER_FILE, "w", encoding="utf-8") as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Header: {HEADER_FILE} ({len(lines)} lines)")


def update_cmake(cpp_files):
    """Update CMakeLists.txt with generated .cpp source files."""
    srcs = ' '.join(f'"{f}"' for f in ['main.cpp'] + cpp_files)
    cmake = f'idf_component_register(\n    SRCS {srcs}\n    INCLUDE_DIRS "."\n)\n'
    with open(CMAKE_FILE, "w") as f:
        f.write(cmake)
    print(f"CMakeLists.txt: SRCS = main.cpp + {len(cpp_files)} data files")


async def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    all_clips = {}

    print("=== Digits ===")
    for name, text in DIGITS.items():
        n, sz = await generate_one(name, text, VOICE, OUT_DIR)
        all_clips[n] = sz

    print("\n=== Letters ===")
    for name, text in LETTERS.items():
        n, sz = await generate_one(name, text, VOICE, OUT_DIR)
        all_clips[n] = sz

    print("\n=== Prompts ===")
    for name, text in PROMPTS.items():
        n, sz = await generate_one(name, text, VOICE, OUT_DIR)
        all_clips[n] = sz

    total = sum(all_clips.values())
    print(f"\nTotal: {len(all_clips)} clips, {total} bytes ({total/1024:.1f} KB)")

    print("\n=== Generating .cpp data files ===")
    cpp_files = generate_data_files(all_clips)

    generate_header(all_clips)
    update_cmake(cpp_files)
    print("\nDone! Run 'pio run' to build.")


if __name__ == "__main__":
    asyncio.run(main())
