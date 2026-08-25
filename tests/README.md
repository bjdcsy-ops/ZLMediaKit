此目录下的所有.cpp文件将被编译成可执行程序(不包含此目录下的子目录).
子目录DeviceHK为海康IPC的适配程序,需要先下载海康的SDK才能编译,
由于操作麻烦,所以仅把源码放在这里仅供参考.

- test_benchmark.cpp
    
    rtsp/rtmp性能测试客户端
    
- test_httpApi.cpp
  
  http api 测试服务器
 
- test_httpClient.cpp
   
   http 测试客户端

- test_player.cpp
   
   rtsp/rtmp带视频渲染的客户端

- test_pusher.cpp
   
   先拉流再推流的测试客户端
 
- test_pusherMp4.cpp
   
   解复用mp4文件再推流的测试客户端
 
- test_server.cpp
   
   rtsp/rtmp/http等服务器
 
- test_wsClient.cpp
  
  websocket测试客户端
 
- test_wsServer.cpp
   
   websocket回显测试服务器
 
## RTSP PLAY RTP-Info 回归测试

`test_rtsp_play_rtp_info.py` 通过 RTSP-over-TCP 连接正在输出的直播流，
并检查 PLAY 响应中每轨的 `seq` 和 `rtptime` 是否与实际发送的首个
RTP 包一致。运行测试前，需要先向 ZLMediaKit 发布一路包含目标音频
采样率的流。

```bash
python3 tests/test_rtsp_play_rtp_info.py \
  'rtsp://user:password@127.0.0.1/live/2' \
  --expect-audio-clock 44100 \
  --sessions 20 \
  --timeout 10
```

测试输出不会显示 URL 中的用户名和密码。任意会话中的轨道缺失或
RTP-Info 不匹配都会使脚本以非零状态退出。
