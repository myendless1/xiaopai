高优先级必测

开机音频初始化
需要确认 AW88298/ES7210/I2S0 能稳定启动。现在 app_main 会直接 audio_service_init/start，如果 I2C、电源、codec 初始化有问题，后续 TTS、录音、realtime 都会失败。
验证：看串口是否有 AudioService start failed、AW88298/ES7210 相关错误；跑 audio_test/tone。

喇叭播放质量
旧路径是 output-only codec，现在统一走 AudioService，16k PCM 会重采样到 24k 播放。
验证：/command/speak、缓存音频、普通 TTS、realtime TTS 是否音量正常、无明显变速、爆音、断续。

录音电平和触发阈值
输入现在是 ES7210 + AFE 后的 16k clean PCM，不再是 M5.Mic 原始数据。旧的 CONFIG_STACKCHAN_VOICE_START_THRESHOLD/STOP_THRESHOLD 可能不再合适。
验证：安静时 level、正常说话 level、远距离说话 level。看屏幕/日志里的 level=，确认不会误触发或太难触发。

AEC 场景
这是最大行为变化。播放 TTS 时输入不断流，但状态机仍会 gate ASR 发送；播放结束后有 echo guard。
验证：安静环境播放 TTS 不应触发识别；TTS 播放时你说话，后续是否能正常打断/识别；喇叭音量高时是否回声误触发。

realtime WebSocket 语音
协议仍是 16k/60ms Opus，但上行音频来源变了。
验证：hello 后能 listen start、连续 binary Opus、listen stop；服务端能返回 STT；服务端下发 Opus TTS 能播放。

中优先级回归

/upload-audio 本地录音上传
WAV 格式没改，但内容改为 AFE clean PCM。
验证：上传后的识别准确率、录音开始/停止时机、是否截断开头或拖尾太长。

stop/sleep/wake/volume 命令
codec_audio_output_stop() 现在只是 abort AudioService 播放队列，不关闭硬件。
验证：TTS 播放中发 stop/sleep 能立即停；wake/listen 后仍可继续识别；volume 命令立即影响后续播放。

sequence 命令
sequence 里的 speak、stop、sleep、camera 混合时，旧逻辑依赖 codec begin/end 生命周期；现在 begin/end 是 shim。
验证：连续 speak、speak 后 camera、camera 后 speak、sequence 中途 stop。

camera / find_owner / tracking
这些路径里还有 codec_audio_output_end()，现在它只等播放空闲，不释放音频硬件。理论上更安全，但如果 camera 和音频共享 I2C/资源，仍需确认。
验证：拍照、find_owner、servo tracking 期间音频服务不崩；执行后 realtime/listen 能恢复。

OpenClaw 回复链路
音频播放、ASR gate、local voice state 都受影响。
验证：OpenClaw 思考时 ASR 暂停，回复 TTS 播放，播放后恢复监听。

低优先级/压力测试

连续 TTS 10 次以上，无播放卡死、队列泄漏、音频服务不可用。
TTS 播放中连续发 stop/speak/volume，确认不会残留旧音频。
长时间 realtime 监听 10-20 分钟，确认没有 I2S read/write 超时堆积。
高音量播放后马上说话，确认 echo guard 和 AEC 不会让识别一直误触发。
需要特别确认的非音频变更
当前 worktree 里还看到 stack-chan/tools/preview_face_grid.py 和两个预览 PNG 有改动，这不属于本次音频重构范围。你需要确认这是不是你之前已有的改动；我没有把它作为音频改动处理。