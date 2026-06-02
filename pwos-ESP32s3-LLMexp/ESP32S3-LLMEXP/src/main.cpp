#include <Arduino.h>
#include "FS.h"
#include "SPIFFS.h"

extern "C" void custom_printf(const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Serial0.println(buf); // 强行把内容塞给串口0
    Serial0.flush();
}

// 因为原作者的 llm 是纯 C 语言写的，在 C++ 的 main.cpp 中调用必须套一层 extern "C"
#include "llm.h"

// 这是一个回调函数，当模型生成完毕后，llm.c 会调用它来报告速度
void generation_done(float tokens_ps) {
    Serial0.printf("\n\n[推理完毕! 生成速度: %.2f tokens/秒]\n", tokens_ps);
}

void setup() {
    // 强制关闭两个核心的硬件看门狗，给大模型无限的算力时间！
    disableCore0WDT();
    disableCore1WDT();

    Serial0.begin(115200);
    delay(2000);
    Serial0.println("\n--- 启动 ESP32-S3 纯本地大模型 ---");

    // 1. 挂载 SPIFFS 文件系统
    if (!SPIFFS.begin(true)) {
        Serial0.println("SPIFFS 挂载失败！");
        return;
    }

    // 2. 检查模型文件和词典文件是否真的烧进去了
    if (!SPIFFS.exists("/stories260K.bin") || !SPIFFS.exists("/tok512.bin")) {
        Serial0.println("错误：找不到模型文件！请检查是否执行了 Upload Filesystem Image。");
        return;
    }

    // 3. 配置 LLM 推理参数
    // 注意：这里的路径必须加 /spiffs 前缀，让底层的 C 语言读取函数知道去哪里找
    char *checkpoint_path = (char *)"/spiffs/stories260K.bin";
    char *tokenizer_path  = (char *)"/spiffs/tok512.bin";
    float temperature     = 1.0f; // 创造力 (0.0=严谨, 1.0=发散)
    float topp            = 0.9f;
    int steps             = 1024;  // 让它最多生成 1024 个词
    char *prompt          = (char *)"There was a young girl living in a strange house."; // 提示词
    unsigned long long rng_seed = millis(); // 随机种子

    // 4. 构建神经网络和相关组件
    Transformer transformer;
    Serial0.println("正在加载 Transformer 模型权重 (stories260K.bin)...");
    build_transformer(&transformer, checkpoint_path);
    
    Tokenizer tokenizer;
    Serial0.println("正在加载分词器 (tok512.bin)...");
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    Sampler sampler;
    Serial0.println("正在初始化采样器...");
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);
    Serial0.println("【系统提示】正式启动推理引擎，纯字符串测试成功！\n");
    Serial0.printf("%s", "【系统提示】正式启动推理引擎，百分号s测试成功！\n");
    Serial0.printf("PSRAM 总容量: %d 字节\n", ESP.getPsramSize());
    Serial0.printf("PSRAM 剩余量: %d 字节\n", ESP.getFreePsram());

    Serial0.println("\n--- 开始燃烧单片机算力！ ---\n");
    
    // 5. 启动推理引擎！
    // 原作者在 llm.c 里使用了标准的 printf，Arduino 框架默认会把 printf 的内容输出到 Serial0
    generate(&transformer, &tokenizer, &sampler, prompt, steps, generation_done);
}

void loop() {
    // LLM 推理是一次性的，算完就休息，所以 loop 留空
    delay(1000);
}