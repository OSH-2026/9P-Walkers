#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN    48
#define LED_COUNT  1

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  // 强制使用硬件串口0 (也就是你现在插着的 UART 接口)
  Serial0.begin(115200);
  
  // 给点时间缓冲
  delay(1000);
  
  // 注意这里改成了 Serial0
  Serial0.println("\n--- ESP32-S3 Hardware UART0 Test Start! ---");

  strip.begin();
  strip.show(); 
  strip.setBrightness(50); 
}

void loop() {
  strip.setPixelColor(0, strip.Color(255, 0, 0));
  strip.show();
  Serial0.println("Red");
  delay(1000);

  strip.setPixelColor(0, strip.Color(0, 255, 0));
  strip.show();
  Serial0.println("Green");
  delay(1000);

  strip.setPixelColor(0, strip.Color(0, 0, 255));
  strip.show();
  Serial0.println("Blue");
  delay(1000);

  strip.setPixelColor(0, strip.Color(0, 0, 0));
  strip.show();
  Serial0.println("Off");
  delay(1000);
}