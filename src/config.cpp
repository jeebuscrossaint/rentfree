#include "config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <tuple>

namespace {

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Strips one layer of matching quotes, so `bullet = "-> "` keeps its space.
std::string unquote(const std::string &s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\'')))
    return s.substr(1, s.size() - 2);
  return s;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string dir_of(const std::string &p) {
  auto s = p.rfind('/');
  return (s == std::string::npos) ? "." : p.substr(0, s);
}

// --- shared value parsers, so config file and CLI behave identically -------

bool to_int(const std::string &v, int32_t &out, std::string &err) {
  try {
    size_t used = 0;
    long n = std::stol(v, &used);
    if (used != v.size()) throw std::invalid_argument("trailing");
    out = static_cast<int32_t>(n);
    return true;
  } catch (...) {
    err = "expected a whole number, got '" + v + "'";
    return false;
  }
}

bool to_real(const std::string &v, double &out, std::string &err) {
  try {
    size_t used = 0;
    double d = std::stod(v, &used);
    if (used != v.size()) throw std::invalid_argument("trailing");
    out = d;
    return true;
  } catch (...) {
    err = "expected a number, got '" + v + "'";
    return false;
  }
}

bool to_bool(const std::string &v, bool &out, std::string &err) {
  std::string l = lower(v);
  if (l == "true" || l == "yes" || l == "on" || l == "1") { out = true; return true; }
  if (l == "false" || l == "no" || l == "off" || l == "0") { out = false; return true; }
  err = "expected true or false, got '" + v + "'";
  return false;
}

bool to_color(const std::string &v, Color &out, std::string &err) {
  if (parse_color(v, out)) return true;
  err = "expected a colour like #rrggbb or #rrggbbaa, got '" + v + "'";
  return false;
}

std::string num(double d) {
  std::ostringstream o;
  o << d;
  return o.str();
}

// Anchor spellings people actually type, mapped onto the canonical nine.
std::string canonical_anchor(const std::string &v) {
  std::string l = lower(trim(v));
  if (l == "centre") return "center";
  if (l == "topleft") return "top-left";
  if (l == "topright") return "top-right";
  if (l == "bottomleft") return "bottom-left";
  if (l == "bottomright") return "bottom-right";
  return l;
}

