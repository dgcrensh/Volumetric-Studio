// L6: sheets and their files (plan docs/plans/L6-drawings.md, "Sheets and files").

#include <catch2/catch_test_macros.hpp>

#include "vs/draw/drawing.hpp"
#include "vs/draw/module.hpp"
#include "vs/draw/sheet.hpp"

using namespace vs::draw;

TEST_CASE("vs_draw names itself", "[draw]") { CHECK(module_name() == "vs_draw"); }

TEST_CASE("Numbers and scales are written as drawings write them", "[draw]") {
  CHECK(number_text(60.0, 2) == "60");
  CHECK(number_text(12.345, 2) == "12.35");
  CHECK(number_text(12.5, 2) == "12.5");
  CHECK(number_text(-0.0001, 2) == "0");
  CHECK(number_text(4.5, 0) == "5");
  CHECK(scale_text(1) == "1:1");
  CHECK(scale_text(0.5) == "1:2");
  CHECK(scale_text(0.2) == "1:5");
  CHECK(scale_text(5) == "5:1");
  CHECK(standard_scale(0.7) == 0.5);
  CHECK(standard_scale(1) == 1);
  CHECK(standard_scale(3.9) == 2);
  CHECK(standard_scale(1000) == 50);
  CHECK(standard_scale(0) == 0.001);
}

namespace {

Sheet sample() {
  Sheet s;
  s.width = 297;
  s.height = 210;
  s.title = "Sample (A4)";
  s.paths.push_back({Pen::Visible, vs::core::Id::random(), {{10, 10}, {50, 10}, {50, 40}}, false,
                     false});
  s.paths.push_back({Pen::Hidden, {}, {{0, 0}, {1, 1}}, false, false});
  s.paths.push_back({Pen::Disposable, {}, {{5, 5}, {8, 5.5}, {8, 4.5}}, true, true});
  s.texts.push_back({Pen::Dimension, {}, {30, 12}, 3.5, false, TextAlign::Centre, "⌀12.5"});
  s.texts.push_back({Pen::Disposable, {}, {20, 30}, 3.5, true, TextAlign::Right, "(4 & <5>)"});
  return s;
}

std::size_t count(const std::string& text, std::string_view what) {
  std::size_t n = 0;
  for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

}  // namespace

TEST_CASE("SVG has a path per path and escaped text, y down", "[draw][sheet]") {
  const auto svg = to_svg(sample());
  CHECK(svg.starts_with("<?xml"));
  CHECK(svg.find("viewBox=\"0 0 297 210\"") != std::string::npos);
  CHECK(count(svg, "<path ") == 3);
  CHECK(svg.find("d=\"M10 200 L50 200 L50 170\"") != std::string::npos);
  CHECK(svg.find("class=\"disposable fill\"") != std::string::npos);
  CHECK(svg.find("(4 &amp; &lt;5&gt;)") != std::string::npos);
  CHECK(svg.find("rotate(-90 20 180)") != std::string::npos);
  CHECK(svg.find("⌀12.5") != std::string::npos);
  CHECK(svg.rfind("</svg>\n") == svg.size() - 7);
}

TEST_CASE("PDF's cross-reference table points at its objects", "[draw][sheet]") {
  const auto pdf = to_pdf(sample());
  CHECK(pdf.starts_with("%PDF-1.4"));
  CHECK(pdf.ends_with("%%EOF\n"));
  const auto sx = pdf.rfind("startxref\n");
  REQUIRE(sx != std::string::npos);
  const auto xref = std::stoul(pdf.substr(sx + 10));
  CHECK(pdf.compare(xref, 4, "xref") == 0);
  // Each object's offset starts "<n> 0 obj".
  for (int n = 1; n <= 6; ++n) {
    const auto line = pdf.substr(xref + 5 + 4 + 20 * static_cast<std::size_t>(n), 10);
    const auto offset = std::stoul(line);
    CHECK(pdf.compare(offset, std::to_string(n).size() + 6, std::to_string(n) + " 0 obj") == 0);
  }
  // The page is A4 in points; the diameter sign is WinAnsi's Ø.
  CHECK(pdf.find("/MediaBox [0 0 841.89 595.276]") != std::string::npos);
  CHECK(pdf.find("(\\33012.5) Tj") != std::string::npos);
  CHECK(pdf.find("0 1 -1 0") != std::string::npos);
  // The content stream's length is right.
  const auto len_at = pdf.find("/Length ");
  const auto len = std::stoul(pdf.substr(len_at + 8));
  const auto stream = pdf.find("stream\n", len_at) + 7;
  CHECK(pdf.compare(stream + len, 9, "endstream") == 0);
}

TEST_CASE("DXF has its sections, a layer per pen and entities", "[draw][sheet]") {
  const auto dxf = to_dxf(sample());
  CHECK(dxf.starts_with("0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1009\n"));
  CHECK(dxf.ends_with("0\nEOF\n"));
  CHECK(count(dxf, "0\nLAYER\n") == 7);
  CHECK(dxf.find("2\nDISPOSABLE\n") != std::string::npos);
  CHECK(count(dxf, "0\nPOLYLINE\n") == 1);
  CHECK(count(dxf, "0\nLINE\n") == 1);
  CHECK(count(dxf, "0\nSOLID\n") == 1);
  CHECK(count(dxf, "0\nTEXT\n") == 2);
  CHECK(dxf.find("1\n%%c12.5\n") != std::string::npos);
}

TEST_CASE("write_sheet picks the format by extension", "[draw][sheet]") {
  const auto dir = std::filesystem::temp_directory_path();
  const auto svg = dir / "vs_draw_sheet.svg";
  CHECK(write_sheet(sample(), svg).empty());
  CHECK(std::filesystem::file_size(svg) == to_svg(sample()).size());
  const auto bad = write_sheet(sample(), dir / "vs_draw_sheet.png");
  REQUIRE(bad.size() == 1);
  CHECK(bad.begin()->code == "draw.file.format");
  CHECK(write_sheet(sample(), dir / "no-such-folder" / "x.pdf").begin()->code == "draw.file.write");
  CHECK(text_width("10", 3.5) > text_width("1", 3.5));
}
