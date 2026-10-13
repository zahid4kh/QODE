#pragma once

#include <QString>

// Where QODE finds the Gemini API key. Qt Core only.
//   1. the key saved from the AI chat's "API key" dialog (~/.config/QODE/gemini.key, mode 0600)
//   1b. the GEMINI_API_KEY environment variable
//   2. a `.env.local` (or `.env`) line GEMINI_API_KEY=... in the project root, the folder QODE was started from, the
//      folders above the executable (so a build/ run finds the repository's file) or ~/.config/QODE
namespace AiEnv {

// Value of `key` (or the single bare line of a file that holds only the key) in the text of a dotenv file ("export " prefix, quotes and trailing comments are understood).
QString parseValue(const QString &text, const QString &key);

// The key, or "" when none is configured. `source` receives where it came from (for the status line).
QString apiKey(const QString &projectRoot, QString *source = nullptr);

// The key typed into the dialog lives in its own owner-only file (never in QODE.conf, never in a project folder).
QString storedKeyPath();
bool saveKey(const QString &key, QString *error = nullptr);
bool clearKey();
bool hasStoredKey();

// Files whose content must never be sent to the model (dotenv files, private keys, credentials). Matches on the file name.
bool isSensitive(const QString &path);

} // namespace AiEnv
