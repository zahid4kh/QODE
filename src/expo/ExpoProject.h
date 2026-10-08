#pragma once

#include <QList>
#include <QString>

// One Expo (React Native) app: a folder whose package.json depends on `expo`. A monorepo has several.
struct ExpoApp
{
    QString dir;           // absolute folder
    QString rel;           // relative to the project root ("" = the root itself)
    QString name;          // expo.name from app.json, else the package name, else the folder name
    QString sdk;           // "54.0.0": the schema version for the installed (or declared) expo, "" when unknown
    QString appJson;       // absolute app.json (or app.config.json), "" when the app only has a dynamic config
    QString dynamicConfig; // absolute app.config.js / .ts when present
};

// Finds Expo apps in a project. QODE shows nothing React-Native-specific unless this returns something. Qt Core only.
namespace ExpoProject {
// The root and the folders below it (three levels, skipping node_modules, native folders, build output...) holding an
// Expo app, shallowest first. Plain React Native projects without `expo` are not reported.
QList<ExpoApp> detect(const QString &root);
}
