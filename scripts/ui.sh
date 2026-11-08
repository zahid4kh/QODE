#!/usr/bin/env bash
# Sourced by the build scripts: coloured, aligned terminal output. Colours only on a terminal, and never with NO_COLOR.
#   banner "Title" "subtitle"     a heading for the whole run
#   step "text"                    one numbered stage (==> 1/4 text)
#   info "text" / ok / warn / die  a detail line, a success, a warning, an error (die exits 1)
#   kv "Key" "value"               an aligned key / value line
#   summary_box "Title" "line"...  a framed block at the end of a run

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RESET=$'\e[0m' C_BOLD=$'\e[1m' C_DIM=$'\e[2m'
    C_ACCENT=$'\e[38;5;75m' C_GREEN=$'\e[32m' C_YELLOW=$'\e[33m' C_RED=$'\e[31m'
else
    C_RESET='' C_BOLD='' C_DIM='' C_ACCENT='' C_GREEN='' C_YELLOW='' C_RED=''
fi

UI_STEP=0
UI_STEPS="${UI_STEPS:-0}" # set before the first step to show "n/total"
UI_START=$SECONDS

banner() {
    echo
    echo "  ${C_BOLD}${C_ACCENT}▌ $1${C_RESET}"
    [ -z "${2:-}" ] || echo "  ${C_ACCENT}▌${C_RESET} ${C_DIM}$2${C_RESET}"
    echo
}

step() {
    UI_STEP=$((UI_STEP + 1))
    local n="$UI_STEP"
    [ "$UI_STEPS" -gt 0 ] && n="$UI_STEP/$UI_STEPS"
    echo "${C_BOLD}${C_ACCENT}==>${C_RESET} ${C_DIM}[$n]${C_RESET} ${C_BOLD}$*${C_RESET}"
}

info() { echo "    ${C_DIM}$*${C_RESET}"; }
ok()   { echo "    ${C_GREEN}✔${C_RESET} $*"; }
warn() { echo "    ${C_YELLOW}!${C_RESET} ${C_YELLOW}$*${C_RESET}" >&2; }
die()  { echo "${C_RED}${C_BOLD}✘ $*${C_RESET}" >&2; exit 1; }

kv() { printf "    ${C_DIM}%-13s${C_RESET} %s\n" "$1" "$2"; } # longer keys push their value out

# Frames its lines; a line "Key|value" is printed as an aligned pair. Keep the lines ASCII: the padding counts bytes.
summary_box() {
    local title="$1"; shift
    local line key width=0 keyw=12 rows=()
    for line in "$@"; do
        [[ "$line" == *"|"* ]] || continue
        key="${line%%|*}"
        [ "${#key}" -le "$keyw" ] || keyw=${#key}
    done
    for line in "$@"; do
        if [[ "$line" == *"|"* ]]; then
            printf -v line '%-*s %s' "$keyw" "${line%%|*}" "${line#*|}"
        fi
        rows+=("$line")
        [ "${#line}" -le "$width" ] || width=${#line}
    done
    [ "${#title}" -le "$width" ] || width=${#title}
    local bar
    printf -v bar '%*s' $((width + 2)) ''
    bar="${bar// /─}"
    echo
    echo "  ${C_GREEN}╭${bar}╮${C_RESET}"
    printf "  ${C_GREEN}│${C_RESET} ${C_BOLD}%-*s${C_RESET} ${C_GREEN}│${C_RESET}\n" "$width" "$title"
    echo "  ${C_GREEN}├${bar}┤${C_RESET}"
    for line in "${rows[@]}"; do
        printf "  ${C_GREEN}│${C_RESET} %-*s ${C_GREEN}│${C_RESET}\n" "$width" "$line"
    done
    echo "  ${C_GREEN}╰${bar}╯${C_RESET}"
    local secs=$((SECONDS - UI_START))
    echo "  ${C_DIM}finished in $((secs / 60))m $((secs % 60))s${C_RESET}"
    echo
}
