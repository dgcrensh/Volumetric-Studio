#include "vs/draw/sheet.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>

#include <fmt/format.h>

namespace vs::draw {

using core::Diagnostic;
using core::Diagnostics;

namespace {

// --- pens -----------------------------------------------------------------------------

struct PenStyle {
  double width;           // mm
  const char* colour;     // SVG
  double rgb[3];          // PDF
  int dxf_colour;         // AutoCAD colour index
  const char* dxf_ltype;  // DXF line type
  std::vector<double> dash;  // mm, empty: solid
};

const PenStyle& style(Pen pen) {
  static const PenStyle kBorder{0.5, "#000000", {0, 0, 0}, 7, "CONTINUOUS", {}};
  static const PenStyle kVisible{0.35, "#000000", {0, 0, 0}, 7, "CONTINUOUS", {}};
  static const PenStyle kHidden{0.25, "#000000", {0, 0, 0}, 8, "DASHED", {3.0, 1.5}};
  static const PenStyle kBoundary{
      0.18, "#4a6fa5", {0.29, 0.435, 0.647}, 5, "PHANTOM", {8.0, 1.5, 1.5, 1.5, 1.5, 1.5}};
  static const PenStyle kSurface{
      0.25, "#1b8a7a", {0.106, 0.541, 0.478}, 4, "DASHDOT", {6.0, 1.5, 1.0, 1.5}};
  static const PenStyle kDimension{0.18, "#000000", {0, 0, 0}, 3, "CONTINUOUS", {}};
  static const PenStyle kDisposable{0.18, "#b35c00", {0.702, 0.361, 0.0}, 30, "CONTINUOUS", {}};
  switch (pen) {
    case Pen::Border: return kBorder;
    case Pen::Visible: return kVisible;
    case Pen::Hidden: return kHidden;
    case Pen::Boundary: return kBoundary;
    case Pen::Surface: return kSurface;
    case Pen::Dimension: return kDimension;
    case Pen::Disposable: return kDisposable;
  }
  return kVisible;
}

constexpr Pen kPens[] = {Pen::Border,  Pen::Visible,   Pen::Hidden,    Pen::Boundary,
                         Pen::Surface, Pen::Dimension, Pen::Disposable};

// Fixed precision, never "-0".
std::string num(double v, int decimals = 3) {
  const double scale = std::pow(10.0, decimals);
  double r = std::round(v * scale) / scale;
  if (r == 0.0) r = 0.0;
  std::string s = fmt::format("{:.{}f}", r, decimals);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

// --- UTF-8 ----------------------------------------------------------------------------

std::vector<std::uint32_t> code_points(std::string_view text) {
  std::vector<std::uint32_t> out;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = '?';
    std::size_t n = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c >> 5) == 0x6 && i + 1 < text.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
      n = 2;
    } else if ((c >> 4) == 0xE && i + 2 < text.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
      n = 3;
    } else if ((c >> 3) == 0x1E && i + 3 < text.size()) {
      n = 4;
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

constexpr std::uint32_t kDiameter = 0x2300;  // ⌀

// Helvetica's advance widths, roughly, in ems.
double char_width(std::uint32_t c) {
  if (c == ' ' || c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == '\'' ||
      c == '|' || c == 'i' || c == 'j' || c == 'l' || c == 'I' || c == 'f' || c == 't') {
    return 0.278;
  }
  if (c == '(' || c == ')' || c == '[' || c == ']' || c == '-' || c == 'r') return 0.333;
  if (c >= '0' && c <= '9') return 0.556;
  if (c == 'm' || c == 'w' || c == 'M' || c == 'W') return 0.833;
  if (c >= 'A' && c <= 'Z') return 0.667;
  if (c == kDiameter || c == 0xD8) return 0.778;
  return 0.556;
}

}  // namespace

std::string_view to_string(Pen pen) {
  switch (pen) {
    case Pen::Border: return "border";
    case Pen::Visible: return "visible";
    case Pen::Hidden: return "hidden";
    case Pen::Boundary: return "boundary";
    case Pen::Surface: return "surface";
    case Pen::Dimension: return "dimension";
    case Pen::Disposable: return "disposable";
  }
  return "visible";
}

double text_width(std::string_view text, double height) {
  double w = 0.0;
  for (const auto c : code_points(text)) w += char_width(c);
  return w * height;
}

namespace {

double aligned_x(const SheetText& t) {
  const double w = text_width(t.text, t.height);
  switch (t.align) {
    case TextAlign::Left: return 0.0;
    case TextAlign::Centre: return -w / 2;
    case TextAlign::Right: return -w;
  }
  return 0.0;
}

// --- SVG ------------------------------------------------------------------------------

std::string xml_escape(std::string_view text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

std::string to_svg(const Sheet& sheet) {
  const double h = sheet.height;
  std::string out;
  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += fmt::format(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"{}mm\" height=\"{}mm\" "
      "viewBox=\"0 0 {} {}\">\n",
      num(sheet.width), num(sheet.height), num(sheet.width), num(sheet.height));
  out += fmt::format("<title>{}</title>\n", xml_escape(sheet.title));
  out += "<style>\n";
  out += "path{fill:none;stroke-linecap:round;stroke-linejoin:round}\n";
  out += "text{font-family:Helvetica,Arial,sans-serif}\n";
  for (Pen p : kPens) {
    const PenStyle& s = style(p);
    std::string dash = "none";
    if (!s.dash.empty()) {
      dash.clear();
      for (std::size_t i = 0; i < s.dash.size(); ++i) dash += (i ? "," : "") + num(s.dash[i]);
    }
    out += fmt::format("path.{0}{{stroke:{1};stroke-width:{2};stroke-dasharray:{3}}}\n",
                       to_string(p), s.colour, num(s.width), dash);
    out += fmt::format("path.{0}.fill{{fill:{1};stroke:none}}\n", to_string(p), s.colour);
    out += fmt::format("text.{0}{{fill:{1}}}\n", to_string(p), s.colour);
  }
  out += "</style>\n";
  out += fmt::format("<rect x=\"0\" y=\"0\" width=\"{}\" height=\"{}\" fill=\"#ffffff\"/>\n",
                     num(sheet.width), num(sheet.height));
  for (const auto& p : sheet.paths) {
    if (p.points.size() < 2) continue;
    std::string d;
    for (std::size_t i = 0; i < p.points.size(); ++i) {
      d += fmt::format("{}{} {}", i == 0 ? "M" : " L", num(p.points[i].x), num(h - p.points[i].y));
    }
    if (p.closed || p.filled) d += " Z";
    out += fmt::format("<path class=\"{}{}\" d=\"{}\"", to_string(p.pen),
                       p.filled ? " fill" : "", d);
    if (!p.source.is_null()) out += fmt::format(" data-source=\"{}\"", p.source.to_string());
    out += "/>\n";
  }
  for (const auto& t : sheet.texts) {
    const char* anchor = t.align == TextAlign::Left     ? "start"
                         : t.align == TextAlign::Centre ? "middle"
                                                        : "end";
    out += fmt::format("<text class=\"{}\" x=\"{}\" y=\"{}\" font-size=\"{}\" text-anchor=\"{}\"",
                       to_string(t.pen), num(t.at.x), num(h - t.at.y), num(t.height), anchor);
    if (t.vertical) {
      out += fmt::format(" transform=\"rotate(-90 {} {})\"", num(t.at.x), num(h - t.at.y));
    }
    if (!t.source.is_null()) out += fmt::format(" data-source=\"{}\"", t.source.to_string());
    out += fmt::format(">{}</text>\n", xml_escape(t.text));
  }
  out += "</svg>\n";
  return out;
}

// --- PDF ------------------------------------------------------------------------------

namespace {

constexpr double kPoint = 72.0 / 25.4;

std::string pdf_string(std::string_view text) {
  std::string out = "(";
  for (const auto cp : code_points(text)) {
    std::uint32_t c = cp;
    if (c == kDiameter) c = 0xD8;  // Ø
    if (c > 0xFF || (c >= 0x80 && c < 0xA0)) c = '?';
    if (c == '(' || c == ')' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20 || c >= 0x80) {
      out += fmt::format("\\{:03o}", c);
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + ")";
}

}  // namespace

std::string to_pdf(const Sheet& sheet) {
  auto pt = [](double mm) { return num(mm * kPoint); };
  std::string content;
  Pen current = Pen::Border;
  bool first = true;
  auto set_pen = [&](Pen p) {
    if (!first && p == current) return;
    first = false;
    current = p;
    const PenStyle& s = style(p);
    content += fmt::format("{} w {} {} {} RG {} {} {} rg ", pt(s.width), num(s.rgb[0]),
                           num(s.rgb[1]), num(s.rgb[2]), num(s.rgb[0]), num(s.rgb[1]),
                           num(s.rgb[2]));
    std::string dash;
    for (std::size_t i = 0; i < s.dash.size(); ++i) dash += (i ? " " : "") + pt(s.dash[i]);
    content += fmt::format("[{}] 0 d\n", dash);
  };
  content += "1 J 1 j\n";
  for (const auto& p : sheet.paths) {
    if (p.points.size() < 2) continue;
    set_pen(p.pen);
    for (std::size_t i = 0; i < p.points.size(); ++i) {
      content += fmt::format("{} {} {} ", pt(p.points[i].x), pt(p.points[i].y), i == 0 ? "m" : "l");
    }
    content += p.filled ? "h f\n" : (p.closed ? "h S\n" : "S\n");
  }
  for (const auto& t : sheet.texts) {
    set_pen(t.pen);
    const double dx = aligned_x(t);
    // The text matrix: a quarter turn for vertical text.
    const double x = t.vertical ? t.at.x : t.at.x + dx;
    const double y = t.vertical ? t.at.y + dx : t.at.y;
    const std::string m = t.vertical ? "0 1 -1 0" : "1 0 0 1";
    content += fmt::format("BT /F1 {} Tf {} {} {} Tm {} Tj ET\n", pt(t.height), m, pt(x), pt(y),
                           pdf_string(t.text));
  }

  std::vector<std::string> objects;
  objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  objects.push_back(fmt::format(
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {} {}] /Resources << /Font << /F1 5 0 R >> "
      ">> /Contents 4 0 R >>",
      pt(sheet.width), pt(sheet.height)));
  objects.push_back(fmt::format("<< /Length {} >>\nstream\n{}endstream", content.size(), content));
  objects.push_back(
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  objects.push_back(fmt::format("<< /Title {} /Producer (Volumetric Studio) >>",
                                pdf_string(sheet.title)));

  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += fmt::format("{} 0 obj\n{}\nendobj\n", i + 1, objects[i]);
  }
  const std::size_t xref = out.size();
  out += fmt::format("xref\n0 {}\n0000000000 65535 f \n", objects.size() + 1);
  for (const auto o : offsets) out += fmt::format("{:010} 00000 n \n", o);
  out += fmt::format("trailer\n<< /Size {} /Root 1 0 R /Info {} 0 R >>\nstartxref\n{}\n%%EOF\n",
                     objects.size() + 1, objects.size(), xref);
  return out;
}

// --- DXF ------------------------------------------------------------------------------

namespace {

std::string dxf_text(std::string_view text) {
  std::string out;
  for (const auto cp : code_points(text)) {
    if (cp == kDiameter || cp == 0xD8) {
      out += "%%c";
    } else if (cp >= 0x20 && cp < 0x7F) {
      out += static_cast<char>(cp);
    } else {
      out += '?';
    }
  }
  return out;
}

std::string layer(Pen p) {
  std::string s{to_string(p)};
  for (auto& c : s) c = static_cast<char>(c - 'a' + 'A');
  return s;
}

}  // namespace

std::string to_dxf(const Sheet& sheet) {
  std::string out;
  auto g = [&](int code, std::string_view value) { out += fmt::format("{}\n{}\n", code, value); };
  auto gn = [&](int code, double value) { g(code, num(value, 4)); };

  g(0, "SECTION");
  g(2, "HEADER");
  g(9, "$ACADVER");
  g(1, "AC1009");
  g(9, "$EXTMIN");
  gn(10, 0);
  gn(20, 0);
  g(9, "$EXTMAX");
  gn(10, sheet.width);
  gn(20, sheet.height);
  g(0, "ENDSEC");

  g(0, "SECTION");
  g(2, "TABLES");
  g(0, "TABLE");
  g(2, "LTYPE");
  g(70, "4");
  struct LType {
    const char* name;
    std::vector<double> dash;
  };
  const LType ltypes[] = {{"CONTINUOUS", {}},
                          {"DASHED", style(Pen::Hidden).dash},
                          {"PHANTOM", style(Pen::Boundary).dash},
                          {"DASHDOT", style(Pen::Surface).dash}};
  for (const auto& lt : ltypes) {
    g(0, "LTYPE");
    g(2, lt.name);
    g(70, "0");
    g(3, lt.dash.empty() ? "Solid line" : lt.name);
    g(72, "65");
    g(73, std::to_string(lt.dash.size()));
    double total = 0;
    for (const double d : lt.dash) total += d;
    gn(40, total);
    for (std::size_t i = 0; i < lt.dash.size(); ++i) gn(49, i % 2 == 0 ? lt.dash[i] : -lt.dash[i]);
  }
  g(0, "ENDTAB");
  g(0, "TABLE");
  g(2, "LAYER");
  g(70, std::to_string(std::size(kPens)));
  for (Pen p : kPens) {
    g(0, "LAYER");
    g(2, layer(p));
    g(70, "0");
    g(62, std::to_string(style(p).dxf_colour));
    g(6, style(p).dxf_ltype);
  }
  g(0, "ENDTAB");
  g(0, "ENDSEC");

  g(0, "SECTION");
  g(2, "ENTITIES");
  for (const auto& p : sheet.paths) {
    if (p.points.size() < 2) continue;
    const std::string l = layer(p.pen);
    if (p.filled && p.points.size() <= 4) {
      g(0, "SOLID");
      g(8, l);
      // SOLID's corners go 1, 2, 4, 3; a triangle repeats its last.
      const auto& q = p.points;
      const dvec2 c[4] = {q[0], q[1], q.size() == 4 ? q[3] : q[2], q[2]};
      for (int i = 0; i < 4; ++i) {
        gn(10 + i, c[i].x);
        gn(20 + i, c[i].y);
        gn(30 + i, 0);
      }
    } else if (p.points.size() == 2 && !p.closed) {
      g(0, "LINE");
      g(8, l);
      gn(10, p.points[0].x);
      gn(20, p.points[0].y);
      gn(30, 0);
      gn(11, p.points[1].x);
      gn(21, p.points[1].y);
      gn(31, 0);
    } else {
      g(0, "POLYLINE");
      g(8, l);
      g(66, "1");
      gn(10, 0);
      gn(20, 0);
      gn(30, 0);
      g(70, p.closed || p.filled ? "1" : "0");
      for (const auto& v : p.points) {
        g(0, "VERTEX");
        g(8, l);
        gn(10, v.x);
        gn(20, v.y);
        gn(30, 0);
      }
      g(0, "SEQEND");
      g(8, l);
    }
  }
  for (const auto& t : sheet.texts) {
    g(0, "TEXT");
    g(8, layer(t.pen));
    gn(10, t.at.x);
    gn(20, t.at.y);
    gn(30, 0);
    gn(40, t.height * 0.72);  // DXF heights are cap heights
    g(1, dxf_text(t.text));
    if (t.vertical) gn(50, 90);
    if (t.align != TextAlign::Left) {
      g(72, t.align == TextAlign::Centre ? "1" : "2");
      gn(11, t.at.x);
      gn(21, t.at.y);
      gn(31, 0);
    }
  }
  g(0, "ENDSEC");
  g(0, "EOF");
  return out;
}

Diagnostics write_sheet(const Sheet& sheet, const std::filesystem::path& path) {
  Diagnostics out;
  std::string ext = path.extension().string();
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::string bytes;
  if (ext == ".svg") {
    bytes = to_svg(sheet);
  } else if (ext == ".pdf") {
    bytes = to_pdf(sheet);
  } else if (ext == ".dxf") {
    bytes = to_dxf(sheet);
  } else {
    out.add(Diagnostic::error("draw.file.format",
                              fmt::format("'{}': drawings are written as .svg, .pdf or .dxf",
                                          path.string()),
                              {{core::SubjectKind::File, {}, path.string()}}));
    return out;
  }
  std::ofstream f(path, std::ios::binary);
  if (f) f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) {
    out.add(Diagnostic::error("draw.file.write", fmt::format("couldn't write '{}'", path.string()),
                              {{core::SubjectKind::File, {}, path.string()}}));
  }
  return out;
}

}  // namespace vs::draw
