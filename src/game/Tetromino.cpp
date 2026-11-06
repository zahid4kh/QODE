// The seven pieces of Teqris: shapes, colours, wall kicks.
#include "game/Tetromino.h"

namespace {

// Spawn orientation of each piece, y pointing down.
const std::array<std::array<QPoint, 4>, Tetromino::Count> BaseShapes = {{
    {{{0, 1}, {1, 1}, {2, 1}, {3, 1}}}, // I
    {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}, // O
    {{{1, 0}, {0, 1}, {1, 1}, {2, 1}}}, // T
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}}}, // S
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}}, // Z
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}}, // J
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}}}, // L
}};

// Order: 0->1, 1->0, 1->2, 2->1, 2->3, 3->2, 3->0, 0->3
const std::array<std::array<QPoint, 5>, 8> KicksJLSTZ = {{
    {{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}},
    {{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}},
    {{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}},
    {{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}},
    {{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}},
    {{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}},
    {{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}},
    {{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}},
}};

const std::array<std::array<QPoint, 5>, 8> KicksI = {{
    {{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}},
    {{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}},
    {{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}},
    {{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}},
    {{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}},
    {{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}},
    {{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}},
    {{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}},
}};

int boxSize(int type)
{
    switch (type) {
    case Tetromino::I:
        return 4;
    case Tetromino::O:
        return 2;
    default:
        return 3;
    }
}

} // namespace

namespace Tetromino {

std::array<QPoint, 4> cells(int type, int rotation)
{
    std::array<QPoint, 4> result = BaseShapes[type];
    const int n = boxSize(type);
    for (int r = 0; r < (rotation & 3); ++r) {
        for (QPoint &p : result)
            p = QPoint(n - 1 - p.y(), p.x());
    }
    return result;
}

const std::array<QPoint, 5> &kicks(int type, int from, int to)
{
    const bool clockwise = to == (from + 1) % 4;
    const int index = clockwise ? from * 2 : ((from + 3) % 4) * 2 + 1;
    return type == I ? KicksI[index] : KicksJLSTZ[index];
}

QColor color(int type)
{
    static const QColor colors[Count] = {
        QColor(34, 232, 255), // I
        QColor(255, 222, 58), // O
        QColor(192, 86, 255), // T
        QColor(61, 255, 138), // S
        QColor(255, 59, 107), // Z
        QColor(59, 123, 255), // J
        QColor(255, 154, 46), // L
    };
    return type >= 0 && type < Count ? colors[type] : QColor(120, 120, 140);
}

} // namespace Tetromino
