#include "confedit.hpp"

#include "paths.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include <unistd.h>

namespace {

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

// Same spellings load_settings() accepts: "[note x]" or "[x]".
bool section_name(const std::string &line, std::string &name) {
  std::string t = trim(line);
  if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
  std::string inner = trim(t.substr(1, t.size() - 2));
  const std::string kw = "note";
  if (inner.rfind(kw, 0) == 0 && inner.size() > kw.size() &&
      (inner[kw.size()] == ' ' || inner[kw.size()] == '\t'))
    inner = trim(inner.substr(kw.size()));
  name = inner;
  return !name.empty();
}

// Does this line assign `key`? `commented` selects "# key = ..." instead.
bool assigns(const std::string &line, const std::string &key, bool commented) {
  std::string t = trim(line);
  if (commented) {
    if (t.empty() || (t[0] != '#' && t[0] != ';')) return false;
    t = trim(t.substr(1));
  } else {
    if (!t.empty() && (t[0] == '#' || t[0] == ';')) return false;
  }
  size_t eq = t.find('=');
  if (eq == std::string::npos) return false;

  std::string k = lower(trim(t.substr(0, eq)));
  std::replace(k.begin(), k.end(), '-', '_');
  return k == key;
}

std::vector<std::string> read_lines(const std::string &path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

bool write_lines(const std::string &path, const std::vector<std::string> &lines,
                 std::string &err) {
  if (!paths::ensure_parent_dir(path)) {
    err = "cannot create the directory for " + path;
    return false;
  }

  // Atomic, so the daemon's watch sees one complete file rather than a partial.
  std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      err = "cannot write " + tmp;
      return false;
    }
    for (const std::string &l : lines) out << l << "\n";
    out.flush();
    if (!out) {
      ::unlink(tmp.c_str());
      err = "failed writing " + tmp;
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    err = "cannot replace " + path;
    return false;
  }
  return true;
}

// Half-open line range of `section` ("" = global, i.e. up to the first header).
// Returns false if the section isn't in the file.
bool section_range(const std::vector<std::string> &lines, const std::string &section,
                   size_t &begin, size_t &end) {
  if (section.empty()) {
    begin = 0;
    end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
      std::string n;
      if (section_name(lines[i], n)) {
        end = i;
        break;
      }
    }
    return true;
  }

  for (size_t i = 0; i < lines.size(); ++i) {
    std::string n;
    if (!section_name(lines[i], n) || lower(n) != lower(section)) continue;
    begin = i + 1;
    end = lines.size();
    for (size_t j = begin; j < lines.size(); ++j) {
      std::string m;
      if (section_name(lines[j], m)) {
        end = j;
        break;
      }
    }
    return true;
  }
  return false;
}

// Keep whatever indentation the surrounding line already had.
std::string indent_of(const std::string &line) {
  size_t n = line.find_first_not_of(" \t");
  return (n == std::string::npos) ? "" : line.substr(0, n);
}

} // namespace

namespace confedit {

bool set(const std::string &path, const std::string &section, const std::string &key,
         const std::string &value, std::string &err) {
  std::vector<std::string> lines = read_lines(path);
  const std::string assignment = key + " = " + value;

  size_t begin = 0, end = 0;
  if (!section_range(lines, section, begin, end)) {
    // No such section yet: append it.
    if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("[note " + section + "]");
    lines.push_back(assignment);
    return write_lines(path, lines, err);
  }

  for (size_t i = begin; i < end; ++i) {
    if (assigns(lines[i], key, false)) { // live line: replace in place
      lines[i] = indent_of(lines[i]) + assignment;
      return write_lines(path, lines, err);
    }
  }
  for (size_t i = begin; i < end; ++i) {
    if (assigns(lines[i], key, true)) { // commented placeholder: fill it in
      lines[i] = indent_of(lines[i]) + assignment;
      return write_lines(path, lines, err);
    }
  }

  // Insert after the last line with content, so we don't land past the blank
  // lines that separate this section from the next.
  size_t at = end;
  while (at > begin && trim(lines[at - 1]).empty()) at--;
  lines.insert(lines.begin() + at, assignment);
  return write_lines(path, lines, err);
}

bool has_section(const std::string &path, const std::string &section) {
  std::vector<std::string> lines = read_lines(path);
  size_t begin = 0, end = 0;
  if (section.empty()) return true; // the global section always exists
  return section_range(lines, section, begin, end);
}

bool unset(const std::string &path, const std::string &section, const std::string &key,
           bool &changed, std::string &err) {
  changed = false;
  std::vector<std::string> lines = read_lines(path);

  size_t begin = 0, end = 0;
  if (!section_range(lines, section, begin, end)) return true; // nothing to do

  for (size_t i = begin; i < end; ++i) {
    if (!assigns(lines[i], key, false)) continue;
    lines.erase(lines.begin() + i);
    changed = true;
    return write_lines(path, lines, err);
  }
  return true;
}

} // namespace confedit
