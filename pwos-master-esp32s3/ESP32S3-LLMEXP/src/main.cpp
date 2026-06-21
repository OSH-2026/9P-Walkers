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
    Serial0.printf("\n\n[calculate finish! %.2f tokens/second]\n", tokens_ps);
    Serial0.println("-----------------------------------");
    
    // 清空用户在模型推理期间胡乱敲入的“垃圾字符”，防止意外触发下一次生成
    while (Serial0.available() > 0) {
        Serial0.read(); 
    }

    Serial0.println("\nplease enter the beginning of the story (Prompt):");
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
    Serial0.println("\n--- start ESP32-S3 interactive local large model ---");

    // 1. 挂载 SPIFFS 文件系统
    if (!SPIFFS.begin(true)) {
        Serial0.println("SPIFFS Mount Failed!");
        return;
    }

    // 2. 检查模型文件是否存在
    if (!SPIFFS.exists("/stories260K.bin") || !SPIFFS.exists("/tok512.bin")) {
        Serial0.println("Error: Model files not found! Please check if you have executed Upload Filesystem Image.");
        return;
    }

    // 3. 初始化大模型组件
    char *checkpoint_path = (char *)"/spiffs/stories260K.bin";
    char *tokenizer_path  = (char *)"/spiffs/tok512.bin";
    unsigned long long rng_seed = esp_random(); // 获取初始硬件随机种子

    Serial0.println("Loading Transformer Model (stories260K.bin)...");
    build_transformer(&transformer, checkpoint_path);

    Serial0.println("Loading Tokenizer (tok512.bin)...");
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    Serial0.println("Loading Sampler...");
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);

    Serial0.printf("PSRAM capacity: %d Bytes | Remaining: %d Bytes\n", ESP.getPsramSize(), ESP.getFreePsram());
    Serial0.println("\nModel loaded successfully!");
    Serial0.println("Please enter the beginning of the story (Prompt) and press Enter to start inference:");
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

            Serial0.printf("\n> Get new Prompt: \"%s\"\n", input_prompt.c_str());
            Serial0.println("--- Start burning MCU computing power! ---\n");
            Serial0.flush();

            // 刷新硬件随机数种子，确保相同的 Prompt 也能生成不同的故事
            sampler.rng_state = esp_random(); 

            // 启动推理引擎
            generate(&transformer, &tokenizer, &sampler, (char*)input_prompt.c_str(), steps, generation_done);
        }
    }
}