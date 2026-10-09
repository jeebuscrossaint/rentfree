#include "confedit.hpp"
#include "config.hpp"
#include "daemon.hpp"
#include "ipc.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const char *kVersion = "0.5.0";

// Every command in one table, so --help, the "did you mean" hint and the shell
// completions can never disagree about what exists.
struct Command {
  const char *name;
  const char *args;
  const char *doc;
};

const std::vector<Command> &commands() {
  static const std::vector<Command> table = {
      {"add", "<text...>", "append a task"},
      {"header", "<text...>", "append a section heading"},
      {"done", "<n>", "tick task n"},
      {"undone", "<n>", "untick task n"},
      {"rm", "<n>", "delete task n"},
      {"clear", "[--all]", "delete ticked tasks, or everything"},
      {"list", "", "print the list as the note shows it"},
      {"edit", "", "open the todo file in $EDITOR"},
      {"path", "", "print the todo file path"},
      {"set", "<key> <value>", "change a setting in the config file"},
      {"get", "[key]", "print an effective setting, or all of them"},
      {"unset", "<key>", "drop a setting back to its default"},
      {"keys", "", "list every setting, with types and defaults"},
      {"config", "[path|edit]", "print, locate or edit the config file"},
      {"notes", "", "list the configured notes"},
      {"outputs", "", "list the outputs the compositor is offering"},
      {"show", "", "show notes"},
      {"hide", "", "hide notes"},
      {"toggle", "", "flip note visibility"},
      {"refresh", "", "make everything current"},
      {"reload", "", "force a rebuild from the config"},
      {"status", "", "report what the daemon is doing"},
      {"quit", "", "stop the daemon"},
      {"daemon", "", "start the daemon (the default with no command)"},
  };
  return table;
}

bool is_command(const std::string &name) {
  return std::any_of(commands().begin(), commands().end(),
                     [&](const Command &c) { return name == c.name; });
}

int fail(const std::string &msg) {
  fprintf(stderr, "rentfree: %s\n", msg.c_str());
  return 1;
}

