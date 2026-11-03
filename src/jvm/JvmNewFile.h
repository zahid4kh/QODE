#pragma once

#include <QList>
#include <QString>

// New Kotlin / Java files (New Kotlin Class/File, New Java Class): the kinds offered, the package a folder implies and
// the text of the new file. Qt Core only; nothing is written here.
namespace JvmNewFile {

enum class Kind {
    Class, File, Interface, DataClass, ValueClass, SealedClass, SealedInterface, EnumClass, Object, Annotation, // Kotlin
    JavaClass, JavaAbstractClass, JavaInterface, JavaRecord, JavaEnum, JavaAnnotation, JavaException            // Java
};

struct KindInfo {
    Kind kind;
    QString title;  // "Data class"
    QChar badge;    // letter on the kind's icon
    int tone = 0;   // 0 class, 1 interface, 2 enum / object, 3 annotation, 4 file / record
};
QList<KindInfo> kinds(bool kotlin);

// The package a new file in `dir` gets: the package a Kotlin / Java file already in the folder declares (Kotlin folders
// may leave out a root package), else the one the folder implies below src/<set>/java|kotlin, else "".
// `known` is false when nothing tells (no source root, no sibling file).
QString packageFor(const QString &dir, bool *known = nullptr);
// Whether the folder lies below a Java / Kotlin source root, or holds Java / Kotlin files.
bool isSourceFolder(const QString &dir);
// Whether Kotlin belongs in the folder: a kotlin/ source root, a .kt file next to it, or a module build that applies Kotlin.
bool kotlinFits(const QString &dir);
bool javaFits(const QString &dir); // a java/ source root, or .java files in the folder

struct Result {
    QString path;    // the file to create
    QString content;
    int caretLine = 0, caretColumn = 0; // 0-based, where typing continues
    QString error;
    bool ok() const { return error.isEmpty(); }
};
// `input` is a name ("UserService") or a dotted path below the folder's package ("api.dto.UserDto": the folders
// api/dto are created and the package grows to match). A Kotlin "File" may also take a lower-case name.
Result generate(Kind kind, const QString &dir, const QString &input);

} // namespace JvmNewFile
