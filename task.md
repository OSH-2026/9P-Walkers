下面是按模块拆解后的 Markdown 版本，可以直接放进任务看板或继续细分给不同成员。

# 9P-Walkers 待完成模块拆解

## 一、当前总体判断

项目目前处于“**协议与主控原型已具备，真实主从闭环尚未打通**”阶段。

### 已有基础
- mini9P 协议规范已完成，见 protocol_spec.md
- 主控 mini9P client 已基本实现，见 mini9p_client.c
- 主控 shell 已有演示入口，见 shell.c
- 从机 littlefs 自测已具备，见 fs_selftest.c

### 主要缺口
- 主控仍在使用 mock transport，未接真实 UART，见 shell.c
- 从机 mini9P server 基本未实现，见 mini9p_server.c
- 从机 mini9p 模块未纳入构建，见 CMakeLists.txt
- Web Shell 仍是预留骨架，未接入主流程，见 websocket_shell.c

---

## 二、模块拆解

## 1. 主控真实传输层

### 目标
将主控侧 mini9P 请求从 mock transport 切换为真实 UART transport。

### 当前状态
- 仍使用 mock transport 模拟收发
- shell 中的 ls、cat、m9p_attach、m9p_walk 都没有走真实链路

### 输入
- mini9P client 已实现
- 协议编解码已实现
- shell 调用路径已存在

### 输出
- `uart_transport()` 或等价 transport 回调
- 支持发送请求帧、接收响应帧、校验 CRC、超时返回错误
- 替换 shell 中的 mock transport

### 关键文件
- shell.c
- mini9p_client.c
- mini9p_protocol.c

### 子任务
1. 设计 UART 发送与接收接口
2. 实现帧同步逻辑，按 `9P` magic 重同步
3. 实现超时控制
4. 将 transport 挂到 `m9p_client_init`
5. 保留基础诊断日志

### 完成标准
- 主控可通过 UART 发出 Tattach/Twalk/Tread/Twrite 请求
- 收到合法响应后能正常解析并返回给 shell

---

## 2. 从机 mini9P Server

### 目标
实现从机对 mini9P v1 最小消息集的处理能力。

### 当前状态
- 协议头文件存在
- server 代码几乎为空

### 输出
- 可处理以下消息：
  - `Tattach`
  - `Twalk`
  - `Topen`
  - `Tread`
  - `Twrite`
  - `Tstat`
  - `Tclunk`

### 关键文件
- mini9p_server.c
- mini9p_server.h
- mini9p_protocol.h

### 子任务
1. 实现帧解码入口
2. 实现消息分发逻辑
3. 实现每类请求的响应构造
4. 实现 fid 表管理
5. 实现错误响应 `Rerror`

### 约束
- 禁止动态内存分配
- 使用静态数组管理 fid
- 先支持单会话、低并发

### 完成标准
- 从机收到主控请求后，能返回合法 mini9P 响应帧
- 至少能完成 attach、walk、read、write、stat

---

## 3. 从机最小虚拟文件树

### 目标
为 mini9P server 提供最小可演示的资源节点。

### 当前状态
- 文件系统底座有
- 虚拟文件树尚未落地

### 建议最小文件树
```text
/
├── sys/
│   ├── version
│   └── health
└── dev/
    ├── led
    └── temperature
```

### 输出
- 路径解析逻辑
- 文件节点属性
- read/write 回调

### 关键文件
- mini9p_server.c
- main.c

### 子任务
1. 定义节点结构体
2. 定义 root/sys/dev 目录节点
3. 定义 version/health/led/temperature 文件节点
4. 实现路径 walk
5. 实现 read/write/stat 回调

### 完成标准
- 主控能读取 `/sys/version`
- 主控能读取 `/sys/health`
- 主控能写 `/dev/led`
- 主控能读 `/dev/temperature`

---

## 4. 从机 UART 接收与协议处理入口

### 目标
把从机 UART2 和 mini9P server 真正接起来。

### 当前状态
- UART2 已初始化
- 仅用于早期启动输出，未承载 mini9P 请求处理

### 关键文件
- main.c
- usart.h

### 子任务
1. 增加 UART 接收缓冲区
2. 增加接收中断或轮询解析入口
3. 从字节流中识别完整 mini9P 帧
4. 调用 mini9P server 生成响应
5. 通过 UART 回发响应帧

