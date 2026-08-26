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

## HTTP API 鉴权与 Cookie 回归测试

`test_http_cookie` 检查服务端生成的 `Expires` 是否使用 IMF-fixdate，并验证
标准日期、旧版 ZLMediaKit 日期和闰日解析：

```bash
./release/linux/Release/test_http_cookie
```

`test_http_api_auth.py` 连接正在运行的 MediaServer，检查以下行为：

- GET、表单 POST 和 JSON POST 携带正确 `secret` 时不创建网页登录 Cookie；
- 缺失或错误的 `secret` 返回并复用 `ZLM_UNLOGIN` challenge；
- challenge 可以完成 digest 登录，并换取 `ZLM_LOGINED`；
- 有效的登录 Cookie 可以独立完成 API 鉴权；
- 登录与登出响应会分别删除旧 challenge 和登录 Cookie；
- HTTP `Date` 与所有 Cookie 的 `Expires` 都使用 IMF-fixdate。

脚本从环境变量读取 API secret，脚本自身不会把 secret 输出到测试结果。
现有服务若启用了 `api.apiDebug`，MediaServer 仍会记录包含 API 参数的请求，
应只在受控环境运行或先关闭该配置：

```bash
ZLM_API_SECRET='<secret>' \
  python3 tests/test_http_api_auth.py http://127.0.0.1:80
```

也可以让脚本使用临时配置启动并停止待测二进制；此模式会自动生成测试
secret，并关闭 HTTP 之外的监听端口：

```bash
python3 tests/test_http_api_auth.py \
  --server artifact/zlmediakit-debian11-arm64/MediaServer
```