bool set_enum(const Option &opt, const std::string &v, std::string &err,
              std::string &dst) {
  std::string want = (std::string(opt.name) == "anchor") ? canonical_anchor(v) : lower(trim(v));
  for (const char *c : opt.choices)
    if (want == c) { dst = want; return true; }

  err = "'" + v + "' is not a valid " + opt.name + "; expected one of ";
  for (size_t i = 0; i < opt.choices.size(); ++i)
    err += std::string(i ? ", " : "") + opt.choices[i];
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

// Each entry is {name, type, doc, choices, get, set, shorthand}. The get/set
// pairs are deliberately symmetric: get(set(x)) == x for every legal value,
// which is what makes `rentfree config` a faithful round-trip.
const std::vector<Option> &options() {
  static const std::vector<Option> table = {
      {"anchor", OptType::Enum, "which screen edge or corner the note sits at",
       {"top-left", "top", "top-right", "left", "center", "right", "bottom-left",
        "bottom", "bottom-right"},
       [](const Style &s) { return s.anchor; },
       [](Style &s, const std::string &v, std::string &e) {
         return set_enum(*find_option("anchor"), v, e, s.anchor);
       },
       false},

      {"layer", OptType::Enum, "stacking layer; bottom is above the wallpaper, below windows",
       {"background", "bottom", "top", "overlay"},
       [](const Style &s) { return s.layer; },
       [](Style &s, const std::string &v, std::string &e) {
         return set_enum(*find_option("layer"), v, e, s.layer);
       },
       false},

      {"output", OptType::Str, "connector to pin the note to, e.g. DP-1; empty lets the compositor pick",
       {},
       [](const Style &s) { return s.output; },
       [](Style &s, const std::string &v, std::string &) { s.output = v; return true; },
       false},

      {"margin", OptType::Int, "all four margins at once, in pixels",
       {},
       [](const Style &s) {
         const bool uniform = s.margin_top == s.margin_right &&
                              s.margin_top == s.margin_bottom &&
                              s.margin_top == s.margin_left;
         return uniform ? std::to_string(s.margin_top) : std::string();
       },
       [](Style &s, const std::string &v, std::string &e) {
         int32_t m = 0;
         if (!to_int(v, m, e)) return false;
         s.margin_top = s.margin_right = s.margin_bottom = s.margin_left = m;
         return true;
       },
       true},

      {"margin_top", OptType::Int, "gap from the top edge, in pixels", {},
       [](const Style &s) { return std::to_string(s.margin_top); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.margin_top, e); },
       false},
      {"margin_right", OptType::Int, "gap from the right edge, in pixels", {},
       [](const Style &s) { return std::to_string(s.margin_right); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.margin_right, e); },
       false},
      {"margin_bottom", OptType::Int, "gap from the bottom edge, in pixels", {},
       [](const Style &s) { return std::to_string(s.margin_bottom); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.margin_bottom, e); },
       false},
      {"margin_left", OptType::Int, "gap from the left edge, in pixels", {},
       [](const Style &s) { return std::to_string(s.margin_left); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.margin_left, e); },
       false},

      {"max_width", OptType::Int, "wrap text at this width, in pixels", {},
       [](const Style &s) { return std::to_string(s.max_width); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.max_width, e); },
       false},
      {"padding", OptType::Int, "space between the text and the card edge", {},
       [](const Style &s) { return std::to_string(s.padding); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.padding, e); },
       false},
      {"corner_radius", OptType::Real, "rounding of the card corners; 0 for square", {},
       [](const Style &s) { return num(s.corner_radius); },
       [](Style &s, const std::string &v, std::string &e) { return to_real(v, s.corner_radius, e); },
       false},

      {"font", OptType::Str, "Pango font description, e.g. 'Iosevka 13'", {},
       [](const Style &s) { return s.font; },
       [](Style &s, const std::string &v, std::string &) { s.font = v; return true; },
       false},
      {"title", OptType::Str, "heading drawn above the list; empty for none", {},
       [](const Style &s) { return s.title; },
       [](Style &s, const std::string &v, std::string &) { s.title = v; return true; },
       false},
      {"bullet", OptType::Str, "marker before an unfinished task", {},
       [](const Style &s) { return s.bullet; },
       [](Style &s, const std::string &v, std::string &) { s.bullet = v; return true; },
       false},
      {"check", OptType::Str, "marker before a finished task", {},
       [](const Style &s) { return s.check; },
       [](Style &s, const std::string &v, std::string &) { s.check = v; return true; },
       false},
      {"line_spacing", OptType::Real, "line height multiplier", {},
       [](const Style &s) { return num(s.line_spacing); },
       [](Style &s, const std::string &v, std::string &e) { return to_real(v, s.line_spacing, e); },
       false},

      {"bg", OptType::Color, "card background; alpha controls translucency", {},
       [](const Style &s) { return format_color(s.bg); },
       [](Style &s, const std::string &v, std::string &e) { return to_color(v, s.bg, e); },
       false},
      {"fg", OptType::Color, "colour of unfinished task text", {},
       [](const Style &s) { return format_color(s.fg); },
       [](Style &s, const std::string &v, std::string &e) { return to_color(v, s.fg, e); },
       false},
      {"done_fg", OptType::Color, "colour of finished task text", {},
       [](const Style &s) { return format_color(s.done_fg); },
       [](Style &s, const std::string &v, std::string &e) { return to_color(v, s.done_fg, e); },
       false},
      {"accent", OptType::Color, "colour of bullets, ticks and headings", {},
       [](const Style &s) { return format_color(s.accent); },
       [](Style &s, const std::string &v, std::string &e) { return to_color(v, s.accent, e); },
       false},

      {"show_done", OptType::Bool, "keep finished tasks on screen", {},
       [](const Style &s) { return std::string(s.show_done ? "true" : "false"); },
       [](Style &s, const std::string &v, std::string &e) { return to_bool(v, s.show_done, e); },
       false},
      {"strike_done", OptType::Bool, "strike through finished tasks", {},
       [](const Style &s) { return std::string(s.strike_done ? "true" : "false"); },
       [](Style &s, const std::string &v, std::string &e) { return to_bool(v, s.strike_done, e); },
       false},
      {"hide_when_empty", OptType::Bool, "draw nothing at all when the list is empty", {},
       [](const Style &s) { return std::string(s.hide_when_empty ? "true" : "false"); },
       [](Style &s, const std::string &v, std::string &e) { return to_bool(v, s.hide_when_empty, e); },
       false},
      {"respect_bars", OptType::Bool, "stay clear of panels' reserved space", {},
       [](const Style &s) { return std::string(s.respect_bars ? "true" : "false"); },
       [](Style &s, const std::string &v, std::string &e) { return to_bool(v, s.respect_bars, e); },
       false},
      {"max_items", OptType::Int, "cap the rows shown; 0 for no limit", {},
       [](const Style &s) { return std::to_string(s.max_items); },
       [](Style &s, const std::string &v, std::string &e) { return to_int(v, s.max_items, e); },
       false},
  };
  return table;
}

