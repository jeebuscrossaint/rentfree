#pragma once
#include "config.hpp"
#include "store.hpp"

#include <cairo/cairo.h>
#include <string>

namespace render {

struct Size {
  int w = 0;
  int h = 0;
};

// Turns the list into a single Pango markup string. Doing it as one layout
// (rather than one per row) lets Pango handle wrapping, tabs and strike-through
// for us, and keeps measurement and drawing trivially consistent.
std::string build_markup(const Style &cfg, const store::List &items);

// Logical (unscaled) size of the whole note, padding included.
Size measure(const Style &cfg, const std::string &markup);

// Paints the note into `cr`. Caller has already applied the buffer scale.
void draw(cairo_t *cr, const Style &cfg, const std::string &markup, int w, int h);

} // namespace render
