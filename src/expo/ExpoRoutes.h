#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// One screen of an Expo Router app, from its file under app/: "(tabs)/chats/[matchId].tsx" is "/chats/[matchId]".
struct ExpoRoute
{
    QString path;       // URL path with the dynamic segments kept as written: "/", "/friend/[id]", "/docs/[...slug]"
    QString file;       // relative to the app folder
    QStringList params; // the dynamic segments' names in order ("id", "...slug", "[...slug]" for an optional catch-all)
};

// Lists an Expo Router app's routes from its file names, without running anything. Qt Core only.
namespace ExpoRoutes {
// The routes folder ("src/app", "app", or the expo-router plugin's `root`) relative to the app, "" when the app does
// not use expo-router (React Navigation apps declare their screens in code; there is nothing to list).
QString routesDir(const QString &appDir);
// Every route, "/" first then by path; layouts, +html / +not-found / +api files and platform duplicates are left out.
QList<ExpoRoute> scan(const QString &appDir);
}