const Option *find_option(const std::string &name) {
  std::string key = lower(trim(name));
  std::replace(key.begin(), key.end(), '-', '_');
  for (const Option &o : options())
    if (key == o.name) return &o;
  return nullptr;
}

std::string option_hint(const Option &opt) {
  switch (opt.type) {
  case OptType::Int: return "a whole number";
  case OptType::Real: return "a number";
  case OptType::Bool: return "true|false";
  case OptType::Color: return "#rrggbb or #rrggbbaa";
  case OptType::Enum: {
    std::string s;
    for (size_t i = 0; i < opt.choices.size(); ++i)
      s += std::string(i ? "|" : "") + opt.choices[i];
    return s;
  }
  case OptType::Str: break;
  }
  return "text";
}

namespace {

// Plain edit distance, capped -- just enough for "did you mean".
size_t edit_distance(const std::string &a, const std::string &b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                         prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
    prev = cur;
  }
  return prev[b.size()];
}

} // namespace

std::vector<std::string> suggest_options(const std::string &name) {
  std::string key = lower(trim(name));
  std::replace(key.begin(), key.end(), '-', '_');

  std::vector<std::pair<size_t, std::string>> scored;
  for (const Option &o : options()) {
    std::string cand = o.name;
    // A substring match is almost always what was meant.
    size_t d = (cand.find(key) != std::string::npos && !key.empty())
                   ? 0
                   : edit_distance(key, cand);
    if (d <= 2) scored.emplace_back(d, cand);
  }
  std::sort(scored.begin(), scored.end());

  std::vector<std::string> out;
  for (auto &[d, n] : scored) {
    out.push_back(n);
    if (out.size() == 3) break;
  }
  return out;
}

// ---------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------

bool parse_color(const std::string &in, Color &out) {
  std::string s = in;
  if (!s.empty() && s[0] == '#') s = s.substr(1);

  for (char c : s)
    if (hexval(c) < 0) return false;

  auto nib = [&](size_t i) { return hexval(s[i]) / 15.0; };
  auto byte = [&](size_t i) { return (hexval(s[i]) * 16 + hexval(s[i + 1])) / 255.0; };

  switch (s.size()) {
  case 3: out = {nib(0), nib(1), nib(2), 1.0};        return true;
  case 4: out = {nib(0), nib(1), nib(2), nib(3)};     return true;
  case 6: out = {byte(0), byte(2), byte(4), 1.0};     return true;
  case 8: out = {byte(0), byte(2), byte(4), byte(6)}; return true;
  default: return false;
  }
}

std::string format_color(const Color &c) {
  char buf[16];
  auto b = [](double v) { return (int)(v * 255.0 + 0.5); };
  if (c.a >= 1.0)
    snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
  else
    snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", b(c.r), b(c.g), b(c.b), b(c.a));
  return buf;
}

// ---------------------------------------------------------------------------
// config file
// ---------------------------------------------------------------------------

namespace {

// "[note groceries]" -> "groceries"; "[groceries]" works too.
bool parse_section(const std::string &line, std::string &name) {
  if (line.size() < 2 || line.front() != '[' || line.back() != ']') return false;
  std::string inner = trim(line.substr(1, line.size() - 2));
  const std::string kw = "note";
  if (inner.rfind(kw, 0) == 0 && inner.size() > kw.size() &&
      (inner[kw.size()] == ' ' || inner[kw.size()] == '\t'))
    inner = trim(inner.substr(kw.size()));
  name = unquote(inner);
  return !name.empty();
}

void apply_kv(Style &st, const std::string &key, const std::string &val, int lineno,
              std::ostringstream &errs) {
  const Option *opt = find_option(key);
  if (!opt) {
    errs << "line " << lineno << ": unknown setting '" << key << "'";
    auto hints = suggest_options(key);
    if (!hints.empty()) {
      errs << "; did you mean ";
      for (size_t i = 0; i < hints.size(); ++i)
        errs << (i ? " or " : "") << hints[i];
    }
    errs << "\n";
    return;
  }
  std::string err;
  if (!opt->set(st, val, err)) errs << "line " << lineno << ": " << err << "\n";
}

} // namespace

