#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glib.h>
#include <pango/pangocairo.h>

namespace {

std::string hex(const Color &c) {
  char buf[16];
  snprintf(buf, sizeof buf, "#%02x%02x%02x", (int)std::lround(c.r * 255),
           (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
  return buf;
}

// User text goes into markup, so it must be escaped or a stray '&' in a todo
// would make Pango reject the whole layout.
std::string esc(const std::string &s) {
  char *e = g_markup_escape_text(s.c_str(), -1);
  std::string out = e ? e : "";
  g_free(e);
  return out;
}

void set_source(cairo_t *cr, const Color &c) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

PangoLayout *make_layout(cairo_t *cr, const Style &cfg, const std::string &markup) {
  PangoLayout *layout = pango_cairo_create_layout(cr);

  PangoFontDescription *desc = pango_font_description_from_string(cfg.font.c_str());
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);

  int wrap = cfg.max_width - 2 * cfg.padding;
  if (wrap > 0) {
    pango_layout_set_width(layout, wrap * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  }
  if (cfg.line_spacing > 0) pango_layout_set_line_spacing(layout, (float)cfg.line_spacing);

  pango_layout_set_markup(layout, markup.c_str(), -1);
  return layout;
}

void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = std::min(r, std::min(w, h) / 2.0);
  if (r <= 0) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  const double deg = M_PI / 180.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r,     r, -90 * deg,   0 * deg);
  cairo_arc(cr, x + w - r, y + h - r, r,   0 * deg,  90 * deg);
  cairo_arc(cr, x + r,     y + h - r, r,  90 * deg, 180 * deg);
  cairo_arc(cr, x + r,     y + r,     r, 180 * deg, 270 * deg);
  cairo_close_path(cr);
}

} // namespace

namespace render {

std::string build_markup(const Style &cfg, const store::List &items) {
  std::string out;
  bool first = true;

  auto newline = [&] {
    if (!first) out += "\n";
    first = false;
  };

  if (!cfg.title.empty()) {
    newline();
    out += "<span foreground=\"" + hex(cfg.accent) + "\"><b>" + esc(cfg.title) +
           "</b></span>";
  }

  int shown = 0;
  for (const auto &it : items) {
    if (it.kind == store::Kind::Task && it.done && !cfg.show_done) continue;
    if (cfg.max_items > 0 && shown >= cfg.max_items) {
      newline();
      out += "<span foreground=\"" + hex(cfg.done_fg) + "\">…</span>";
      break;
    }

    newline();
    shown++;

    if (it.kind == store::Kind::Header) {
      out += "<span foreground=\"" + hex(cfg.accent) + "\"><b>" + esc(it.text) +
             "</b></span>";
      continue;
    }

    if (it.done) {
      out += "<span foreground=\"" + hex(cfg.accent) + "\">" + esc(cfg.check) + " </span>";
      out += "<span foreground=\"" + hex(cfg.done_fg) + "\">";
      if (cfg.strike_done) out += "<s>";
      out += esc(it.text);
      if (cfg.strike_done) out += "</s>";
      out += "</span>";
    } else {
      out += "<span foreground=\"" + hex(cfg.accent) + "\">" + esc(cfg.bullet) + " </span>";
      out += "<span foreground=\"" + hex(cfg.fg) + "\">" + esc(it.text) + "</span>";
    }
  }

  return out;
}

Size measure(const Style &cfg, const std::string &markup) {
  // A throwaway 1x1 surface is enough to own a Pango context for measurement.
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(surf);
  PangoLayout *layout = make_layout(cr, cfg, markup);

  int tw = 0, th = 0;
  pango_layout_get_pixel_size(layout, &tw, &th);

  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(surf);

  Size s;
  s.w = tw + 2 * cfg.padding;
  s.h = th + 2 * cfg.padding;
  if (s.w < 1) s.w = 1;
  if (s.h < 1) s.h = 1;
  return s;
}

void draw(cairo_t *cr, const Style &cfg, const std::string &markup, int w, int h) {
  // Start from fully transparent: the note is a floating card, not a window.
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  if (cfg.bg.a > 0.0) {
    rounded_rect(cr, 0, 0, w, h, cfg.corner_radius);
    set_source(cr, cfg.bg);
    cairo_fill(cr);
  }

  PangoLayout *layout = make_layout(cr, cfg, markup);
  cairo_move_to(cr, cfg.padding, cfg.padding);
  set_source(cr, cfg.fg);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
}

} // namespace render
