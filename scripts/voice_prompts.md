# Box2Robot CAM 语音提示清单

## 1. 数字 (0-9)
| ID | 文字 | 文件名 | 用途 |
|----|------|--------|------|
| d0 | 零 | digit_0.pcm | 配对码/密码播报 |
| d1 | 一 | digit_1.pcm | |
| d2 | 二 | digit_2.pcm | |
| d3 | 三 | digit_3.pcm | |
| d4 | 四 | digit_4.pcm | |
| d5 | 五 | digit_5.pcm | |
| d6 | 六 | digit_6.pcm | |
| d7 | 七 | digit_7.pcm | |
| d8 | 八 | digit_8.pcm | |
| d9 | 九 | digit_9.pcm | |

## 2. 英文字母 (A-Z)
| ID | 文字 | 文件名 | 用途 |
|----|------|--------|------|
| lA | A | letter_A.pcm | SSID 名称播报 |
| lB | B | letter_B.pcm | |
| ... | ... | ... | |
| lZ | Z | letter_Z.pcm | |

## 3. 配网流程提示
| ID | 文字 | 文件名 | 用途 |
|----|------|--------|------|
| wifi_ap | 请用手机连接WiFi | prompt_wifi_ap.pcm | AP 模式启动时 |
| wifi_name | WiFi名称是 | prompt_wifi_name.pcm | 播报 SSID 前缀 |
| wifi_pwd | 密码是 | prompt_wifi_pwd.pcm | 播报密码前 |
| wifi_open | 连接后请打开浏览器，访问配网页面 | prompt_wifi_open.pcm | 引导用户配网 |
| wifi_ok | WiFi连接成功 | prompt_wifi_ok.pcm | STA 连接成功 |
| wifi_fail | WiFi连接失败，请重试 | prompt_wifi_fail.pcm | STA 连接失败 |
| wifi_config | 正在进入配网模式 | prompt_wifi_config.pcm | 进入 AP 模式 |

## 4. 设备绑定提示
| ID | 文字 | 文件名 | 用途 |
|----|------|--------|------|
| bind_code | 绑定码是 | prompt_bind_code.pcm | 播报配对码前 |
| bind_wait | 请在手机上输入绑定码完成绑定 | prompt_bind_wait.pcm | 等待绑定 |
| bind_ok | 设备绑定成功 | prompt_bind_ok.pcm | 绑定完成 |
| bind_fail | 绑定失败，请重试 | prompt_bind_fail.pcm | 绑定失败 |

## 5. 系统提示
| ID | 文字 | 文件名 | 用途 |
|----|------|--------|------|
| boot | 设备启动中，请稍候 | prompt_boot.pcm | 开机 |
| ready | 设备已就绪 | prompt_ready.pcm | 所有初始化完成 |
| error | 出现错误，正在重启 | prompt_error.pcm | 致命错误重启 |
| btn_reset | 已清除WiFi设置，正在重启 | prompt_btn_reset.pcm | 按钮清除 WiFi |
| camera_ok | 摄像头初始化成功 | prompt_camera_ok.pcm | 摄像头启动 |
| audio_ok | 音频模块初始化成功 | prompt_audio_ok.pcm | 音频启动 |
| server_ok | 已连接到服务器 | prompt_server_ok.pcm | WS 连接成功 |
| server_fail | 无法连接服务器 | prompt_server_fail.pcm | WS 连接失败 |

## 6. 语音对话提示
| ID | 文字 | 文件名 | 用途 |
|----|------|--------|------|
| listen | 请说 | prompt_listen.pcm | 开始录音 |
| thinking | 正在思考 | prompt_thinking.pcm | 等待AI回复 |
| bye | 再见 | prompt_bye.pcm | 结束对话 |

## 播报示例

### 配网播报 (每3秒循环)
```
"正在进入配网模式"
"请用手机连接WiFi"
"WiFi名称是" + "B" + "o" + "x" + "二" + "R" + "o" + "b" + "o" + "t" + "C" + "a" + "m" + "_" + "A" + "三" + "B" + "五"
"密码是" + "一" + "二" + "三" + "四" + "五" + "六" + "七" + "八"
"连接后请打开浏览器，访问配网页面"
```

简化版 (实际播报):
```
"请用手机连接WiFi，密码是一二三四五六七八，连接后请打开浏览器访问配网页面"
```

### 配对码播报
```
"绑定码是" + "三" + "五" + "七" + "八" + "二" + "一"
"请在手机上输入绑定码完成绑定"
```

## 技术规格
- 格式: 16-bit PCM, 16000Hz, Mono
- 生成工具: edge-tts (Microsoft Azure TTS, 免费)
- 语音: zh-CN-XiaoxiaoNeural (女声，清晰)
- 每个片段约 0.5-3 秒
- 预计总大小: ~200-400KB
