#include "daemon.hpp"

#include "config.hpp"
#include "ipc.hpp"
#include "paths.hpp"
#include "render.hpp"
#include "store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

namespace {

// ---------------------------------------------------------------------------
// shm buffers
// ---------------------------------------------------------------------------

struct Buffer {
  wl_buffer *wl = nullptr;
  uint8_t *data = nullptr;
  size_t size = 0;
  int width = 0; // device pixels
  int height = 0;
  bool busy = false; // held by the compositor
};

void buffer_release(void *data, wl_buffer *) {
  static_cast<Buffer *>(data)->busy = false;
}
const wl_buffer_listener buffer_listener = {.release = buffer_release};

void buffer_destroy(Buffer &b) {
  if (b.wl) wl_buffer_destroy(b.wl);
  if (b.data) munmap(b.data, b.size);
  b = Buffer{};
}

bool buffer_create(Buffer &b, wl_shm *shm, int w, int h) {
  buffer_destroy(b);

  const int stride = w * 4;
  const size_t size = static_cast<size_t>(stride) * h;

  int fd = memfd_create("rentfree", MFD_CLOEXEC);
  if (fd < 0) return false;
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    close(fd);
    return false;
  }

  void *map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    return false;
  }

  wl_shm_pool *pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
  b.wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  b.data = static_cast<uint8_t *>(map);
  b.size = size;
  b.width = w;
  b.height = h;
  b.busy = false;
  wl_buffer_add_listener(b.wl, &buffer_listener, &b);
  return true;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

struct App;

struct Output {
  App *app = nullptr;
  wl_output *wl = nullptr;
  uint32_t global = 0;
  std::string name;
  int32_t scale = 1;
};

// One note == one file == one layer surface.
struct Note {
  App *app = nullptr;
  NoteSpec spec;

  store::List items;
  std::string markup;

  wl_surface *surface = nullptr;
  zwlr_layer_surface_v1 *layer_surface = nullptr;
  Buffer buffers[2];

  bool visible = true;   // what the user asked for
  bool mapped = false;   // whether a surface currently exists
  bool configured = false;
  int want_w = 0, want_h = 0; // logical size we asked for
  int have_w = 0, have_h = 0; // logical size we were given
  int scale = 1;

  int watch = -1;        // inotify descriptor for the containing directory
  std::string basename;  // the file name we're filtering that directory for

  // Set from a Wayland callback, acted on by the main loop. Remapping touches
  // the display, so it has no business happening mid-dispatch.
  bool needs_update = false;
};

struct App {
  note::Options opts;
  Settings settings;
  // unique_ptr because listener user-data points at these; a vector of values
  // would dangle the moment it reallocated.
  std::vector<std::unique_ptr<Note>> notes;

  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_compositor *compositor = nullptr;
  wl_shm *shm = nullptr;
  zwlr_layer_shell_v1 *layer_shell = nullptr;
  uint32_t compositor_version = 1;

  std::vector<Output *> outputs;

  bool running = true;

  int inotify_fd = -1;
  int config_watch = -1;
  std::string config_name;
  // Serialised effective settings, so we can tell a real config change from a
  // save that altered nothing.
  std::string fingerprint;

  ipc::Server server;
};

// Signal -> main loop, via the usual self-pipe so we stay async-signal-safe.
int g_signal_pipe[2] = {-1, -1};

void on_signal(int) {
  const char c = 1;
  ssize_t r = write(g_signal_pipe[1], &c, 1);
  (void)r;
}

void redraw(Note &n);
void update(Note &n);
void unmap(Note &n);

// ---------------------------------------------------------------------------
// wayland listeners
// ---------------------------------------------------------------------------

void output_geometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t,
                     int32_t, const char *, const char *, int32_t) {}
void output_mode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
void output_done(void *, wl_output *) {}

void output_scale(void *data, wl_output *, int32_t factor) {
  static_cast<Output *>(data)->scale = factor < 1 ? 1 : factor;
}
void output_name(void *data, wl_output *, const char *name);
void output_description(void *, wl_output *, const char *) {}

const wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

void surface_enter(void *data, wl_surface *, wl_output *out);
void surface_leave(void *, wl_surface *, wl_output *) {}

// wl_surface v6 sends these; we bind v4 so they never fire, but libwayland
// dispatches by slot and a null handler would be a crash waiting to happen.
void surface_preferred_scale(void *, wl_surface *, int32_t) {}
void surface_preferred_transform(void *, wl_surface *, uint32_t) {}

const wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_scale,
    .preferred_buffer_transform = surface_preferred_transform,
};

void layer_configure(void *data, zwlr_layer_surface_v1 *ls, uint32_t serial,
                     uint32_t w, uint32_t h) {
  Note &n = *static_cast<Note *>(data);
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  n.have_w = w ? static_cast<int>(w) : n.want_w;
  n.have_h = h ? static_cast<int>(h) : n.want_h;
  n.configured = true;
  redraw(n);
}

void layer_closed(void *data, zwlr_layer_surface_v1 *) {
  // The compositor pulled the surface out from under us: the output went away,
  // a monitor slept, the session reconfigured. Drop our side and ask for a
  // remap -- without this the note silently stays gone until something else
  // happens to trigger an update, which can be hours.
  Note &n = *static_cast<Note *>(data);
  if (n.layer_surface) {
    zwlr_layer_surface_v1_destroy(n.layer_surface);
    n.layer_surface = nullptr;
  }
  if (n.surface) {
    wl_surface_destroy(n.surface);
    n.surface = nullptr;
  }
  n.configured = false;
  n.mapped = false;
  n.needs_update = true;
}

const zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

void registry_global(void *data, wl_registry *reg, uint32_t name,
                     const char *iface, uint32_t version) {
  App &app = *static_cast<App *>(data);

  if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
    app.compositor_version = std::min(version, 4u);
    app.compositor = static_cast<wl_compositor *>(
        wl_registry_bind(reg, name, &wl_compositor_interface, app.compositor_version));
  } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
    app.shm = static_cast<wl_shm *>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
  } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    app.layer_shell = static_cast<zwlr_layer_shell_v1 *>(wl_registry_bind(
        reg, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
  } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
    Output *o = new Output();
    o->app = &app;
    o->global = name;
    o->wl = static_cast<wl_output *>(
        wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 4u)));
    wl_output_add_listener(o->wl, &output_listener, o);
    app.outputs.push_back(o);
  }
}

void registry_global_remove(void *data, wl_registry *, uint32_t name) {
  App &app = *static_cast<App *>(data);
  for (auto it = app.outputs.begin(); it != app.outputs.end(); ++it) {
    if ((*it)->global == name) {
      wl_output_destroy((*it)->wl);
      delete *it;
      app.outputs.erase(it);
      return;
    }
  }
}

const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// ---------------------------------------------------------------------------
// surface lifecycle
// ---------------------------------------------------------------------------

uint32_t anchor_bits(const std::string &a) {
  const uint32_t T = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
  const uint32_t B = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
  const uint32_t L = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
  const uint32_t R = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

  if (a == "top") return T;
  if (a == "bottom") return B;
  if (a == "left") return L;
  if (a == "right") return R;
  if (a == "top-left" || a == "topleft") return T | L;
  if (a == "top-right" || a == "topright") return T | R;
  if (a == "bottom-left" || a == "bottomleft") return B | L;
  if (a == "bottom-right" || a == "bottomright") return B | R;
  if (a == "center" || a == "centre") return 0;
  return T | R;
}

uint32_t layer_value(const std::string &l) {
  if (l == "background") return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
  if (l == "top") return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
  if (l == "overlay") return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
  return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM; // above the wallpaper, below windows
}

wl_output *pick_output(App &app, const Style &st) {
  if (st.output.empty()) return nullptr; // compositor's choice
  for (Output *o : app.outputs)
    if (o->name == st.output) return o->wl;
  return nullptr;
}

void surface_enter(void *data, wl_surface *, wl_output *out) {
  Note &n = *static_cast<Note *>(data);
  for (Output *o : n.app->outputs) {
    if (o->wl == out && o->scale != n.scale) {
      n.scale = o->scale;
      redraw(n);
    }
  }
}

// Defined here because it needs both App and Note to be complete.
void output_name(void *data, wl_output *, const char *name) {
  Output &o = *static_cast<Output *>(data);
  o.name = name ? name : "";
  if (!o.app) return;

  // A note pinned to this connector can be mapped now that it exists again.
  for (auto &n : o.app->notes)
    if (!n->mapped && n->spec.style.output == o.name) n->needs_update = true;
}

