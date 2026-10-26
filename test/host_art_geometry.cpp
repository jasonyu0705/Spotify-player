#include "../src/ui/ArtGeometry.h"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
  for (int source : {1, 8, 64, 150, 299, 300, 640, 1024}) {
    for (int destination : {1, 37, 132, 224, 264}) {
      for (int block : {8, 16}) {
        std::vector<int> coverage(destination, 0);
        for (int start = 0; start < source; start += block) {
          int end = start + block < source ? start + block : source;
          int a = artBlockEdge(start, source, destination);
          int b = artBlockEdge(end, source, destination);
          assert(a >= 0 && b <= destination);
          for (int pixel = a; pixel < b; ++pixel) {
            int sample = artSourcePixel(pixel, source, destination);
            assert(sample >= start && sample < end);
            ++coverage[pixel];
          }
        }
        // Every output pixel appears exactly once, including cropped edge MCUs,
        // and every sample stays within its decoder-owned input block.
        for (int count : coverage) assert(count == 1);
      }
    }
  }
  // Exercise the batched renderer with downscaling, upscaling, partial MCUs,
  // and nonsquare artwork. A strip must fit its fixed 512-pixel scratch buffer.
  for (auto dimensions : {std::pair<int, int>{300, 300}, {64, 64}, {640, 480}, {299, 197}}) {
    int sw = dimensions.first, sh = dimensions.second;
    int dw = sw >= sh ? 264 : sw * 264 / sh;
    int dh = sh >= sw ? 264 : sh * 264 / sw;
    std::vector<int> coverage(dw * dh, 0);
    for (int y = 0; y < sh; y += 16) {
      for (int x = 0; x < sw; x += 16) {
        int width = std::min(16, sw - x), height = std::min(16, sh - y);
        int x0 = artBlockEdge(x, sw, dw), x1 = artBlockEdge(x + width, sw, dw);
        int y0 = artBlockEdge(y, sh, dh), y1 = artBlockEdge(y + height, sh, dh);
        int outputWidth = x1 - x0;
        if (!outputWidth) continue;
        int batch = 512 / outputWidth;
        assert(batch > 0);
        for (int start = y0; start < y1; start += batch) {
          int rows = std::min(batch, y1 - start);
          assert(rows * outputWidth <= 512);
          for (int row = 0; row < rows; ++row) {
            int sy = artSourcePixel(start + row, sh, dh) - y;
            assert(sy >= 0 && sy < height);
            for (int dx = x0; dx < x1; ++dx) {
              int sx = artSourcePixel(dx, sw, dw) - x;
              assert(sx >= 0 && sx < width);
              ++coverage[(start + row) * dw + dx];
            }
          }
        }
      }
    }
    for (int count : coverage) assert(count == 1);
  }
  std::puts("PASS: JPEG blocks cover resized images without gaps, overlap, or out-of-bounds reads");
}
