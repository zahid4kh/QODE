#pragma once

#include <QString>

struct Project {
    QString root; // absolute, canonical
    QString name;

    bool isValid() const { return !root.isEmpty(); }
};
