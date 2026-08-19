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

const char *kVersion = "0.4.0";

void usage(FILE *out) {
  fprintf(out,
R"(rentfree -- keep your todo list living rent free on your desktop

usage: rentfree [options] [command] [args...]

Running with no command starts the daemon: click-through notes pinned above
your wallpaper and below your windows. Every other command edits a todo file
or talks to that daemon.

list commands (these edit the file; the note updates itself)
  add <text...>        append a task
  header <text...>     append a section heading
  done <n>             tick task n
  undone <n>           untick task n
  rm <n>               delete task n
  clear                delete every ticked task
  clear --all          delete everything
  list                 print the list, numbered as the note shows it
  edit                 open the todo file in $EDITOR
  path                 print the todo file path

config commands
  config               print the effective config, defaults included
  config path          print the config file path
  config edit          open the config file in $EDITOR

daemon commands
  show | hide | toggle  change visibility (every note, or just -n's)
  reload                re-read the config file
  refresh               force a redraw
  status                report what the daemon is doing
  notes                 list the configured notes
  quit                  stop the daemon

options
  -n, --note <name>    which note to act on   [the first one in your config]
  -f, --foreground     run the daemon in the foreground (don't fork)
      --file <path>    use this todo file       [$RENTFREE_FILE]
      --config <path>  use this config file     [$RENTFREE_CONFIG]
                       the control socket is $RENTFREE_SOCKET, else keyed
                       on $WAYLAND_DISPLAY
  -h, --help           show this help
  -v, --version        show the version

With no config file you get a single note and can ignore -n entirely. Declare
[note <name>] sections to get more; see contrib/config.example.

Indices are 1-based and count tasks only, so headings never shift your numbers.
)");
}

int fail(const std::string &msg) {
  fprintf(stderr, "rentfree: %s\n", msg.c_str());
  return 1;
}

// Parses a 1-based task index argument.
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

// Fire-and-forget nudge: if a daemon is up, tell it to redraw now instead of
// waiting on inotify. Silence is fine -- no daemon just means no note yet.
void nudge(const std::string &sock, const std::string &note) {
  std::string reply;
  ipc::request(sock, "refresh " + note, reply);
}

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

// Reads from the config so it works with no daemon running; annotates with
// live visibility if one happens to be up.
int cmd_notes(const Settings &settings, const std::string &sock) {
  std::string live;
  ipc::request(sock, "notes", live);

  for (const NoteSpec &spec : settings.notes) {
    const char *state = "";
    size_t at = live.find(spec.name + "\t");
    if (at != std::string::npos && (at == 0 || live[at - 1] == '\n')) {
      size_t b = at + spec.name.size() + 1;
      size_t e = live.find('\t', b);
      if (e != std::string::npos) state = live.compare(b, e - b, "hidden") == 0
                                             ? "  (hidden)"
                                             : "  (visible)";
    }
    struct stat st;
    const char *missing = ::stat(spec.file.c_str(), &st) == 0 ? "" : "  (no file yet)";
    printf("%s\n    %s%s%s\n", spec.name.c_str(), spec.file.c_str(), missing, state);
  }
  return 0;
}

const char *kConfigSeed =
    "# rentfree config -- everything here is optional.\n"
    "#\n"
    "# Top-level keys are global and apply to every note. A [note <name>]\n"
    "# section ADDS another note and overrides what it likes -- your default\n"
    "# note stays. To take the default over instead, name a section 'default'\n"
    "# or point one at the file `rentfree path` reports.\n"
    "#\n"
    "# `rentfree config` prints the effective values, including defaults.\n"
    "# The full annotated reference is contrib/config.example in the source.\n"
    "\n"
    "# anchor = top-right\n"
    "# margin = 24\n"
    "# font   = Sans 12\n"
    "# accent = #73b8ff\n"
    "\n"
    "# [note groceries]\n"
    "# file   = shopping.md\n"
    "# anchor = bottom-left\n";

int cmd_config(const std::vector<std::string> &args, const Settings &settings,
               const std::string &config_path) {
  const std::string sub = args.size() > 1 ? args[1] : "";

  if (sub.empty()) {
    fputs(describe_settings(settings, config_path).c_str(), stdout);
    return 0;
  }
  if (sub == "path") {
    printf("%s\n", config_path.c_str());
    return 0;
  }
  if (sub == "edit") return cmd_edit(config_path, kConfigSeed);

  return fail("config takes nothing, 'path' or 'edit', not '" + sub + "'");
}

// Forwards a control command and prints the daemon's answer.
int cmd_daemon_control(const std::string &sock, const std::string &verb,
                       const std::string &note) {
  std::string reply;
  std::string cmd = note.empty() ? verb : verb + " " + note;
  if (!ipc::request(sock, cmd, reply))
    return fail("no daemon is running (start one with `rentfree`)");
  if (!reply.empty()) printf("%s\n", reply.c_str());
  return reply.rfind("error:", 0) == 0 ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
  std::string todo_path = paths::todo_file();
  std::string config_path = paths::config_file();
  std::string file_override;
  std::string note_name;
  bool foreground = false;

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
    // Everything after the first non-option word belongs to the command.
    args.push_back(a);
    while (++i < argc) args.push_back(argv[i]);
    break;
  }

  if (!file_override.empty()) todo_path = file_override;

  const std::string sock = paths::socket_path();
  Settings settings = load_settings(config_path, todo_path);

  if (!settings.error.empty())
    fprintf(stderr, "rentfree: %s: warnings:\n%s", config_path.c_str(),
            settings.error.c_str());

  if (args.empty() || args[0] == "daemon") {
    note::Options o;
    o.todo_path = todo_path;
    o.config_path = config_path;
    o.socket_path = sock;
    o.foreground = foreground;
    return note::run(o);
  }

  const std::string &cmd = args[0];

  // --- daemon control -----------------------------------------------------
  // Visibility applies to every note unless -n narrows it; that's what you
  // want from a keybind, and with one note the distinction doesn't exist.
  if (cmd == "show" || cmd == "hide" || cmd == "toggle" || cmd == "refresh" ||
      cmd == "reload" || cmd == "status" || cmd == "quit")
    return cmd_daemon_control(sock, cmd, note_name);

  if (cmd == "notes") return cmd_notes(settings, sock);
  if (cmd == "config") return cmd_config(args, settings, config_path);

  // --- pick the note these list commands act on ---------------------------
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
  } else {
    fprintf(stderr, "rentfree: unknown command '%s'\n\n", cmd.c_str());
    usage(stderr);
    return 1;
  }

  if (!store::save(file, items)) return fail("could not write " + file);
  nudge(sock, target->name);
  return 0;
}
