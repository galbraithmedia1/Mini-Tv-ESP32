/***
 * Required libraries:
 * https://github.com/moononournation/Arduino_GFX.git
 * https://github.com/pschatzmann/arduino-libhelix.git
 * https://github.com/bitbank2/JPEGDEC.git
 */

//  Audio and video code

//  ffmpeg -i  office1.mp4 -ar 44100 -ac 1 -ab 24k -filter:a loudnorm -filter:a "volume=-5dB" office1.aac
// ffmpeg -i office1.mp4 -vf "fps=25,scale=-1:240:flags=lanczos,crop=288:in_h:(in_w-288)/2:0" -q:v 11 office1.mjpeg
// auto fall back to MP3 if AAC file not available
#define AAC_FILENAME "/<FILENAME>.aac"
#define MP3_FILENAME "/<FILENAME>.mp3"
// #define MP3_FILENAME "/EP05.mp3"
#define MJPEG_FILENAME "/<FILENAME>.mjpeg"
// #define MJPEG_FILENAME "/EP05.mjpeg"
// #define MJPEG_FILENAME "/320_30fps.mjpeg"
#define FPS 25
#define MJPEG_BUFFER_SIZE (288 * 240 * 2 / 8)
// #define MJPEG_BUFFER_SIZE (320 * 240 * 2 / 8)
#define AUDIOASSIGNCORE 1
#define DECODEASSIGNCORE 0
#define DRAWASSIGNCORE 1

#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPIFFS.h>
#include <FFat.h>
#include <SD.h>
#include <SD_MMC.h>

/* Arduino_GFX */
#include <Arduino_GFX_Library.h>
#define GFX_BL DF_GFX_BL // default backlight pin, you may replace DF_GFX_BL to actual backlight pin
Arduino_DataBus *bus = create_default_Arduino_DataBus();
// Arduino_GFX *gfx = new Arduino_ILI9341(bus, DF_GFX_RST, 3 /* rotation */, false /* IPS */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, DF_GFX_RST, 1 /* rotation */, true /* IPS */, 240 /* width */, 288 /* height */, 0 /* col offset 1 */, 20 /* row offset 1 */, 0 /* col offset 2 */, 12 /* row offset 2 */);

/* variables */
static int next_frame = 0;
static int skipped_frames = 0;
static unsigned long start_ms, curr_ms, next_frame_ms;

/* audio */
#include "esp32_audio_task.h"

/* MJPEG Video */
#include "mjpeg_decode_draw_task.h"

// pixel drawing callback
static int drawMCU(JPEGDRAW *pDraw)
{
  // Serial.printf("Draw pos = (%d, %d), size = %d x %d\n", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
  unsigned long s = millis();
  gfx->draw16bitRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video_ms += millis() - s;
  return 1;
} /* drawMCU() */

