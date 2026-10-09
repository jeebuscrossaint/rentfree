# fish completions for rentfree
#
# Every candidate list is asked of the binary itself (rentfree --complete-*), so
# new settings, new notes and new tasks appear here without this file changing.

command -q rentfree; or exit 0

# The setting a `set`/`get`/`unset` is waiting on a value for; fails while the
# key itself is still being typed.
function __rentfree_pending_key
    set -l toks (commandline -opc)
    for verb in set get unset
        set -l i (contains -i -- $verb $toks); or continue
        test (count $toks) -gt $i; or return 1
        echo $toks[(math $i + 1)]
        return 0
    end
    return 1
end

# The -n the user already typed, so task lists follow the note they mean.
function __rentfree_note_flag
    set -l toks (commandline -opc)
    for flag in -n --note
        set -l i (contains -i -- $flag $toks); or continue
        test (count $toks) -gt $i; or return 1
        echo $toks[(math $i + 1)]
        return 0
    end
    return 1
end

function __rentfree_tasks
    set -l note (__rentfree_note_flag)
    if test -n "$note"
        rentfree -n $note --complete-tasks 2>/dev/null
    else
        rentfree --complete-tasks 2>/dev/null
    end
end

# rentfree takes words, not paths.
complete -c rentfree -f

# --- commands --------------------------------------------------------------
complete -c rentfree -n __fish_use_subcommand -a "(rentfree --complete-commands)"

# --- global options --------------------------------------------------------
complete -c rentfree -s n -l note -x -a "(rentfree --complete-notes)" -d "Note to act on"
complete -c rentfree -s f -l foreground -d "Run the daemon in the foreground"
complete -c rentfree -s h -l help -d "Show help"
complete -c rentfree -s v -l version -d "Show the version"
complete -c rentfree -l file -r -F -d "Todo file to use"
complete -c rentfree -l config -r -F -d "Config file to use"

# --- set / get / unset: a key, then that key's legal values -----------------
complete -c rentfree -x \
    -n 'not __rentfree_pending_key; and __fish_seen_subcommand_from set get unset' \
    -a "(rentfree --complete-keys)"
complete -c rentfree -x -n __rentfree_pending_key \
    -a "(rentfree --complete-values (__rentfree_pending_key))"

# --- task numbers, shown with their text -----------------------------------
complete -c rentfree -x -n '__fish_seen_subcommand_from done undone rm' \
    -a "(__rentfree_tasks)"

# --- subcommand words -------------------------------------------------------
complete -c rentfree -x -n '__fish_seen_subcommand_from config' \
    -a "path\t'Print the config file path' edit\t'Open the config in \$EDITOR'"
complete -c rentfree -x -n '__fish_seen_subcommand_from clear' \
    -a "--all\t'Delete everything, not just the ticked tasks'"

# --- every setting is also a --flag ----------------------------------------
set -l __rf_bools (rentfree --complete-bools)
for __rf_line in (rentfree --complete-keys)
    set -l __rf_parts (string split -m1 \t -- $__rf_line)
    set -l __rf_key $__rf_parts[1]
    complete -c rentfree -l $__rf_key -x \
        -a "(rentfree --complete-values $__rf_key)" -d "$__rf_parts[2]"
    if contains -- $__rf_key $__rf_bools
        complete -c rentfree -l "no-$__rf_key" -d "$__rf_parts[2] (off)"
    end
end
set -e __rf_bools __rf_line __rf_parts __rf_key
