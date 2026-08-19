#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
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

bool parse_bool(const std::string &s, bool &out) {
  std::string v = s;
  std::transform(v.begin(), v.end(), v.begin(), ::tolower);
  if (v == "true" || v == "yes" || v == "on" || v == "1") { out = true; return true; }
  if (v == "false" || v == "no" || v == "off" || v == "0") { out = false; return true; }
  return false;
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

// Applies one key/value to a style. Returns false if the key is unknown, so
// the caller can report it with a line number.
bool apply_style_key(Style &st, const std::string &key, const std::string &val,
                     std::string &why) {
  auto as_int = [&](int32_t &dst) {
    try { dst = std::stoi(val); } catch (...) { why = "not a number"; }
  };
  auto as_double = [&](double &dst) {
    try { dst = std::stod(val); } catch (...) { why = "not a number"; }
  };
  auto as_bool = [&](bool &dst) {
    if (!parse_bool(val, dst)) why = "not a boolean";
  };
  auto as_color = [&](Color &dst) {
    if (!parse_color(val, dst)) why = "not a colour";
  };

  if      (key == "anchor")        st.anchor = val;
  else if (key == "layer")         st.layer = val;
  else if (key == "output")        st.output = val;
  else if (key == "font")          st.font = val;
  else if (key == "title")         st.title = val;
  else if (key == "bullet")        st.bullet = val;
  else if (key == "check")         st.check = val;
  else if (key == "margin") {
    int32_t m = 0;
    as_int(m);
    st.margin_top = st.margin_right = st.margin_bottom = st.margin_left = m;
  }
  else if (key == "margin_top")    as_int(st.margin_top);
  else if (key == "margin_right")  as_int(st.margin_right);
  else if (key == "margin_bottom") as_int(st.margin_bottom);
  else if (key == "margin_left")   as_int(st.margin_left);
  else if (key == "max_width")     as_int(st.max_width);
  else if (key == "padding")       as_int(st.padding);
  else if (key == "max_items")     as_int(st.max_items);
  else if (key == "corner_radius") as_double(st.corner_radius);
  else if (key == "line_spacing")  as_double(st.line_spacing);
  else if (key == "bg")            as_color(st.bg);
  else if (key == "fg")            as_color(st.fg);
  else if (key == "done_fg")       as_color(st.done_fg);
  else if (key == "accent")        as_color(st.accent);
  else if (key == "show_done")     as_bool(st.show_done);
  else if (key == "strike_done")   as_bool(st.strike_done);
  else if (key == "hide_when_empty") as_bool(st.hide_when_empty);
  else if (key == "respect_bars")  as_bool(st.respect_bars);
  else return false;

  return true;
}

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

} // namespace

bool parse_color(const std::string &in, Color &out) {
  std::string s = in;
  if (!s.empty() && s[0] == '#') s = s.substr(1);

  for (char c : s)
    if (hexval(c) < 0) return false;

  auto nib = [&](size_t i) { return hexval(s[i]) / 15.0; };
  auto byte = [&](size_t i) { return (hexval(s[i]) * 16 + hexval(s[i + 1])) / 255.0; };

  switch (s.size()) {
  case 3: out = {nib(0), nib(1), nib(2), 1.0};           return true;
  case 4: out = {nib(0), nib(1), nib(2), nib(3)};        return true;
  case 6: out = {byte(0), byte(2), byte(4), 1.0};        return true;
  case 8: out = {byte(0), byte(2), byte(4), byte(6)};    return true;
  default: return false;
  }
}

Settings load_settings(const std::string &path, const std::string &default_file) {
  Settings out;
  Style defaults; // global keys, inherited by every note
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
          // Re-opening a section just continues it rather than losing the keys.
          current = &*dup;
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
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      std::replace(key.begin(), key.end(), '-', '_');

      if (key == "file") {
        if (!current)
          errs << "line " << lineno << ": 'file' only means something inside a "
                                       "[note ...] section\n";
        else
          current->file = val;
        continue;
      }

      if (current) {
        current->kvs.emplace_back(lineno, key, val);
        continue;
      }

      std::string why;
      if (!apply_style_key(defaults, key, val, why))
        errs << "line " << lineno << ": unknown key '" << key << "'\n";
      else if (!why.empty())
        errs << "line " << lineno << ": '" << val << "' is " << why << "\n";
    }
  }

  const std::string base = dir_of(default_file);
  std::vector<NoteSpec> built;

  for (Decl &d : declared) {
    NoteSpec spec;
    spec.name = d.name;
    spec.style = defaults;
    for (const auto &[lineno, key, val] : d.kvs) {
      std::string why;
      if (!apply_style_key(spec.style, key, val, why))
        errs << "line " << lineno << ": unknown key '" << key << "'\n";
      else if (!why.empty())
        errs << "line " << lineno << ": '" << val << "' is " << why << "\n";
    }
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

  out.error = errs.str();
  return out;
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

namespace {

// Empty values print as "key =" with no trailing space, so the dump stays
// byte-clean and still parses back to the same empty string.
void emit(std::ostringstream &o, const char *key, const std::string &val) {
  o << key << (val.empty() ? " =" : " = ") << val << "\n";
}

void emit_style(std::ostringstream &o, const Style &st) {
  emit(o, "anchor", st.anchor);
  emit(o, "layer", st.layer);

  if (st.margin_top == st.margin_right && st.margin_top == st.margin_bottom &&
      st.margin_top == st.margin_left) {
    o << "margin = " << st.margin_top << "\n";
  } else {
    o << "margin_top = " << st.margin_top << "\n"
      << "margin_right = " << st.margin_right << "\n"
      << "margin_bottom = " << st.margin_bottom << "\n"
      << "margin_left = " << st.margin_left << "\n";
  }

  emit(o, "output", st.output);
  o << "respect_bars = " << (st.respect_bars ? "true" : "false") << "\n"
    << "max_width = " << st.max_width << "\n"
    << "padding = " << st.padding << "\n"
    << "corner_radius = " << st.corner_radius << "\n";
  emit(o, "font", st.font);
  emit(o, "title", st.title);
  emit(o, "bullet", st.bullet);
  emit(o, "check", st.check);
  o << "line_spacing = " << st.line_spacing << "\n"
    << "bg = " << format_color(st.bg) << "\n"
    << "fg = " << format_color(st.fg) << "\n"
    << "done_fg = " << format_color(st.done_fg) << "\n"
    << "accent = " << format_color(st.accent) << "\n"
    << "show_done = " << (st.show_done ? "true" : "false") << "\n"
    << "strike_done = " << (st.strike_done ? "true" : "false") << "\n"
    << "hide_when_empty = " << (st.hide_when_empty ? "true" : "false") << "\n"
    << "max_items = " << st.max_items << "\n";
}

} // namespace

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
    emit_style(o, spec.style);
  }
  return o.str();
}