void setup()
{
  disableCore0WDT();

  WiFi.mode(WIFI_OFF);
  Serial.begin(115200);
  // while (!Serial);

  // Init Display
  gfx->begin(80000000);
  gfx->fillScreen(BLACK);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  Serial.println("Init I2S");
  gfx->println("Init I2S");
#if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 25 /* SCLK */, 26 /* LRCK */, 32 /* DOUT */, -1 /* DIN */);
#elif defined(ESP32) && (CONFIG_IDF_TARGET_ESP32S2)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 4 /* SCLK */, 5 /* LRCK */, 18 /* DOUT */, -1 /* DIN */);
#elif defined(ESP32) && (CONFIG_IDF_TARGET_ESP32S3)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, 42 /* MCLK */, 46 /* SCLK */, 45 /* LRCK */, 43 /* DOUT */, 44 /* DIN */);
#elif defined(ESP32) && (CONFIG_IDF_TARGET_ESP32C3)
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 10 /* SCLK */, 19 /* LRCK */, 18 /* DOUT */, -1 /* DIN */);
#endif
  if (ret_val != ESP_OK)
  {
    Serial.printf("i2s_init failed: %d\n", ret_val);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.println("Init FS");
  gfx->println("Init FS");
  // if (!LittleFS.begin(false, "/root"))
  // if (!SPIFFS.begin(false, "/root"))
  // if (!FFat.begin(false, "/root"))
  SPIClass spi = SPIClass(HSPI);
  // spi.begin(14 /* SCK */, 2 /* MISO */, 15 /* MOSI */, 13 /* CS */);
  spi.begin(14 /* SCK */, 4 /* MISO */, 15 /* MOSI */, 13 /* CS */);
  if (!SD.begin(13, spi, 80000000))
  // if ((!SD_MMC.begin("/root")) && (!SD_MMC.begin("/root")) && (!SD_MMC.begin("/root")) && (!SD_MMC.begin("/root"))) /* 4-bit SD bus mode */
  // if ((!SD_MMC.begin("/root", true)) && (!SD_MMC.begin("/root", true)) && (!SD_MMC.begin("/root", true)) && (!SD_MMC.begin("/root", true))) /* 1-bit SD bus mode */
  {
    Serial.println("ERROR: File system mount failed!");
    gfx->println("ERROR: File system mount failed!");
  }
  else
  {
    bool aac_file_available = false;
    Serial.println("Open AAC file: " AAC_FILENAME);
    gfx->println("Open AAC file: " AAC_FILENAME);
    // File aFile = LittleFS.open(AAC_FILENAME);
    // File aFile = SPIFFS.open(AAC_FILENAME);
    // File aFile = FFat.open(AAC_FILENAME);
    File aFile = SD.open(AAC_FILENAME);
    // File aFile = SD_MMC.open(AAC_FILENAME);
    if (aFile)
    {
      aac_file_available = true;
    }
    else
    {
      Serial.println("Open MP3 file: " MP3_FILENAME);
      gfx->println("Open MP3 file: " MP3_FILENAME);
      // aFile = LittleFS.open(MP3_FILENAME);
      // aFile = SPIFFS.open(MP3_FILENAME);
      // aFile = FFat.open(MP3_FILENAME);
      aFile = SD.open(MP3_FILENAME);
      // aFile = SD_MMC.open(MP3_FILENAME);
    }

    if (!aFile || aFile.isDirectory())
    {
      Serial.println("ERROR: Failed to open " AAC_FILENAME " or " MP3_FILENAME " file for reading");
      gfx->println("ERROR: Failed to open " AAC_FILENAME " or " MP3_FILENAME " file for reading");
    }
    else
    {
      Serial.println("Open MJPEG file: " MJPEG_FILENAME);
      gfx->println("Open MJPEG file: " MJPEG_FILENAME);
      // File vFile = LittleFS.open(MJPEG_FILENAME);
      // File vFile = SPIFFS.open(MJPEG_FILENAME);
      // File vFile = FFat.open(MJPEG_FILENAME);
      File vFile = SD.open(MJPEG_FILENAME);
      // File vFile = SD_MMC.open(MJPEG_FILENAME);
      if (!vFile || vFile.isDirectory())
      {
        Serial.println("ERROR: Failed to open " MJPEG_FILENAME " file for reading");
        gfx->println("ERROR: Failed to open " MJPEG_FILENAME " file for reading");
      }
      else
      {
        Serial.println("Init video");
        gfx->println("Init video");
        mjpeg_setup(&vFile, MJPEG_BUFFER_SIZE, drawMCU,
                    false /* useBigEndian */, DECODEASSIGNCORE, DRAWASSIGNCORE);

        Serial.println("Start play audio task");
        gfx->println("Start play audio task");
        BaseType_t ret_val;
        if (aac_file_available)
        {
          ret_val = aac_player_task_start(&aFile, AUDIOASSIGNCORE);
        }
        else
        {
          ret_val = mp3_player_task_start(&aFile, AUDIOASSIGNCORE);
        }
        if (ret_val != pdPASS)
        {
          Serial.printf("Audio player task start failed: %d\n", ret_val);
          gfx->printf("Audio player task start failed: %d\n", ret_val);
        }

        Serial.println("Start play video");
        gfx->println("Start play video");
        start_ms = millis();
        curr_ms = millis();
        next_frame_ms = start_ms + (++next_frame * 1000 / FPS / 2);
        while (vFile.available() && mjpeg_read_frame()) // Read video
        {
          total_read_video_ms += millis() - curr_ms;
          curr_ms = millis();

          if (millis() < next_frame_ms) // check show frame or skip frame
          {
            // Play video
            mjpeg_draw_frame();
            total_decode_video_ms += millis() - curr_ms;
            curr_ms = millis();
          }
          else
          {
            ++skipped_frames;
            Serial.println("Skip frame");
          }

          while (millis() < next_frame_ms)
          {
            vTaskDelay(pdMS_TO_TICKS(1));
          }

          curr_ms = millis();
          next_frame_ms = start_ms + (++next_frame * 1000 / FPS);
        }
        int time_used = millis() - start_ms;
        int total_frames = next_frame - 1;
        Serial.println("AV end");
        vFile.close();
        aFile.close();

        int played_frames = total_frames - skipped_frames;
        float fps = 1000.0 * played_frames / time_used;
        total_decode_audio_ms -= total_play_audio_ms;
        // total_decode_video_ms -= total_show_video_ms;
        Serial.printf("Played frames: %d\n", played_frames);
        Serial.printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / total_frames);
        Serial.printf("Time used: %d ms\n", time_used);
        Serial.printf("Expected FPS: %d\n", FPS);
        Serial.printf("Actual FPS: %0.1f\n", fps);
        Serial.printf("Read audio: %lu ms (%0.1f %%)\n", total_read_audio_ms, 100.0 * total_read_audio_ms / time_used);
        Serial.printf("Decode audio: %lu ms (%0.1f %%)\n", total_decode_audio_ms, 100.0 * total_decode_audio_ms / time_used);
        Serial.printf("Play audio: %lu ms (%0.1f %%)\n", total_play_audio_ms, 100.0 * total_play_audio_ms / time_used);
        Serial.printf("Read video: %lu ms (%0.1f %%)\n", total_read_video_ms, 100.0 * total_read_video_ms / time_used);
        Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video_ms, 100.0 * total_decode_video_ms / time_used);
        Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video_ms, 100.0 * total_show_video_ms / time_used);

#define CHART_MARGIN 64
#define LEGEND_A_COLOR 0x1BB6
#define LEGEND_B_COLOR 0xFBE1
#define LEGEND_C_COLOR 0x2D05
#define LEGEND_D_COLOR 0xD125
#define LEGEND_E_COLOR 0x9337
#define LEGEND_F_COLOR 0x8AA9
#define LEGEND_G_COLOR 0xE3B8
#define LEGEND_H_COLOR 0x7BEF
#define LEGEND_I_COLOR 0xBDE4
#define LEGEND_J_COLOR 0x15F9
        // gfx->setCursor(0, 0);
        gfx->setTextColor(WHITE);
        gfx->printf("Played frames: %d\n", played_frames);
        gfx->printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / total_frames);
        gfx->printf("Time used: %d ms\n", time_used);
        gfx->printf("Expected FPS: %d\n", FPS);
        gfx->printf("Actual FPS: %0.1f\n\n", fps);

        int16_t r1 = ((gfx->height() - CHART_MARGIN - CHART_MARGIN) / 2);
        int16_t r2 = r1 / 2;
        int16_t cx = gfx->width() - r1 - 10;
        int16_t cy = r1 + CHART_MARGIN;

        float arc_start1 = 0;
        float arc_end1 = arc_start1 + max(2.0, 360.0 * total_read_audio_ms / time_used);
        for (int i = arc_start1 + 1; i < arc_end1; i += 2)
        {
          gfx->fillArc(cx, cy, r1, r2, arc_start1 - 90.0, i - 90.0, LEGEND_A_COLOR);
        }
        gfx->fillArc(cx, cy, r1, r2, arc_start1 - 90.0, arc_end1 - 90.0, LEGEND_A_COLOR);
        gfx->setTextColor(LEGEND_A_COLOR);
        gfx->printf("Read audio: %lu ms (%0.1f %%)\n", total_read_audio_ms, 100.0 * total_read_audio_ms / time_used);

        float arc_start2 = arc_end1;
        float arc_end2 = arc_start2 + max(2.0, 360.0 * total_decode_audio_ms / time_used);
        for (int i = arc_start2 + 1; i < arc_end2; i += 2)
        {
          gfx->fillArc(cx, cy, r1, r2, arc_start2 - 90.0, i - 90.0, LEGEND_B_COLOR);
        }
        gfx->fillArc(cx, cy, r1, r2, arc_start2 - 90.0, arc_end2 - 90.0, LEGEND_B_COLOR);
        gfx->setTextColor(LEGEND_B_COLOR);
        gfx->printf("Decode audio: %lu ms (%0.1f %%)\n", total_decode_audio_ms, 100.0 * total_decode_audio_ms / time_used);
        gfx->setTextColor(LEGEND_J_COLOR);
        gfx->printf("Play audio: %lu ms (%0.1f %%)\n", total_play_audio_ms, 100.0 * total_play_audio_ms / time_used);

        float arc_start3 = arc_end2;
        float arc_end3 = arc_start3 + max(2.0, 360.0 * total_read_video_ms / time_used);
        for (int i = arc_start3 + 1; i < arc_end3; i += 2)
        {
          gfx->fillArc(cx, cy, r1, r2, arc_start3 - 90.0, i - 90.0, LEGEND_C_COLOR);
        }
        gfx->fillArc(cx, cy, r1, r2, arc_start3 - 90.0, arc_end3 - 90.0, LEGEND_C_COLOR);
        gfx->setTextColor(LEGEND_C_COLOR);
        gfx->printf("Read video: %lu ms (%0.1f %%)\n", total_read_video_ms, 100.0 * total_read_video_ms / time_used);

        float arc_start4 = arc_end3;
        float arc_end4 = arc_start4 + max(2.0, 360.0 * total_show_video_ms / time_used);
        for (int i = arc_start4 + 1; i < arc_end4; i += 2)
        {
          gfx->fillArc(cx, cy, r1, r2, arc_start4 - 90.0, i - 90.0, LEGEND_D_COLOR);
        }
        gfx->fillArc(cx, cy, r1, r2, arc_start4 - 90.0, arc_end4 - 90.0, LEGEND_D_COLOR);
        gfx->setTextColor(LEGEND_D_COLOR);
        gfx->printf("Show video: %lu ms (%0.1f %%)\n", total_show_video_ms, 100.0 * total_show_video_ms / time_used);

        float arc_start5 = 0;
        float arc_end5 = arc_start5 + max(2.0, 360.0 * total_decode_video_ms / time_used);
        for (int i = arc_start5 + 1; i < arc_end5; i += 2)
        {
          gfx->fillArc(cx, cy, r2, 0, arc_start5 - 90.0, i - 90.0, LEGEND_E_COLOR);
        }
        gfx->fillArc(cx, cy, r2, 0, arc_start5 - 90.0, arc_end5 - 90.0, LEGEND_E_COLOR);
        gfx->setTextColor(LEGEND_E_COLOR);
        gfx->printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video_ms, 100.0 * total_decode_video_ms / time_used);
      }
      // delay(60000);
#ifdef GFX_BL
      // digitalWrite(GFX_BL, LOW);
#endif
      // gfx->displayOff();
      // esp_deep_sleep_start();
    }
  }
}

void loop()
{
}
