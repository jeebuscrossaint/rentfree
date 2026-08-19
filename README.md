# rentfree

Your todo list, living rent free on your desktop.

A Wayland sticky note that sits **above your wallpaper and below your windows**.
It's click-through, it can't take focus, and it never appears in your window
list — it's just *there*, the way a Post-it on your monitor bezel is there.

```
$ rentfree                      # start the note
$ rentfree add "call the bank"  # it appears, instantly
$ rentfree done 1               # it gets struck through
```

You can have several — a todo in one corner, groceries in another — each with
its own file, position and colours. Or just the one, with no config file at all.

## How it works

The design has one rule: **the file is the truth.**

```
   rentfree add / done / rm          todo.md      ─┐
   $EDITOR                    ──▶    shopping.md  ─┤ plain markdown checklists
   echo >> the file                  ideas.md     ─┘
                                          │
                                          │ inotify
                                          ▼
                                   rentfree daemon ──▶ one layer surface per note
                                                          (layer: bottom)
```

Nothing owns the list but the file. The CLI edits it, the daemon watches it with
inotify and redraws. That means `rentfree add` works whether or not the daemon is
running, hand-editing the file in vim behaves identically to the CLI, and the
daemon can be killed and restarted without losing a thing. The control socket
carries only what has no on-disk meaning: `show`, `hide`, `reload`, `quit`.

The todo file is a normal markdown checklist, so it stays comfortable to edit by
hand:

```markdown
# today
- [x] make rentfree work
- [ ] buy milk
- [ ] a longer note that wraps on its own
```

Parsing is deliberately forgiving — a bare line with no `- [ ]` is read as an
unfinished task, so you can just type into the file. Writes are normalised, and
atomic (temp file + `rename`), so the daemon never catches a half-written list.

## Building

Needs `wayland-client`, `wayland-protocols`, `pangocairo` and a compositor that
speaks `wlr-layer-shell` (anything wlroots-based — sway, river, Hyprland, dwl,
mango — plus KDE; GNOME does **not** support it).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build      # optional
```

## Usage

Run `rentfree` once and it forks into the background. Put it in your
compositor's autostart and forget about it.

**Editing the list** (these write the file; the note follows):

| | |
|---|---|
| `rentfree add <text>` | append a task |
| `rentfree header <text>` | append a section heading |
| `rentfree done <n>` / `undone <n>` | tick / untick |
| `rentfree rm <n>` | delete a task |
| `rentfree clear` | drop every ticked task |
| `rentfree clear --all` | drop everything |
| `rentfree list` | print the list, numbered as the note shows it |
| `rentfree edit` | open the file in `$EDITOR` |
| `rentfree path` | print the file's location |

**Config commands:**

| | |
|---|---|
| `rentfree config` | print the effective config, defaults included |
| `rentfree config path` | print the config file's location |
| `rentfree config edit` | open the config in `$EDITOR`, seeded if it's new |

Indices are 1-based and count tasks only, so headings never shift your numbers.
Every one of these takes `-n <note>` to pick a note; without it they act on the
first note in your config.

**Talking to the daemon:**

| | |
|---|---|
| `rentfree show` / `hide` / `toggle` | change visibility |
| `rentfree reload` | force a rebuild from the config |
| `rentfree refresh` | make everything current (config included) |
| `rentfree status` | what the daemon is up to |
| `rentfree notes` | list the configured notes |
| `rentfree quit` | stop it |

`rentfree toggle` bound to a compositor keybind is the good one. With no `-n` it
acts on every note as a single switch — if anything is showing it hides them
all, otherwise it shows them all — so you never end up in a half-hidden state.
`rentfree -n groceries toggle` flips just that one.

## Configuration

The config lives at **`~/.config/rentfree/config`** (strictly
`$XDG_CONFIG_HOME/rentfree/config`; `rentfree config path` will tell you). It's
entirely optional — with no file at all you get one note at `rentfree path` and
never need `-n`.

`rentfree config edit` opens it, creating a commented starter if it doesn't
exist. `rentfree config` prints the *effective* configuration with every default
filled in, which is the quickest way to see what a note is actually being drawn
with. That output is itself valid config and round-trips exactly, so

```sh
rentfree config > ~/.config/rentfree/config
```

is a fine way to start from your current state and edit down from there.
[`contrib/config.example`](contrib/config.example) is the annotated reference.

The daemon watches this file, so saving it re-applies live; no `reload` needed.
`rentfree refresh` also re-reads it, and only rebuilds if the effective values
actually changed — so if a watch ever can't be established (an unwritable config
directory, say), one `refresh` still brings you up to date.

```ini
anchor = top-right
margin = 32
font = Iosevka 13
bg = #1a1a2299
accent = #7ab8ff
title = rent free
```

### Several notes

Top-level keys are global and apply to every note wherever you write them. A
`[note <name>]` section declares one note and overrides whatever it likes:

```ini
font  = Iosevka 12       # every note gets these
layer = bottom

[note todo]
anchor = top-right
title  = today

[note groceries]
file      = shopping.md  # relative to the dir of `rentfree path`
anchor    = bottom-left
max_width = 260
accent    = #9ae6a0      # just this one is green

[note ideas]
anchor    = bottom-right
max_items = 8
```

```sh
rentfree add "call the bank"        # -> todo, the first note declared
rentfree -n groceries add "milk"
rentfree -n groceries done 1
rentfree notes                     # what's declared, and what's showing
```

Notes with no `file =` use `<name>.md`. Files are created on first write, and a
note whose file is empty just doesn't render (`hide_when_empty`), so declaring a
note you rarely use costs you nothing on screen.

**The default note always exists.** A `[note ...]` section *adds* a note; it
never silently takes your existing todo off screen, and bare `rentfree add`
keeps writing to it. If you'd rather configure the default yourself, claim it —
name a section `default`, or point one's `file =` at what `rentfree path`
reports — and your section wins instead of being added alongside.

Paths follow the XDG basedirs and can be overridden with `--file` / `--config`
or `$RENTFREE_FILE` / `$RENTFREE_CONFIG`. The daemon keys its control socket on
`$WAYLAND_DISPLAY`, so nested compositors get their own note; set
`$RENTFREE_SOCKET` to run a second, independent note on one compositor.

## License

MIT
