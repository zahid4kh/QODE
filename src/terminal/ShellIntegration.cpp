#include "ShellIntegration.h"
#include "platform/AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

// bash: started with --rcfile this file instead of ~/.bashrc (/etc/bash.bashrc is still read before it). PS0 runs at
// the start of each command (not for an empty line), PROMPT_COMMAND before each prompt: ours goes first so it sees the
// command's $? and hands it on unchanged. The command line comes from the history; one kept out of it (a leading space
// with ignorespace, a repeat with ignoredups) leaves HISTCMD where the prompt saw it, and is sent as "".
const char *const kBash = R"(# QODE shell integration (bash): marks each command's start and exit code for the terminal.
if [ -r ~/.bashrc ]; then . ~/.bashrc; fi
__qode_preexec() {
    local c=
    if [[ $HISTCMD != "$__qode_histcmd" ]]; then
        c=$(HISTTIMEFORMAT= builtin history 1)
        [[ $c =~ ^[[:space:]]*[0-9]+[*]?[[:space:]]+(.*)$ ]] && c=${BASH_REMATCH[1]}
    fi
    builtin printf '\e]133;C\a\e]633;E;%s\a' "${c//[[:cntrl:]]/ }"
}
__qode_precmd() {
    local s=$?
    builtin printf '\e]133;D;%s\a' "$s"
    __qode_histcmd=$HISTCMD
    return $s
}
PS0='$(__qode_preexec)'"${PS0-}"
PROMPT_COMMAND="__qode_precmd${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
)";

// zsh: ZDOTDIR points here, so these two files are read instead of the user's; each reads the user's own first (from
// their ZDOTDIR, or $HOME) and .zshrc gives ZDOTDIR back, so shells started later read only the user's files.
const char *const kZshEnv = R"(# QODE shell integration (zsh): the user's .zshenv, then back to this folder for .zshrc.
QODE_ZDOTDIR=$ZDOTDIR
ZDOTDIR=${QODE_USER_ZDOTDIR:-$HOME}
[[ -f $ZDOTDIR/.zshenv ]] && source $ZDOTDIR/.zshenv
QODE_USER_ZDOTDIR=$ZDOTDIR
ZDOTDIR=$QODE_ZDOTDIR
)";

const char *const kZshRc = R"(# QODE shell integration (zsh): the user's .zshrc, then marks for each command's start and exit code.
ZDOTDIR=$QODE_USER_ZDOTDIR
unset QODE_USER_ZDOTDIR QODE_ZDOTDIR
[[ -f $ZDOTDIR/.zshrc ]] && source $ZDOTDIR/.zshrc
__qode_preexec() { print -rn -- $'\e]133;C\a\e]633;E;'"${1//[[:cntrl:]]/ }"$'\a' }
__qode_precmd() { local s=$?; print -rn -- $'\e]133;D;'"$s"$'\a'; return $s }
precmd_functions=(__qode_precmd $precmd_functions)
preexec_functions+=(__qode_preexec)
)";

// fish: sourced through --init-command, after the user's config.
const char *const kFish = R"(# QODE shell integration (fish): marks each command's start and exit code for the terminal.
function __qode_preexec --on-event fish_preexec
    printf '\e]133;C\a\e]633;E;%s\a' (string replace -ra '[[:cntrl:]]' ' ' -- "$argv")
end
function __qode_postexec --on-event fish_postexec
    printf '\e]133;D;%s\a' $status
end
)";

// Writes `text` to `path` unless it already holds it.
bool ensureFile(const QString &path, const char *text)
{
    const QByteArray want(text);
    QFile current(path);
    if (current.open(QIODevice::ReadOnly) && current.readAll() == want)
        return true;
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(want);
    return f.commit();
}

} // namespace

namespace ShellIntegration {

bool apply(const QString &shell, QStringList *args, QProcessEnvironment *env)
{
    const QString dir = AppPaths::cache(QStringLiteral("shell"));
    const QString name = QFileInfo(shell).fileName();
    if (name == QLatin1String("bash")) {
        const QString rc = dir + QStringLiteral("/bashrc");
        if (!ensureFile(rc, kBash))
            return false;
        *args = {QStringLiteral("--rcfile"), rc, QStringLiteral("-i")};
        return true;
    }
    if (name == QLatin1String("zsh")) {
        const QString zdot = dir + QStringLiteral("/zsh");
        if (!ensureFile(zdot + QStringLiteral("/.zshenv"), kZshEnv) || !ensureFile(zdot + QStringLiteral("/.zshrc"), kZshRc))
            return false;
        if (env->contains(QStringLiteral("ZDOTDIR")))
            env->insert(QStringLiteral("QODE_USER_ZDOTDIR"), env->value(QStringLiteral("ZDOTDIR")));
        env->insert(QStringLiteral("ZDOTDIR"), zdot);
        *args = {QStringLiteral("-i")};
        return true;
    }
    if (name == QLatin1String("fish")) {
        const QString script = dir + QStringLiteral("/qode.fish");
        if (!ensureFile(script, kFish))
            return false;
        *args = {QStringLiteral("-i"), QStringLiteral("--init-command"), QStringLiteral("source '%1'").arg(script)};
        return true;
    }
    return false;
}

} // namespace ShellIntegration