void usage(FILE *out) {
  fprintf(out,
R"(rentfree -- keep your todo list living rent free on your desktop

usage: rentfree [options] [command] [args...]

With no command it starts the daemon: click-through notes pinned above your
wallpaper and below your windows.

)");
  fprintf(out, "commands\n");
  for (const Command &c : commands())
    fprintf(out, "  %-10s %-14s %s\n", c.name, c.args, c.doc);

  fprintf(out, R"(
options
  -n, --note <name>    which note to act on   [the default note]
  -f, --foreground     run the daemon in the foreground (don't fork)
      --file <path>    use this todo file       [$RENTFREE_FILE]
      --config <path>  use this config file     [$RENTFREE_CONFIG]
  -h, --help           show this help
  -v, --version        show the version

Every setting is also a command-line option, applied over the config file and
lasting only as long as the process -- `rentfree --anchor center --font "Sans 14"`
starts a daemon that way without editing anything. `rentfree set` writes to the
config instead, so it sticks. `rentfree keys` lists them all.

Indices are 1-based and count tasks only, so headings never shift your numbers.
)");
}

// --- small helpers --------------------------------------------------------

bool want_index(const std::vector<std::string> &args, size_t i, const store::List &list,
                int &out_pos) {
  if (i >= args.size()) {
    fprintf(stderr, "rentfree: expected a task number\n");
    return false;
  }
  int n = 0;
  try {
    n = std::stoi(args[i]);
  } catch (...) {
    fprintf(stderr, "rentfree: '%s' is not a task number\n", args[i].c_str());
    return false;
  }
  out_pos = store::task_index(list, n);
  if (out_pos < 0) {
    fprintf(stderr, "rentfree: no task %d (the list has %d)\n", n,
            store::task_count(list));
    return false;
  }
  return true;
}

std::string join(const std::vector<std::string> &args, size_t from) {
  std::string s;
  for (size_t i = from; i < args.size(); ++i) {
    if (i > from) s += " ";
    s += args[i];
  }
  return s;
}

void nudge(const std::string &sock, const std::string &note) {
  std::string reply;
  ipc::request(sock, "refresh " + note, reply);
}

// --- commands -------------------------------------------------------------

int cmd_list(const store::List &items) {
  int n = 0;
  for (const auto &it : items) {
    if (it.kind == store::Kind::Header) {
      printf("\n%s\n", it.text.c_str());
      continue;
    }
    printf("%3d. [%c] %s\n", ++n, it.done ? 'x' : ' ', it.text.c_str());
  }
  if (n == 0 && items.empty()) printf("(nothing rent free yet)\n");
  return 0;
}

int cmd_edit(const std::string &file, const char *seed = nullptr) {
  const char *editor = std::getenv("EDITOR");
  if (!editor || !*editor) editor = std::getenv("VISUAL");
  if (!editor || !*editor) editor = "vi";

  paths::ensure_parent_dir(file);

  // Opening an empty buffer tells you nothing about what belongs in it.
  struct stat st;
  if (seed && ::stat(file.c_str(), &st) != 0) {
    if (FILE *f = std::fopen(file.c_str(), "w")) {
      std::fputs(seed, f);
      std::fclose(f);
    }
  }

  pid_t pid = fork();
  if (pid < 0) return fail("fork failed");
  if (pid == 0) {
    execlp(editor, editor, file.c_str(), (char *)nullptr);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

const char *kConfigSeed =
    "# rentfree config -- everything here is optional.\n"
    "#\n"
    "# Top-level keys are global and apply to every note. A [note <name>]\n"
    "# section ADDS another note and overrides what it likes -- your default\n"
    "# note stays. To take the default over instead, name a section 'default'\n"
    "# or point one at the file `rentfree path` reports.\n"
    "#\n"
    "# `rentfree keys` lists every setting; `rentfree set <key> <value>` edits\n"
    "# this file for you; `rentfree config` prints the effective values.\n"
    "\n"
    "# anchor = top-right\n"
    "# margin = 24\n"
    "# font   = Sans 12\n"
    "# accent = #73b8ff\n"
    "\n"
    "# [note groceries]\n"
    "# file   = shopping.md\n"
    "# anchor = bottom-left\n";

int cmd_notes(const Settings &settings, const std::string &sock) {
  std::string live;
  ipc::request(sock, "notes", live);

  for (const NoteSpec &spec : settings.notes) {
    const char *state = "";
    size_t at = live.find(spec.name + "\t");
    if (at != std::string::npos && (at == 0 || live[at - 1] == '\n')) {
      size_t b = at + spec.name.size() + 1;
      size_t e = live.find('\t', b);
      if (e != std::string::npos)
        state = live.compare(b, e - b, "hidden") == 0 ? "  (hidden)" : "  (visible)";
    }
    struct stat st;
    const char *missing = ::stat(spec.file.c_str(), &st) == 0 ? "" : "  (no file yet)";
    printf("%s\n    %s%s%s\n", spec.name.c_str(), spec.file.c_str(), missing, state);
  }
  return 0;
}

int cmd_keys() {
  const Style d; // a default-constructed style *is* the documented default
  for (const Option &o : options()) {
    printf("%-17s%s\n", o.name, o.doc);
    std::string detail = option_hint(o);
    if (!o.shorthand) {
      std::string def = o.get(d);
      detail += "  (default: " + (def.empty() ? std::string("empty") : def) + ")";
    }
    printf("%-17s%s\n", "", detail.c_str());
  }
  return 0;
}

// Prints an unknown-setting error with suggestions. Shared by set/get/unset.
int unknown_key(const std::string &key) {
  fprintf(stderr, "rentfree: no setting called '%s'\n", key.c_str());
  auto hints = suggest_options(key);
  if (!hints.empty()) {
    fprintf(stderr, "  did you mean: ");
    for (size_t i = 0; i < hints.size(); ++i)
      fprintf(stderr, "%s%s", i ? ", " : "", hints[i].c_str());
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "  `rentfree keys` lists every setting\n");
  return 1;
}

int cmd_set(const std::vector<std::string> &args, const std::string &config_path,
            const std::string &note_name, const NoteSpec &target,
            const std::string &sock) {
  if (args.size() < 3)
    return fail("set needs a key and a value, e.g. `rentfree set anchor center`");

  const std::string key = args[1];
  const std::string value = join(args, 2);

  const Option *opt = find_option(key);
  if (!opt) return unknown_key(key);

  // Validate before touching the file, and say what was expected.
  Style probe = target.style;
  std::string err;
  if (!opt->set(probe, value, err)) {
    fprintf(stderr, "rentfree: %s\n", err.c_str());
    fprintf(stderr, "  %s expects %s\n", opt->name, option_hint(*opt).c_str());
    return 1;
  }

  // -n writes into that note's section; without it the value is global.
  const std::string section = note_name.empty() ? "" : target.name;

  // Creating a section for a note that only existed implicitly must pin its
  // file, or the note would silently start pointing at <name>.md instead.
  if (!section.empty() && !confedit::has_section(config_path, section)) {
    if (!confedit::set(config_path, section, "file", target.file, err))
      return fail(err);
  }

  if (!confedit::set(config_path, section, opt->name, value, err)) return fail(err);

  printf("%s = %s%s\n", opt->name, value.c_str(),
         section.empty() ? "" : (" [note " + section + "]").c_str());

  // The config is watched, but nudging means it applies even if the watch
  // couldn't be established.
  std::string reply;
  ipc::request(sock, "refresh", reply);
  return 0;
}

int cmd_get(const std::vector<std::string> &args, const NoteSpec &target) {
  if (args.size() < 2) {
    for (const Option &o : options()) {
      if (o.shorthand) continue;
      std::string v = o.get(target.style);
      printf("%s%s%s\n", o.name, v.empty() ? " =" : " = ", v.c_str());
    }
    return 0;
  }

  const Option *opt = find_option(args[1]);
  if (!opt) return unknown_key(args[1]);
  printf("%s\n", opt->get(target.style).c_str());
  return 0;
}

int cmd_unset(const std::vector<std::string> &args, const std::string &config_path,
              const std::string &note_name, const NoteSpec &target,
              const std::string &sock) {
  if (args.size() < 2) return fail("unset needs a key, e.g. `rentfree unset anchor`");

  const Option *opt = find_option(args[1]);
  if (!opt) return unknown_key(args[1]);

  const std::string section = note_name.empty() ? "" : target.name;
  bool changed = false;
  std::string err;
  if (!confedit::unset(config_path, section, opt->name, changed, err)) return fail(err);

  if (!changed) {
    printf("%s was not set%s\n", opt->name,
           section.empty() ? "" : (" in [note " + section + "]").c_str());
    return 0;
  }
  printf("unset %s\n", opt->name);
  std::string reply;
  ipc::request(sock, "refresh", reply);
  return 0;
}

int cmd_daemon_control(const std::string &sock, const std::string &verb,
                       const std::string &note) {
  std::string reply;
  std::string cmd = note.empty() ? verb : verb + " " + note;
  if (!ipc::request(sock, cmd, reply))
    return fail("no daemon is running (start one with `rentfree`)");
  if (!reply.empty()) printf("%s\n", reply.c_str());
  return reply.rfind("error:", 0) == 0 ? 1 : 0;
}

// --- completion support ---------------------------------------------------
//
// The shell scripts ask the binary what exists rather than hardcoding it, so
// they stay correct when settings are added. Output is <value>\t<description>,
// which is exactly what fish wants and which bash/zsh can cut at the tab.

int complete(const std::string &what, const std::vector<std::string> &args,
             const Settings &settings, const std::string &sock,
             const std::string &note_name) {
  if (what == "commands") {
    for (const Command &c : commands()) printf("%s\t%s\n", c.name, c.doc);
    return 0;
  }
  if (what == "keys") {
    for (const Option &o : options()) printf("%s\t%s\n", o.name, o.doc);
    return 0;
  }
  if (what == "bools") {
    // So a completion script can find the boolean settings, and offer their
    // bare and --no- forms, with one call instead of one per setting.
    for (const Option &o : options())
      if (o.type == OptType::Bool) printf("%s\n", o.name);
    return 0;
  }
  if (what == "notes") {
    for (const NoteSpec &s : settings.notes) printf("%s\t%s\n", s.name.c_str(), s.file.c_str());
    return 0;
  }
  if (what == "tasks") {
    // Task numbers with their text, so `done <TAB>` shows you what you'd tick.
    const NoteSpec *n = &settings.notes.front();
    for (const NoteSpec &s : settings.notes)
      if (s.name == note_name) n = &s;

    store::List items = store::load(n->file);
    int idx = 0;
    for (const store::Item &it : items) {
      if (it.kind != store::Kind::Task) continue;
      printf("%d\t%s%s\n", ++idx, it.done ? "[x] " : "", it.text.c_str());
    }
    return 0;
  }

  if (what == "values") {
    if (args.empty()) return 0;
    const Option *opt = find_option(args[0]);
    if (!opt) return 0;

    if (opt->type == OptType::Bool) {
      printf("true\nfalse\n");
      return 0;
    }
    for (const char *c : opt->choices) printf("%s\n", c);

    // The compositor is the only thing that knows the connector names.
    if (std::string(opt->name) == "output") {
      std::string reply;
      // No daemon, or an older one that doesn't know the command, simply means
      // no suggestions -- completion must never print an error.
      if (ipc::request(sock, "outputs", reply) && !reply.empty() &&
          reply.rfind("error:", 0) != 0)
        printf("%s\n", reply.c_str());
    }
    return 0;
  }
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  std::string todo_path = paths::todo_file();
  std::string config_path = paths::config_file();
  std::string file_override;
  std::string note_name;
  bool foreground = false;
  std::vector<Override> overrides;
  std::string complete_what;

  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];

    if (a == "-h" || a == "--help") { usage(stdout); return 0; }
    if (a == "-v" || a == "--version") { printf("rentfree %s\n", kVersion); return 0; }
    if (a == "-f" || a == "--foreground") { foreground = true; continue; }
    if (a == "-n" || a == "--note") {
      if (i + 1 >= argc) return fail(a + " needs a note name");
      note_name = argv[++i];
      continue;
    }
    if (a == "--file" || a == "--config") {
      if (i + 1 >= argc) return fail(a + " needs a path");
      (a == "--file" ? file_override : config_path) = argv[++i];
      continue;
    }
    if (a.rfind("--complete-", 0) == 0) {
      complete_what = a.substr(std::strlen("--complete-"));
      continue;
    }

    // Any setting can be given as --key value, or --key=value. Booleans may be
    // bare (--show-done) or negated (--no-show-done).
    if (a.rfind("--", 0) == 0) {
      std::string body = a.substr(2), inline_value;
      bool has_inline = false;
      if (auto eq = body.find('='); eq != std::string::npos) {
        inline_value = body.substr(eq + 1);
        body = body.substr(0, eq);
        has_inline = true;
      }

      bool negated = false;
      std::string name = body;
      if (!has_inline && body.rfind("no-", 0) == 0 && find_option(body.substr(3))) {
        negated = true;
        name = body.substr(3);
      }

      if (const Option *opt = find_option(name)) {
        std::string value;
        if (has_inline) {
          value = inline_value;
        } else if (negated) {
          value = "false";
        } else if (opt->type == OptType::Bool) {
          // A bare --flag means true. Only swallow the next word if it really
          // is a boolean, so `rentfree --show-done get anchor` still works.
          Style scratch;
          std::string ignored;
          if (i + 1 < argc && opt->set(scratch, argv[i + 1], ignored))
            value = argv[++i];
          else
            value = "true";
        } else if (i + 1 < argc) {
          value = argv[++i];
        } else {
          return fail("--" + name + " needs a value (" + option_hint(*opt) + ")");
        }

        Style probe;
        std::string err;
        if (!opt->set(probe, value, err)) {
          fprintf(stderr, "rentfree: --%s: %s\n", name.c_str(), err.c_str());
          return 1;
        }
        overrides.push_back({opt->name, value});
        continue;
      }

      fprintf(stderr, "rentfree: unknown option '%s'\n", a.c_str());
      auto hints = suggest_options(name);
      if (!hints.empty()) {
        fprintf(stderr, "  did you mean: ");
        for (size_t j = 0; j < hints.size(); ++j)
          fprintf(stderr, "%s--%s", j ? ", " : "", hints[j].c_str());
        fprintf(stderr, "\n");
      }
      return 1;
    }

    // Everything from the first bare word on belongs to the command.
    args.push_back(a);
    while (++i < argc) args.push_back(argv[i]);
    break;
  }

  if (!file_override.empty()) todo_path = file_override;

  const std::string sock = paths::socket_path();
  Settings settings = load_settings(config_path, todo_path, overrides);

  if (!complete_what.empty())
    return complete(complete_what, args, settings, sock, note_name);

  if (!settings.error.empty())
    fprintf(stderr, "rentfree: %s:\n%s", config_path.c_str(), settings.error.c_str());

  if (args.empty() || args[0] == "daemon") {
    note::Options o;
    o.todo_path = todo_path;
    o.config_path = config_path;
    o.socket_path = sock;
    o.foreground = foreground;
    o.overrides = overrides;
    return note::run(o);
  }

  const std::string &cmd = args[0];

  if (!is_command(cmd)) {
    fprintf(stderr, "rentfree: unknown command '%s'\n", cmd.c_str());
    std::vector<std::string> near;
    for (const Command &c : commands())
      if (std::string(c.name).rfind(cmd, 0) == 0) near.push_back(c.name);
    if (!near.empty()) {
      fprintf(stderr, "  did you mean: ");
      for (size_t i = 0; i < near.size(); ++i)
        fprintf(stderr, "%s%s", i ? ", " : "", near[i].c_str());
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "  `rentfree --help` lists every command\n");
    return 1;
  }

  // --- daemon control -----------------------------------------------------
  if (cmd == "show" || cmd == "hide" || cmd == "toggle" || cmd == "refresh" ||
      cmd == "reload" || cmd == "status" || cmd == "quit" || cmd == "outputs")
    return cmd_daemon_control(sock, cmd, note_name);

  if (cmd == "notes") return cmd_notes(settings, sock);
  if (cmd == "keys") return cmd_keys();

  // --- pick the note the rest act on --------------------------------------
  const NoteSpec *target = &settings.notes.front();
  if (!note_name.empty()) {
    auto it = std::find_if(settings.notes.begin(), settings.notes.end(),
                           [&](const NoteSpec &s) { return s.name == note_name; });
    if (it == settings.notes.end()) {
      fprintf(stderr, "rentfree: no note called '%s'. Configured notes:\n",
              note_name.c_str());
      for (const NoteSpec &s : settings.notes)
        fprintf(stderr, "  %s\n", s.name.c_str());
      return 1;
    }
    target = &*it;
  }

  if (cmd == "set") return cmd_set(args, config_path, note_name, *target, sock);
  if (cmd == "get") return cmd_get(args, *target);
  if (cmd == "unset") return cmd_unset(args, config_path, note_name, *target, sock);

  if (cmd == "config") {
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (sub.empty()) {
      fputs(describe_settings(settings, config_path).c_str(), stdout);
      return 0;
    }
    if (sub == "path") { printf("%s\n", config_path.c_str()); return 0; }
    if (sub == "edit") return cmd_edit(config_path, kConfigSeed);
    return fail("config takes nothing, 'path' or 'edit', not '" + sub + "'");
  }

  // An explicit --file wins over whatever the config says.
  const std::string file = file_override.empty() ? target->file : file_override;

  if (cmd == "path") { printf("%s\n", file.c_str()); return 0; }
  if (cmd == "edit") return cmd_edit(file);

  store::List items = store::load(file);
  if (cmd == "list") return cmd_list(items);

  if (cmd == "add" || cmd == "header") {
    std::string text = join(args, 1);
    if (text.empty()) return fail(cmd + " needs some text");
    store::Item it;
    it.kind = (cmd == "header") ? store::Kind::Header : store::Kind::Task;
    it.text = text;
    items.push_back(std::move(it));
  } else if (cmd == "done" || cmd == "undone" || cmd == "rm") {
    int pos = 0;
    if (!want_index(args, 1, items, pos)) return 1;
    if (cmd == "rm")
      items.erase(items.begin() + pos);
    else
      items[pos].done = (cmd == "done");
  } else if (cmd == "clear") {
    bool all = args.size() > 1 && (args[1] == "--all" || args[1] == "-a");
    if (all)
      items.clear();
    else
      items.erase(std::remove_if(items.begin(), items.end(),
                                 [](const store::Item &it) {
                                   return it.kind == store::Kind::Task && it.done;
                                 }),
                  items.end());
  }

  if (!store::save(file, items)) return fail("could not write " + file);
  nudge(sock, target->name);
  return 0;
}
