#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Starter projects for File > New Project. A template is a file tree embedded in the binary (resources/templates/<id>/files)
// plus a manifest (template.json) naming the options the dialog asks for. Shared trees ("mixins", e.g. the Gradle wrapper)
// live in resources/templates/_shared and dependency versions in _shared/versions.json.
//
// Text files are rendered with {{key}} placeholders; a line holding only {{#if key}} / {{#if !key}} ... {{/if}} includes the
// lines between it and its end marker only when the value is set (non-empty, not "false"). File and folder names may hold
// __key__ (e.g. __packagePath__) and a leading "dot_" becomes ".". Needs Qt Gui for the icon conversion, nothing else.
class ProjectTemplates
{
public:
    struct Option {
        QString key;
        QString label;
        QString type;  // text | package | version | choice | bool | image
        QString def;   // may hold placeholders ({{nameId}}, {{gitIdentity}} ...)
        QString hint;
        QStringList choices;
        bool required = false;
    };

    struct ImageOutput {
        QString path;   // relative to the project; may hold placeholders and {ext} (the source's extension)
        QString format; // png | ico | copy
        int size = 0;   // png: edge in pixels
        QList<int> sizes; // ico: one frame per edge
    };

    struct Template {
        QString id;
        QString category;
        QString name;
        QString description;
        int order = 0;
        QStringList mixins;
        // Tools the project needs, checked by the dialog (a warning, never a block): "jdk17" = a Java runtime of at least that
        // major version, "tool:<exe>" = an executable on PATH (<exe> may hold {{placeholders}}), "qt6" = Qt 6 development files.
        // A "?key=value" suffix applies the entry only while that option has that value.
        QStringList needs;
        QStringList executable; // files that get the executable bit
        QStringList openFiles;  // opened after creation (relative, may hold placeholders)
        QString runCommand;     // stored as the project's run command ("./run.sh"), empty = leave it to the detectors
        QMap<QString, QString> when; // file or folder (relative, rendered) -> option key, "!key" = only when unset
        QList<Option> options;
        QMap<QString, QList<ImageOutput>> imageOutputs; // by the key of an image option
    };

    using Values = QMap<QString, QString>;

    struct Result {
        bool ok = false;
        QString error;
        QStringList openFiles; // absolute paths
        QString runCommand;
    };

    static const QList<Template> &all();
    static const Template *find(const QString &id);

    // The value an option starts with for the project `name`, given what the other options hold now.
    static QString defaultValue(const Template &t, const Option &o, const Values &current);
    // The text of `text` with every {{key}} replaced from the template's built-in values and `values`.
    static QString expand(const Template &t, const QString &text, const Values &values);

    // An empty string when `values` (the project "name" plus one entry per option) can create a project, else why not.
    static QString validate(const Template &t, const Values &values);

    // Writes the project into `destDir` (absent or an empty folder). A failure leaves nothing behind.
    static Result instantiate(const Template &t, const Values &values, const QString &destDir);

    // Lower-case letters and digits of `text` ("My App 2" -> "myapp2"); never empty, never starts with a digit.
    static QString identifier(const QString &text);
    // "Name <email>" from the global git configuration, or an empty string.
    static QString gitIdentity();
};