### 完成标准
- STM32 能从 UART 接收完整请求并回包
- 无需上位机辅助，即可完成主从直连演示

---

## 5. 从机构建接入

### 目标
让 mini9P server 真正参与编译与链接。

### 当前状态
- 从机 CMake 只包含 fs/app 等模块
- mini9p 源文件未加入
- 存在异常文件名

### 关键文件
- CMakeLists.txt
- mini9p_server.c
- mini9p_protocol..c

### 子任务
1. 将 mini9p 源文件加入 `target_sources`
2. 将 mini9p 头文件目录加入 include path
3. 修复 mini9p_protocol..c 的命名问题
4. 确保链接无重复符号和缺失符号

### 完成标准
- 从机工程可编译通过
- mini9P server 代码被实际编译进产物

---

## 6. 主控 Shell 接入真实链路

### 目标
让 shell 命令真正访问远程从机，而不是只做本地 mock 演示。

### 当前状态
- shell 已有命令框架
- 命令行为仍是 mock

### 关键文件
- shell.c

### 子任务
1. 替换 mock client 初始化逻辑
2. 修改 `ls`
3. 修改 `cat`
4. 修改 `m9p_attach`
5. 修改 `m9p_walk`
6. 保留错误输出与状态码可视化

### 完成标准
- 在 shell 中执行 `m9p_attach` 有真实响应
- `cat /sys/version` 可打印远端内容
- `echo 1 > /dev/led` 或等价命令可触发远端写入

---

## 7. cluster_vfs 与统一命名空间

### 目标
在主控侧引入 `/mcu1/...` 形式的统一命名空间。

### 当前状态
- 架构文档已定义
- 实际代码尚未落地

### 依赖
- 必须在单节点主从闭环跑通之后再做

### 输出
- 本地路径分发层
- `/mcu1/...` 到单节点本地路径的映射
- 多节点扩展预留接口

### 完成标准
- 主控本地能通过统一前缀访问单节点
- 后续支持多节点时不需要重写协议层

---

## 8. Web Shell 接入

### 目标
把已有 Web 页面、WebSocket 和 shell 真正接入系统主流程。

### 当前状态
- 前端页面已写
- HTTP server 与 WS 代码已有骨架
- 广播仍是 TODO
- 主程序未启动 Web server
- 构建未纳入 web 源文件

### 关键文件
- http_server.c
- websocket_shell.c
- CMakeLists.txt
- hello_world_main.c

### 子任务
1. 将 web 模块加入构建
2. 在主程序中启动 HTTP server
3. 实现 WS 文本回传
4. 打通 shell 输出到浏览器
5. 再考虑设置页里的 UART/SPI/WiFi 参数绑定

### 完成标准
- 浏览器可连接到主控
- 能通过网页输入 shell 命令并看到执行输出

---

## 三、推荐执行顺序

## Phase 1：单节点闭环
1. 从机构建接入
2. 从机 mini9P server
3. 从机最小虚拟文件树
4. 主控真实 UART transport
5. shell 接入真实链路

### 目标结果
- 单个 STM32 从机可被 ESP32 主控通过 mini9P 访问
- 支持 attach/read/write/stat 基本链路

## Phase 2：统一路径
1. cluster_vfs
2. `/mcu1/...` 命名空间映射
3. 错误处理与日志完善

### 目标结果
- 主控可以用统一路径访问远程节点

## Phase 3：展示层
1. Web Shell 接入
2. Lua 编排接 VFS
3. 计算任务 demo

### 目标结果
- 形成课程展示可用的完整交互链路

---

## 四、可直接分配的任务包

## 任务包 A：从机协议服务端
- mini9P server
- fid 表
- 最小虚拟文件树
- UART 请求处理

## 任务包 B：主控通信与命令接入
- UART transport
- shell 替换 mock
- 单节点联调日志

## 任务包 C：系统集成与展示
- cluster_vfs
- Web Shell
- Lua 与远程文件访问绑定

---

## 五、最小里程碑定义

### 里程碑 M1
- 主控 `m9p_attach` 成功
- 主控读取 `/sys/version`
- 主控读取 `/sys/health`

### 里程碑 M2
- 主控写 `/dev/led`
- 主控读 `/dev/temperature`
- shell 命令路径完整跑通

### 里程碑 M3
- 支持 `/mcu1/...`
- 支持网页端发命令并回显