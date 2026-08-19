#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Color {
  double r = 0, g = 0, b = 0, a = 1;
};

// Parses #RGB, #RGBA, #RRGGBB, #RRGGBBAA (leading '#' optional).
bool parse_color(const std::string &s, Color &out);

// Everything about how one note looks and where it sits. Global keys in the
// config seed this; each [note] section may override any of it.
struct Style {
  // --- placement ---
  std::string anchor = "top-right"; // top|bottom|left|right|center|top-left|...
  int32_t margin_top = 24;
  int32_t margin_right = 24;
  int32_t margin_bottom = 24;
  int32_t margin_left = 24;
  std::string layer = "bottom";  // background|bottom|top|overlay
  std::string output;            // empty == let the compositor choose

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
  int32_t max_items = 0; // 0 == unlimited
};

// One note: a name, the file behind it, and how to draw it.
struct NoteSpec {
  std::string name = "default";
  std::string file; // absolute path
  Style style;
};

struct Settings {
  std::vector<NoteSpec> notes; // never empty
  std::string error;           // non-fatal complaints about the config file
};

// Reads the config. A file with no [note] sections (or no file at all) yields
// exactly one note called "default" backed by `default_file` -- i.e. the
// single-note behaviour, with zero configuration required.
//
// Relative `file =` paths resolve against the directory of `default_file`.
Settings load_settings(const std::string &path, const std::string &default_file);

// "#rrggbb", or "#rrggbbaa" when the colour isn't opaque.
std::string format_color(const Color &c);

// The fully resolved configuration, in the same syntax the config file uses --
// globals are already merged into each note, so what you see is what is being
// drawn. Paste-able straight back into a config file.
std::string describe_settings(const Settings &settings, const std::string &path);