void unmap(Note &n) {
  if (n.layer_surface) {
    zwlr_layer_surface_v1_destroy(n.layer_surface);
    n.layer_surface = nullptr;
  }
  if (n.surface) {
    wl_surface_destroy(n.surface);
    n.surface = nullptr;
  }
  // Let the compositor process the destroy before we munmap buffers it may
  // still be reading from.
  wl_display_roundtrip(n.app->display);
  for (Buffer &b : n.buffers) buffer_destroy(b);
  n.configured = false;
  n.mapped = false;
}

void map(Note &n, int w, int h) {
  App &app = *n.app;
  const Style &st = n.spec.style;

  n.surface = wl_compositor_create_surface(app.compositor);
  wl_surface_add_listener(n.surface, &surface_listener, &n);

  // An empty input region is what makes this a note rather than a window:
  // clicks, scrolls and hovers all pass straight through to the desktop.
  wl_region *empty = wl_compositor_create_region(app.compositor);
  wl_surface_set_input_region(n.surface, empty);
  wl_region_destroy(empty);

  n.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      app.layer_shell, n.surface, pick_output(app, st), layer_value(st.layer),
      "rentfree");
  zwlr_layer_surface_v1_add_listener(n.layer_surface, &layer_listener, &n);

  zwlr_layer_surface_v1_set_size(n.layer_surface, w, h);
  zwlr_layer_surface_v1_set_anchor(n.layer_surface, anchor_bits(st.anchor));
  zwlr_layer_surface_v1_set_margin(n.layer_surface, st.margin_top, st.margin_right,
                                   st.margin_bottom, st.margin_left);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      n.layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  // 0 = don't reserve space but stay clear of panels; -1 = ignore them entirely.
  zwlr_layer_surface_v1_set_exclusive_zone(n.layer_surface, st.respect_bars ? 0 : -1);

  wl_surface_commit(n.surface);

  n.want_w = w;
  n.want_h = h;
  n.mapped = true;
  n.configured = false;
}

void redraw(Note &n) {
  if (!n.mapped || !n.configured || !n.visible) return;

  App &app = *n.app;
  const int scale = n.scale < 1 ? 1 : n.scale;
  const int pw = n.have_w * scale;
  const int ph = n.have_h * scale;
  if (pw <= 0 || ph <= 0) return;

  Buffer *b = nullptr;
  for (Buffer &cand : n.buffers) {
    if (cand.busy) continue;
    if (cand.wl && cand.width == pw && cand.height == ph) { b = &cand; break; }
    if (!b) b = &cand; // reusable slot, wrong size
  }
  if (!b) return; // both in flight; the next frame catches up

  if (!b->wl || b->width != pw || b->height != ph) {
    if (!buffer_create(*b, app.shm, pw, ph)) return;
  }

  cairo_surface_t *surf = cairo_image_surface_create_for_data(
      b->data, CAIRO_FORMAT_ARGB32, pw, ph, pw * 4);
  cairo_t *cr = cairo_create(surf);
  cairo_scale(cr, scale, scale); // draw in logical pixels throughout
  render::draw(cr, n.spec.style, n.markup, n.have_w, n.have_h);
  cairo_destroy(cr);
  cairo_surface_flush(surf);
  cairo_surface_destroy(surf);

  b->busy = true;
  if (app.compositor_version >= 3) wl_surface_set_buffer_scale(n.surface, scale);
  wl_surface_attach(n.surface, b->wl, 0, 0);
  if (app.compositor_version >= 4)
    wl_surface_damage_buffer(n.surface, 0, 0, pw, ph);
  else
    wl_surface_damage(n.surface, 0, 0, n.have_w, n.have_h);
  wl_surface_commit(n.surface);
}

// Reconciles what's on screen with what's in this note's file.
void update(Note &n) {
  n.items = store::load(n.spec.file);
  n.markup = render::build_markup(n.spec.style, n.items);

  const bool empty = n.markup.empty();
  const bool should_show = n.visible && !(empty && n.spec.style.hide_when_empty);

  if (!should_show) {
    if (n.mapped) unmap(n);
    return;
  }

  render::Size size = render::measure(n.spec.style, n.markup);

  if (!n.mapped) {
    map(n, size.w, size.h);
    return; // the configure event drives the first paint
  }

  if (size.w != n.want_w || size.h != n.want_h) {
    n.want_w = size.w;
    n.want_h = size.h;
    zwlr_layer_surface_v1_set_size(n.layer_surface, size.w, size.h);
    wl_surface_commit(n.surface);
    return; // configure lands next, then we paint at the new size
  }

  redraw(n);
}