Settings load_settings(const std::string &path, const std::string &default_file,
                       const std::vector<Override> &overrides) {
  Settings out;
  Style defaults;
  std::ostringstream errs;

  // Per-note keys are collected verbatim and applied on top of the globals at
  // the end, so where you put a global key in the file doesn't change anything.
  struct Decl {
    std::string name;
    std::string file;
    std::vector<std::tuple<int, std::string, std::string>> kvs;
  };
  std::vector<Decl> declared;

  std::ifstream in(path);
  if (in) {
    std::string line;
    int lineno = 0;
    Decl *current = nullptr; // null while we are in the global section

    while (std::getline(in, line)) {
      lineno++;
      std::string t = trim(line);
      if (t.empty() || t[0] == '#' || t[0] == ';') continue;

      std::string section;
      if (parse_section(t, section)) {
        auto dup = std::find_if(declared.begin(), declared.end(),
                                [&](const Decl &d) { return d.name == section; });
        if (dup != declared.end()) {
          current = &*dup; // re-opening a section continues it
          continue;
        }
        declared.push_back(Decl{section, "", {}});
        current = &declared.back();
        continue;
      }

      size_t eq = t.find('=');
      if (eq == std::string::npos) {
        errs << "line " << lineno << ": expected key = value\n";
        continue;
      }

      std::string key = trim(t.substr(0, eq));
      std::string val = unquote(trim(t.substr(eq + 1)));

      if (lower(key) == "file") {
        if (!current)
          errs << "line " << lineno
               << ": 'file' only means something inside a [note ...] section\n";
        else
          current->file = val;
        continue;
      }

      if (current)
        current->kvs.emplace_back(lineno, key, val);
      else
        apply_kv(defaults, key, val, lineno, errs);
    }
  }

  const std::string base = dir_of(default_file);
  std::vector<NoteSpec> built;

  for (Decl &d : declared) {
    NoteSpec spec;
    spec.name = d.name;
    spec.style = defaults;
    for (const auto &[lineno, key, val] : d.kvs)
      apply_kv(spec.style, key, val, lineno, errs);
    // An undeclared file defaults to <name>.md next to the default note.
    spec.file = d.file.empty() ? d.name + ".md" : d.file;
    if (spec.file[0] != '/') spec.file = base + "/" + spec.file;
    built.push_back(std::move(spec));
  }

  // The default note always exists: declaring a section ADDS a note, it doesn't
  // replace the one your existing todo file is behind. Otherwise adding your
  // second note silently takes the first one off screen.
  //
  // Claim it explicitly -- by naming a section "default", or by pointing one at
  // the default file -- and that section wins instead.
  const bool claimed =
      std::any_of(built.begin(), built.end(), [&](const NoteSpec &s) {
        return s.name == "default" || s.file == default_file;
      });

  if (!claimed) {
    NoteSpec spec;
    spec.name = "default";
    spec.file = default_file;
    spec.style = defaults;
    out.notes.push_back(std::move(spec)); // first, so bare `add` targets it
  }
  for (NoteSpec &spec : built) out.notes.push_back(std::move(spec));

  // Command-line overrides land last, on every note: the most explicit thing
  // the user typed should win over anything in the file.
  for (const Override &ov : overrides) {
    const Option *opt = find_option(ov.key);
    if (!opt) continue; // main() has already reported this
    for (NoteSpec &spec : out.notes) {
      std::string err;
      if (!opt->set(spec.style, ov.value, err)) errs << "--" << ov.key << ": " << err << "\n";
    }
  }

  out.error = errs.str();
  return out;
}

std::string describe_settings(const Settings &settings, const std::string &path) {
  std::ostringstream o;

  std::ifstream probe(path);
  if (probe)
    o << "# from " << path << "\n";
  else
    o << "# " << path << " does not exist -- these are the built-in defaults\n";
  o << "#\n# Globals are already merged into each note, so every value below is\n"
       "# what that note is actually drawn with.\n";

  for (const NoteSpec &spec : settings.notes) {
    o << "\n[note " << spec.name << "]\n";
    o << "file = " << spec.file << "\n";
    for (const Option &opt : options()) {
      if (opt.shorthand) continue; // would duplicate the real settings
      std::string v = opt.get(spec.style);
      o << opt.name << (v.empty() ? " =" : " = ") << v << "\n";
    }
  }
  return o.str();
}
