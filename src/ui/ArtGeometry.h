#pragma once

// First destination pixel whose nearest-neighbor source lies in this block.
// Ceil, rather than rounding, gives adjacent JPEG blocks gap-free boundaries.
inline int artBlockEdge(int sourceEdge, int sourceSize, int destinationSize) {
  return (sourceEdge * destinationSize + sourceSize - 1) / sourceSize;
}

inline int artSourcePixel(int destination, int sourceSize, int destinationSize) {
  return destination * sourceSize / destinationSize;
}
