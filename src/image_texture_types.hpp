#pragma once
#include <vector>
struct ImageTexture {
    struct Tile {
        unsigned int id = 0;
        int x = 0, y = 0, w = 0, h = 0;
        int cx = 0, cy = 0, cw = 0, ch = 0;
        bool dirty = false;
    };
    int width = 0, height = 0;
    std::vector<Tile> tiles;
};
