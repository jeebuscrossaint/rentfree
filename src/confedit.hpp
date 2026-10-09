#pragma once
#include <string>

// Surgical edits to the config file. These rewrite the file rather than
// regenerating it, so comments, blank lines and key order all survive -- the
// config stays something you'd want to open by hand.
namespace confedit {

// `section` is "" for the global section, or a note name.
//
// Order of preference: replace a live `key = ...` line in place; otherwise
// replace a commented-out `# key = ...` placeholder in place (the seeded config
// is nothing but placeholders, so this fills them in where they already sit);
// otherwise insert at the end of the section. A missing section is appended, a
// missing file is created.
bool set(const std::string &path, const std::string &section, const std::string &key,
         const std::string &value, std::string &err);

// Deletes the live line, leaving comments alone. `changed` reports whether
// there was anything to delete.
// Is this section physically present in the file?
bool has_section(const std::string &path, const std::string &section);

bool unset(const std::string &path, const std::string &section, const std::string &key,
           bool &changed, std::string &err);

} // namespace confedit