// ---------------------------------------------------------------------------
// file watching
// ---------------------------------------------------------------------------

std::string dirname_of(const std::string &p) {
  auto s = p.rfind('/');
  return (s == std::string::npos) ? "." : p.substr(0, s ? s : 1);
}
std::string basename_of(const std::string &p) {
  auto s = p.rfind('/');
  return (s == std::string::npos) ? p : p.substr(s + 1);
}

const uint32_t kWatchMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                            IN_MOVED_FROM | IN_MODIFY;

// Watches each note's containing *directory*, not the file. Editors save by
// writing a temp file and renaming over the target, which silently detaches an
// inotify watch on the inode -- watching the directory survives that. Notes
// sharing a directory share one watch descriptor, which is why matching has to
// check the file name too.
void watch_notes(App &app) {
  for (auto &n : app.notes) {
    paths::ensure_parent_dir(n->spec.file);
    n->basename = basename_of(n->spec.file);
    n->watch = inotify_add_watch(app.inotify_fd,
                                 dirname_of(n->spec.file).c_str(), kWatchMask);
  }
}

void unwatch_notes(App &app) {
  for (auto &n : app.notes) {
    if (n->watch >= 0) inotify_rm_watch(app.inotify_fd, n->watch);
    n->watch = -1;
  }
}

// ---------------------------------------------------------------------------
// note set construction
// ---------------------------------------------------------------------------

void build_notes(App &app) {
  // Remember visibility per name so a config reload doesn't un-hide things.
  std::vector<std::pair<std::string, bool>> was_visible;
  for (auto &n : app.notes) was_visible.emplace_back(n->spec.name, n->visible);

  for (auto &n : app.notes)
    if (n->mapped) unmap(*n);
  unwatch_notes(app);
  app.notes.clear();

  for (const NoteSpec &spec : app.settings.notes) {
    auto n = std::make_unique<Note>();
    n->app = &app;
    n->spec = spec;
    for (const auto &[name, vis] : was_visible)
      if (name == spec.name) n->visible = vis;
    app.notes.push_back(std::move(n));
  }

  watch_notes(app);
  for (auto &n : app.notes) update(*n);
}

// The config's directory has to exist before inotify can watch it, and it
// often won't on a first run -- that silently cost us every config reload until
// someone ran `reload` by hand. Re-armable so a deleted/recreated directory
// doesn't strand us either.
void watch_config(App &app) {
  if (app.inotify_fd < 0) return;

  if (app.config_watch >= 0) {
    inotify_rm_watch(app.inotify_fd, app.config_watch);
    app.config_watch = -1;
  }

  paths::ensure_parent_dir(app.opts.config_path);
  app.config_name = basename_of(app.opts.config_path);
  app.config_watch = inotify_add_watch(
      app.inotify_fd, dirname_of(app.opts.config_path).c_str(), kWatchMask);

  if (app.config_watch < 0)
    fprintf(stderr, "rentfree: cannot watch %s, so config changes won't apply "
                    "on their own: %s\n",
            dirname_of(app.opts.config_path).c_str(), std::strerror(errno));
}

// Re-reads the config and rebuilds the notes if the effective settings actually
// changed. Returns true if it rebuilt. `force` rebuilds regardless.
bool apply_settings(App &app, bool force) {
  Settings fresh =
      load_settings(app.opts.config_path, app.opts.todo_path, app.opts.overrides);
  // Comparing the serialisation means every field is covered automatically,
  // including ones added later.
  std::string fp = describe_settings(fresh, app.opts.config_path);

  if (!force && fp == app.fingerprint) return false;

  app.settings = std::move(fresh);
  app.fingerprint = std::move(fp);
  watch_config(app); // the directory may have only just appeared
  build_notes(app);
  return true;
}

// ---------------------------------------------------------------------------
// control commands
// ---------------------------------------------------------------------------

// Splits "hide groceries" into verb and target. An empty target means every
// note, which is what you want from a keybind.
void split_command(const std::string &cmd, std::string &verb, std::string &target) {
  auto sp = cmd.find(' ');
  if (sp == std::string::npos) {
    verb = cmd;
    target.clear();
    return;
  }
  verb = cmd.substr(0, sp);
  target = cmd.substr(sp + 1);
  while (!target.empty() && target.front() == ' ') target.erase(target.begin());
  while (!target.empty() && target.back() == ' ') target.pop_back();
}

