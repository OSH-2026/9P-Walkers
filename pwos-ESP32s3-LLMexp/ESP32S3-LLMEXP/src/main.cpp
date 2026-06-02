#include <Arduino.h>
#include "FS.h"
#include "SPIFFS.h"
#include "llm.h"

// 全局大模型组件 (保证其在 setup 结束后依然存活)
Transformer transformer;
Tokenizer tokenizer;
Sampler sampler;

// 交互模式参数
int steps = 1024;          // 生成步数限制
float temperature = 0.8f; // 创造力 (0.0=严谨, 1.0=发散)
float topp = 0.9f;

// 状态锁：确保“推理完毕才允许下一次串口接收”
volatile bool is_inferencing = false; 

// 回调函数：模型生成完毕后触发
void generation_done(float tokens_ps) {
    // 这里直接使用标准的 Serial.printf 即可
    Serial0.printf("\n\n[推理完毕! 生成速度: %.2f tokens/秒]\n", tokens_ps);
    Serial0.println("-----------------------------------");
    
    // 清空用户在模型推理期间胡乱敲入的“垃圾字符”，防止意外触发下一次生成
    while (Serial0.available() > 0) {
        Serial0.read(); 
    }

    Serial0.println("\n请输入新的故事开头 (Prompt):");
    Serial0.printf("\x04");
    // 解锁状态，允许 loop() 接收下一次输入
    is_inferencing = false; 
}

void setup() {
    // 强制关闭两个核心的硬件看门狗，防止跑大模型时触发复位
    disableCore0WDT();
    disableCore1WDT();

    Serial0.begin(115200);
    delay(2000);
    Serial0.println("\n--- 启动 ESP32-S3 交互式本地大模型 ---");

    // 1. 挂载 SPIFFS 文件系统
    if (!SPIFFS.begin(true)) {
        Serial0.println("SPIFFS 挂载失败！");
        return;
    }

    // 2. 检查模型文件是否存在
    if (!SPIFFS.exists("/stories260K.bin") || !SPIFFS.exists("/tok512.bin")) {
        Serial0.println("错误：找不到模型文件！请检查是否执行了 Upload Filesystem Image。");
        return;
    }

    // 3. 初始化大模型组件
    char *checkpoint_path = (char *)"/spiffs/stories260K.bin";
    char *tokenizer_path  = (char *)"/spiffs/tok512.bin";
    unsigned long long rng_seed = esp_random(); // 获取初始硬件随机种子

    Serial0.println("正在加载 Transformer 模型权重 (stories260K.bin)...");
    build_transformer(&transformer, checkpoint_path);
    
    Serial0.println("正在加载分词器 (tok512.bin)...");
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    Serial0.println("正在初始化采样器...");
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);
    
    Serial0.printf("PSRAM 总容量: %d 字节 | 剩余量: %d 字节\n", ESP.getPsramSize(), ESP.getFreePsram());
    Serial0.println("\n大模型大脑加载完毕！");
    Serial0.println("请输入故事开头 (Prompt) 并回车开始推理：");
}

void loop() {
    // 如果正在推理，直接跳过整个 loop，绝对不理会串口新消息
    if (is_inferencing) {
        delay(50); // 稍微延时，让出 CPU 算力给推理引擎
        return;
    }

    // 检查串口是否有新的提示词送达
    if (Serial0.available() > 0) {
        // 读取一行输入，直到遇到换行符 \n
        String input_prompt = Serial0.readStringUntil('\n');
        input_prompt.trim(); // 清除前后的空格或回车符

        // 如果输入不是空的，开始推理
        if (input_prompt.length() > 0) {
            
            // 立刻上锁禁止在此期间接收任何新串口输入
            is_inferencing = true; 

            Serial0.printf("\n> 收到新 Prompt: \"%s\"\n", input_prompt.c_str());
            Serial0.println("--- 开始燃烧单片机算力！ ---\n");
            Serial0.flush();

            // 刷新硬件随机数种子，确保相同的 Prompt 也能生成不同的故事
            sampler.rng_state = esp_random(); 

            // 启动推理引擎
            generate(&transformer, &tokenizer, &sampler, (char*)input_prompt.c_str(), steps, generation_done);
        }
    }
}