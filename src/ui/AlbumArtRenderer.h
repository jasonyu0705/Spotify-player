#pragma once

#include <TFT_eSPI.h>
#include "backend/NowPlaying.h"

bool drawAlbumArt(TFT_eSPI &tft, const AlbumArt &image);
