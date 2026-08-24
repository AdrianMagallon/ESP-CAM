// Custom TFT_eSPI setup for the ILI9488 3.5" panel, wired per our
// ESP32-S3 pin layout: CS=1, DC=2, SCK=21, MOSI=47, MISO=41, TOUCH_CS=42,
// RST tied straight to 3.3V (no GPIO).
#define USER_SETUP_INFO "User_Setup"

#define USE_HSPI_PORT // will not work without this
#define ILI9488_DRIVER //// WARNING: Do not connect ILI9488 display SDO to MISO if other devices share the SPI bus (TFT SDO does NOT tristate when CS is high)

// If colors look wrong (blue-tinted, blocky, or otherwise off), it's
// almost always this: the panel and TFT_eSPI disagree on red/blue byte
// order. Try TFT_BGR first; if that makes it worse, swap to TFT_RGB.
#define TFT_RGB_ORDER TFT_BGR

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

#define TFT_MISO 41
#define TFT_MOSI 47
#define TFT_SCLK 21
#define TFT_CS    1  // Chip select control pin
#define TFT_DC    2  // Data Command control pin
#define TFT_RST  -1  // Tied straight to 3.3V, not driven by a GPIO
 
#define TOUCH_CS 42  // Chip select pin (T_CS) of touch screen

#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
#define LOAD_FONT6  // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
#define LOAD_FONT7  // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:-.
#define LOAD_FONT8  // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only characters 1234567890:-.
//#define LOAD_FONT8N // Font 8. Alternative to Font 8 above, slightly narrower, so 3 digits fit a 160 pixel TFT
#define LOAD_GFXFF  // FreeFonts. Include access to the 48 Adafruit_GFX free fonts FF1 to FF48 and custom fonts
 
// Comment out the #define below to stop the SPIFFS filing system and smooth font code being loaded
// this will save ~20kbytes of FLASH
#define SMOOTH_FONT

#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000