std::string handle_command(App &app, const std::string &cmd) {
  std::string verb, target;
  split_command(cmd, verb, target);

  if (verb == "ping") return "pong";

  if (verb == "quit") {
    app.running = false;
    return "ok";
  }

  if (verb == "reload") {
    apply_settings(app, true);
    return app.settings.error.empty() ? "ok"
                                      : "ok, with config warnings:\n" + app.settings.error;
  }

  if (verb == "outputs") {
    std::string out;
    for (Output *o : app.outputs)
      if (!o->name.empty()) out += o->name + "\n";
    if (!out.empty()) out.pop_back();
    return out;
  }

  if (verb == "notes") {
    std::string out;
    for (auto &n : app.notes) {
      out += n->spec.name;
      out += "\t";
      out += n->visible ? "visible" : "hidden";
      out += "\t";
      out += n->spec.file;
      out += "\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
  }

  if (verb == "status") {
    std::string out = "pid " + std::to_string(getpid()) + "\n";
    for (auto &n : app.notes) {
      const char *state = !n->visible ? "hidden"
                          : n->mapped ? "visible"
                                      : "visible (nothing to show)";
      out += n->spec.name + ": " + state + ", " +
             std::to_string(store::task_count(n->items)) + " items, " +
             n->spec.file + "\n";
    }
    out.pop_back();
    return out;
  }

  if (verb == "refresh") {
    // Picking up a config change here means `refresh` is simply "make it
    // current", which is what people expect it to mean. Unchanged config costs
    // one file read, so the nudge after every `add` stays cheap.
    if (apply_settings(app, false)) return "ok";
  }

  if (verb == "show" || verb == "hide" || verb == "toggle" || verb == "refresh") {
    std::vector<Note *> targets;
    if (target.empty() || target == "--all") {
      for (auto &n : app.notes) targets.push_back(n.get());
    } else {
      for (auto &n : app.notes)
        if (n->spec.name == target) targets.push_back(n.get());
      if (targets.empty()) return "error: no note called '" + target + "'";
    }

    // Toggling several notes acts as one switch rather than flipping each
    // independently: from a mixed state a keybind should give a clean result,
    // not another mixed one.
    bool any_visible = false;
    for (Note *n : targets) any_visible = any_visible || n->visible;

    for (Note *n : targets) {
      if (verb == "show") n->visible = true;
      else if (verb == "hide") n->visible = false;
      else if (verb == "toggle")
        n->visible = targets.size() == 1 ? !n->visible : !any_visible;
      update(*n);
    }

    if (verb == "toggle" && targets.size() == 1)
      return targets[0]->visible ? "shown" : "hidden";
    return "ok";
  }

  return "error: unknown command '" + verb + "'";
}

// ---------------------------------------------------------------------------
// daemonise
// ---------------------------------------------------------------------------

bool daemonise() {
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid > 0) _exit(0); // parent returns to the shell immediately

  if (setsid() < 0) return false;

  // Second fork: guarantees we can never reacquire a controlling terminal.
  pid = fork();
  if (pid < 0) return false;
  if (pid > 0) _exit(0);

  if (chdir("/") != 0) { /* not fatal */ }

  int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    if (null > STDERR_FILENO) close(null);
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------

namespace note {

int run(const Options &opts) {
  App app;
  app.opts = opts;
  app.settings = load_settings(opts.config_path, opts.todo_path, opts.overrides);

  if (!app.settings.error.empty())
    fprintf(stderr, "rentfree: %s: warnings:\n%s", opts.config_path.c_str(),
            app.settings.error.c_str());

  // Claim the socket before daemonising so a duplicate launch fails loudly,
  // while the user is still looking at the terminal.
  std::string err;
  if (!app.server.listen(opts.socket_path, err)) {
    fprintf(stderr, "rentfree: %s\n", err.c_str());
    return 1;
  }

  app.display = wl_display_connect(nullptr);
  if (!app.display) {
    fprintf(stderr, "rentfree: cannot connect to a Wayland compositor "
                    "(is WAYLAND_DISPLAY set?)\n");
    return 1;
  }

  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &registry_listener, &app);
  wl_display_roundtrip(app.display); // globals
  wl_display_roundtrip(app.display); // output names and scales

  if (!app.compositor || !app.shm) {
    fprintf(stderr, "rentfree: compositor is missing wl_compositor/wl_shm\n");
    return 1;
  }
  if (!app.layer_shell) {
    fprintf(stderr,
            "rentfree: this compositor does not support wlr-layer-shell, so a "
            "note cannot be pinned to the desktop\n");
    return 1;
  }
  for (const NoteSpec &spec : app.settings.notes)
    if (!spec.style.output.empty() && !pick_output(app, spec.style))
      fprintf(stderr,
              "rentfree: note '%s' wants output '%s', which doesn't exist; "
              "letting the compositor choose\n",
              spec.name.c_str(), spec.style.output.c_str());

  if (pipe2(g_signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    fprintf(stderr, "rentfree: pipe: %s\n", std::strerror(errno));
    return 1;
  }
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN); // a CLI hanging up mid-reply must not kill the notes

  app.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (app.inotify_fd < 0)
    fprintf(stderr, "rentfree: inotify unavailable, so files won't be watched: "
                    "%s\n",
            std::strerror(errno));

  if (!opts.foreground && !daemonise()) {
    fprintf(stderr, "rentfree: failed to daemonise\n");
    return 1;
  }

  app.fingerprint = describe_settings(app.settings, opts.config_path);
  watch_config(app);
  build_notes(app);

  const int WL = 0, IN = 1, SOCK = 2, SIG = 3;
  while (app.running) {
    // The prepare_read / read_events dance lets us poll our own fds alongside
    // the Wayland one without racing the display queue.
    while (wl_display_prepare_read(app.display) != 0)
      wl_display_dispatch_pending(app.display);

    if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(app.display);
      break; // compositor went away
    }

    pollfd fds[4];
    fds[WL] = {wl_display_get_fd(app.display), POLLIN, 0};
    fds[IN] = {app.inotify_fd, POLLIN, 0};
    fds[SOCK] = {app.server.fd(), POLLIN, 0};
    fds[SIG] = {g_signal_pipe[0], POLLIN, 0};

    int n = poll(fds, 4, -1);
    if (n < 0) {
      wl_display_cancel_read(app.display);
      if (errno == EINTR) continue;
      break;
    }

    if (fds[WL].revents & POLLIN) {
      if (wl_display_read_events(app.display) < 0) break;
    } else {
      wl_display_cancel_read(app.display);
    }
    if (wl_display_dispatch_pending(app.display) < 0) break;

    // Remap anything a Wayland callback asked us to bring back.
    for (auto &n : app.notes) {
      if (!n->needs_update) continue;
      n->needs_update = false;
      update(*n);
    }

    if (fds[SIG].revents & POLLIN) {
      char c;
      while (read(g_signal_pipe[0], &c, 1) > 0) {}
      app.running = false;
      continue;
    }

    if (fds[IN].revents & POLLIN) {
      bool config_changed = false;
      std::vector<Note *> dirty;

      alignas(inotify_event) char buf[8192];
      for (;;) {
        ssize_t got = read(app.inotify_fd, buf, sizeof buf);
        if (got <= 0) break;
        for (char *p = buf; p < buf + got;) {
          auto *ev = reinterpret_cast<inotify_event *>(p);
          if (ev->len > 0) {
            std::string name(ev->name);
            if (ev->wd == app.config_watch && name == app.config_name)
              config_changed = true;
            for (auto &note : app.notes) {
              if (ev->wd != note->watch || name != note->basename) continue;
              if (std::find(dirty.begin(), dirty.end(), note.get()) == dirty.end())
                dirty.push_back(note.get());
            }
          }
          p += sizeof(inotify_event) + ev->len;
        }
      }

      // A real config change rebuilds everything, so per-note updates are moot.
      if (config_changed && apply_settings(app, false)) {
        // handled by the rebuild
      } else {
        for (Note *note : dirty) update(*note);
      }
    }

    if (fds[SOCK].revents & POLLIN)
      app.server.accept_once([&](const std::string &c) { return handle_command(app, c); });

    if (fds[WL].revents & (POLLHUP | POLLERR)) break;
  }

  for (auto &n : app.notes)
    if (n->mapped) unmap(*n);
  app.server.close();
  if (app.inotify_fd >= 0) close(app.inotify_fd);
  wl_display_disconnect(app.display);
  return 0;
}

} // namespace note
