#include "store.hpp"
#include "paths.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Matches "- [ ] ", "- [x] ", "* [X] ", "+ [ ] " and returns the rest.
bool parse_checkbox(const std::string &line, bool &done, std::string &rest) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i >= line.size()) return false;
  if (line[i] != '-' && line[i] != '*' && line[i] != '+') return false;
  i++;
  if (i >= line.size() || line[i] != ' ') return false;
  while (i < line.size() && line[i] == ' ') i++;
  if (i + 2 >= line.size() || line[i] != '[' || line[i + 2] != ']') return false;

  char c = line[i + 1];
  if (c == ' ') done = false;
  else if (c == 'x' || c == 'X') done = true;
  else return false;

  rest = trim(line.substr(i + 3));
  return true;
}

} // namespace

namespace store {

List load(const std::string &path) {
  List items;
  std::ifstream in(path);
  if (!in) return items; // no file yet is a perfectly good empty list

  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty()) continue;

    Item item;
    bool done = false;
    std::string rest;

    if (parse_checkbox(line, done, rest)) {
      item.kind = Kind::Task;
      item.done = done;
      item.text = rest;
    } else if (t[0] == '#') {
      item.kind = Kind::Header;
      size_t h = t.find_first_not_of('#');
      item.text = (h == std::string::npos) ? "" : trim(t.substr(h));
    } else {
      // Bare line: treat as an unfinished task so you can just type into
      // the file without ceremony.
      item.kind = Kind::Task;
      item.done = false;
      item.text = t;
    }

    if (!item.text.empty()) items.push_back(std::move(item));
  }
  return items;
}

bool save(const std::string &path, const List &items) {
  if (!paths::ensure_parent_dir(path)) return false;

  std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    for (const auto &it : items) {
      if (it.kind == Kind::Header)
        out << "# " << it.text << "\n";
      else
        out << "- [" << (it.done ? 'x' : ' ') << "] " << it.text << "\n";
    }
    out.flush();
    if (!out) { ::unlink(tmp.c_str()); return false; }
  }

  // rename(2) is atomic within a filesystem: readers see old or new, never both.
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

int task_count(const List &items) {
  int n = 0;
  for (const auto &it : items)
    if (it.kind == Kind::Task) n++;
  return n;
}

int task_index(const List &items, int nth) {
  if (nth < 1) return -1;
  int seen = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].kind != Kind::Task) continue;
    if (++seen == nth) return static_cast<int>(i);
  }
  return -1;
}

} // namespace store
