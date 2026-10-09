# bash completion for rentfree
#
# Candidate lists come from `rentfree --complete-*`, so this file doesn't need
# updating when settings are added. Descriptions are cut off at the tab.

_rentfree() {
    local cur prev words cword
    if declare -F _init_completion >/dev/null 2>&1; then
        _init_completion -n = || return
    else
        cur=${COMP_WORDS[COMP_CWORD]}
        prev=${COMP_WORDS[COMP_CWORD-1]}
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    fi


    # A path is wanted after these.
    case $prev in
        --file|--config) _filedir; return ;;
        -n|--note)
            COMPREPLY=($(compgen -W "$(rentfree --complete-notes 2>/dev/null | cut -f1)" -- "$cur"))
            return ;;
    esac

    # The word after set/get/unset is a setting; the one after that is a value.
    local i verb= key=
    for ((i=1; i<cword; i++)); do
        case ${words[i]} in
            set|get|unset) verb=${words[i]}; key=${words[i+1]}; break ;;
        esac
    done
    if [[ -n $verb ]]; then
        if (( cword == i+1 )); then
            COMPREPLY=($(compgen -W "$(rentfree --complete-keys 2>/dev/null | cut -f1)" -- "$cur"))
        elif (( cword == i+2 )) && [[ -n $key ]]; then
            COMPREPLY=($(compgen -W "$(rentfree --complete-values "$key" 2>/dev/null | cut -f1)" -- "$cur"))
        fi
        return
    fi

    # Task numbers for the commands that take one.
    for ((i=1; i<cword; i++)); do
        case ${words[i]} in
            done|undone|rm)
                COMPREPLY=($(compgen -W "$(rentfree --complete-tasks 2>/dev/null | cut -f1)" -- "$cur"))
                return ;;
            config)
                COMPREPLY=($(compgen -W "path edit" -- "$cur"))
                return ;;
        esac
    done

    # Any setting can be given as a --flag, plus --no- forms for the booleans.
    if [[ $cur == --* ]]; then
        local keys flags
        keys=$(rentfree --complete-keys 2>/dev/null | cut -f1)
        flags="--note --foreground --file --config --help --version"
        for k in $keys; do
            flags+=" --$k"
            [[ $(rentfree --complete-values "$k" 2>/dev/null | tr '\n' ' ') == "true false " ]] && flags+=" --no-$k"
        done
        COMPREPLY=($(compgen -W "$flags" -- "$cur"))
        return
    fi

    COMPREPLY=($(compgen -W "$(rentfree --complete-commands 2>/dev/null | cut -f1)" -- "$cur"))
}
complete -F _rentfree rentfree
