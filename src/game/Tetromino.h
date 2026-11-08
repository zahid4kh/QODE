#pragma once

#include <QColor>
#include <QPoint>

#include <array>

// The seven tetrominoes. Types are plain ints (0..6) so they can be stored in the board grid,
// where -1 means an empty cell.
namespace Tetromino {

enum Type { I, O, T, S, Z, J, L };
constexpr int Count = 7;

// Cells of a piece in its bounding box for rotation 0..3 (0 = spawn, 1 = clockwise, ...).
std::array<QPoint, 4> cells(int type, int rotation);

// Super Rotation System wall kick offsets for rotating from -> to. Offsets use y-up, like the
// published SRS tables.
const std::array<QPoint, 5> &kicks(int type, int from, int to);

QColor color(int type);

} // namespace Tetromino
