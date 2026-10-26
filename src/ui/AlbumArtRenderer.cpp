#include "ui/AlbumArtRenderer.h"

#include <TJpg_Decoder.h>
#include "ui/ArtGeometry.h"
#include "ui/Theme.h"

namespace {
// Only the UI task calls the decoder. No full RGB framebuffer is allocated.
TFT_eSPI *display;
int imageWidth, imageHeight, drawWidth, drawHeight, drawX, drawY;

bool outputBlock(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *pixels) {
  const int x0 = artBlockEdge(x, imageWidth, drawWidth);
  const int x1 = artBlockEdge(x + width, imageWidth, drawWidth);
  const int y0 = artBlockEdge(y, imageHeight, drawHeight);
  const int y1 = artBlockEdge(y + height, imageHeight, drawHeight);
  const int outputWidth = x1 - x0;
  if (!outputWidth) return true;
  uint16_t block[512];
  const int rowsPerBatch = 512 / outputWidth;
  for (int start = y0; start < y1; start += rowsPerBatch) {
    int rows = min(rowsPerBatch, y1 - start);
    for (int row = 0; row < rows; ++row) {
      int sy = artSourcePixel(start + row, imageHeight, drawHeight) - y;
      for (int dx = x0; dx < x1; ++dx) {
        int sx = artSourcePixel(dx, imageWidth, drawWidth) - x;
        block[row * outputWidth + dx - x0] = pixels[sy * width + sx];
      }
    }
    display->pushImage(drawX + x0, drawY + start, outputWidth, rows, block);
  }
  return true;
}
}  // namespace

bool drawAlbumArt(TFT_eSPI &tft, const AlbumArt &image) {
  uint32_t started = millis();
  if (!image.jpeg || !image.length) return false;
  uint16_t width = 0, height = 0;
  if (TJpgDec.getJpgSize(&width, &height, image.jpeg.get(), image.length) != JDR_OK ||
      !width || !height || width > 1024 || height > 1024) return false;
  imageWidth = width;
  imageHeight = height;
  drawWidth = width >= height ? layout::ART_SIZE : int(width) * layout::ART_SIZE / height;
  drawHeight = height >= width ? layout::ART_SIZE : int(height) * layout::ART_SIZE / width;
  if (!drawWidth || !drawHeight) return false;
  drawX = layout::ART_X + (layout::ART_SIZE - drawWidth) / 2;
  drawY = layout::ART_Y + (layout::ART_SIZE - drawHeight) / 2;
  display = &tft;
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(outputBlock);
  bool wasSwapped = tft.getSwapBytes();
  tft.setSwapBytes(true);
  tft.fillRect(layout::ART_X, layout::ART_Y, layout::ART_SIZE, layout::ART_SIZE, theme::BG);
  JRESULT result = TJpgDec.drawJpg(0, 0, image.jpeg.get(), image.length);
  tft.setSwapBytes(wasSwapped);
  display = nullptr;
  Serial.printf("Album art: rendered %ux%u -> %dx%d in %lu ms (decoder %d)\n", width, height,
                drawWidth, drawHeight, (unsigned long)(millis() - started), int(result));
  return result == JDR_OK;
}
