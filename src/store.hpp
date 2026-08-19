#pragma once
#include <string>
#include <vector>

namespace store {

enum class Kind {
  Task,   // "- [ ] foo" / "- [x] foo" / bare text
  Header, // "# foo"
};

struct Item {
  Kind kind = Kind::Task;
  bool done = false;
  std::string text;
};

// The on-disk list. Missing file == empty list, not an error.
using List = std::vector<Item>;

// Parses leniently so the file stays comfortable to hand-edit:
//   "- [ ] foo" / "- [x] foo" / "* [X] foo"  -> task
//   "# foo"                                  -> header
//   "foo"                                    -> undone task
//   blank                                    -> skipped
List load(const std::string &path);

// Writes normalised markdown, atomically (tmp file + rename), so a reader
// never observes a half-written list.
bool save(const std::string &path, const List &items);

// Indices used by the CLI are 1-based and count *tasks only*, so headers
// don't shift the numbers you see on screen. Returns -1 if out of range.
int task_index(const List &items, int nth);

// Number of Kind::Task entries.
int task_count(const List &items);

} // namespace store
