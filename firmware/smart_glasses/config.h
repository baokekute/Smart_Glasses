#ifndef CONFIG_H
#define CONFIG_H

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

#define OLED_ADDRESS    0x3C
// Khai báo chân cho màn hình OLED (I2C) trên ESP32-S3
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 15

// Khai báo chân cho Micro INMP441 (I2S) trên ESP32-S3
#define I2S_WS_PIN 42
#define I2S_SCK_PIN 41
#define I2S_SD_PIN 40
// Nút nhấn bắt đầu/dừng nghe và nút hủy phiên.
#define BUTTON_PIN 4
#define CANCEL_BUTTON_PIN 5
#endif