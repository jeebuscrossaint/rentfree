#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Color {
  double r = 0, g = 0, b = 0, a = 1;
};

// Parses #RGB, #RGBA, #RRGGBB, #RRGGBBAA (leading '#' optional).
bool parse_color(const std::string &s, Color &out);
// "#rrggbb", or "#rrggbbaa" when the colour isn't opaque.
std::string format_color(const Color &c);

// Everything about how one note looks and where it sits. Global keys in the
// config seed this; each [note] section may override any of it.
struct Style {
  // --- placement ---
  std::string anchor = "top-right";
  int32_t margin_top = 24;
  int32_t margin_right = 24;
  int32_t margin_bottom = 24;
  int32_t margin_left = 24;
  std::string layer = "bottom";
  std::string output;

  // --- box ---
  int32_t max_width = 420;
  int32_t padding = 18;
  double corner_radius = 12;

  // --- text ---
  std::string font = "Sans 12";
  std::string title;
  std::string bullet = "▪";
  std::string check = "✓";
  double line_spacing = 1.35;

  // --- colours ---
  Color bg{0, 0, 0, 0.45};
  Color fg{0.94, 0.94, 0.96, 1.0};
  Color done_fg{0.55, 0.57, 0.62, 1.0};
  Color accent{0.45, 0.72, 1.0, 1.0};

  // --- behaviour ---
  bool show_done = true;
  bool strike_done = true;
  bool hide_when_empty = true;
  bool respect_bars = true;
  int32_t max_items = 0;
};

// ---------------------------------------------------------------------------
// the option registry
//
// One table describes every setting: how to parse it, how to print it, what it
// means and what values are legal. The config parser, `rentfree set/get`, the
// --key command-line overrides, the effective-config dump and the shell
// completions all read from here, so adding a setting in one place teaches all
// of them about it at once.
// ---------------------------------------------------------------------------

enum class OptType { Str, Int, Real, Bool, Color, Enum };

struct Option {
  const char *name;
  OptType type;
  const char *doc;
  std::vector<const char *> choices; // Enum only; also drives completion
  // Canonical string form of the current value.
  std::string (*get)(const Style &);
  // Parses `value` into the style, or explains what went wrong.
  bool (*set)(Style &, const std::string &value, std::string &err);
  // True for shorthands like `margin`, which write several real settings.
  // Skipped when dumping so the dump stays a faithful round-trip.
  bool shorthand;
};

const std::vector<Option> &options();

// Accepts dashes for underscores, so --max-width and max_width both work.
const Option *find_option(const std::string &name);

// Names close to `name`, for "did you mean" on a typo.
std::vector<std::string> suggest_options(const std::string &name);

// Human-readable value hint: "an integer", "top-left|top|...", "#rrggbb[aa]".
std::string option_hint(const Option &opt);

// ---------------------------------------------------------------------------

// One note: a name, the file behind it, and how to draw it.
struct NoteSpec {
  std::string name = "default";
  std::string file;
  Style style;
};

struct Settings {
  std::vector<NoteSpec> notes; // never empty
  std::string error;
};

// A --key=value given on the command line, applied after everything in the
// config file so the most explicit thing wins.
struct Override {
  std::string key;
  std::string value;
};

// Reads the config. A file with no [note] sections still yields the "default"
// note backed by `default_file`; declaring sections adds notes rather than
// replacing it.
//
// Relative `file =` paths resolve against the directory of `default_file`.
Settings load_settings(const std::string &path, const std::string &default_file,
                       const std::vector<Override> &overrides = {});

// The fully resolved configuration in config-file syntax -- globals already
// merged into each note, so what you see is what is being drawn. Round-trips.
std::string describe_settings(const Settings &settings, const std::string &path);
