#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// 根据官方文档，ESP32-S3-DevKitC-1 的板载 WS2812 连接在 GPIO 48
#define LED_PIN    48

// 板子上只有 1 颗 WS2812 灯珠
#define LED_COUNT  1

// 创建一个 NeoPixel 对象
// 参数1：灯珠数量
// 参数2：控制引脚
// 参数3：颜色顺序(通常是 GRB)和通信频率(800 KHz)
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  // 初始化串口（如果需要看打印信息的话）
  Serial.begin(115200);
  Serial.println("ESP32-S3 WS2812 Test Start!");

  // 初始化 WS2812 库
  strip.begin();
  // 显示当前状态（默认为全黑/关闭）
  strip.show(); 
  // 设置亮度 (0-255)。WS2812 非常亮，建议先设置低一点（比如 50）以免刺眼
  strip.setBrightness(50); 
}

void loop() {
  // 1. 设置为红色：strip.Color(红, 绿, 蓝)
  strip.setPixelColor(0, strip.Color(255, 0, 0));
  strip.show(); // 把颜色数据发送给灯带生效
  Serial.println("Red");
  delay(1000);  // 等待 1 秒 (1000 毫秒)

  // 2. 设置为绿色
  strip.setPixelColor(0, strip.Color(0, 255, 0));
  strip.show();
  Serial.println("Green");
  delay(1000);

  // 3. 设置为蓝色
  strip.setPixelColor(0, strip.Color(0, 0, 255));
  strip.show();
  Serial.println("Blue");
  delay(1000);

  // 4. 关闭 (黑色)
  strip.setPixelColor(0, strip.Color(0, 0, 0));
  strip.show();
  Serial.println("Off");
  delay(1000);
}