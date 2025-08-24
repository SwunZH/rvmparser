#include "Parser.h"
#include "Store.h"
#include "LinAlgOps.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdarg>

namespace {

struct Context {
  Store* store = nullptr;
  Logger logger = nullptr;
  const char* path = nullptr;

  char* errbuf = nullptr;
  size_t errbuf_size = 0;

  // parent stack: [File] -> [Model]? -> [Group]? ...
  std::vector<Node*> stack;
};

inline void set_error(Context* ctx, const char* fmt, ...)
{
  if (!ctx || !ctx->store) return;
  if (!ctx->errbuf || ctx->errbuf_size == 0) {
    ctx->store->setErrorString("parseREV: error");
    return;
  }
  va_list ap;
  va_start(ap, fmt);
#if defined(_WIN32)
  vsnprintf_s(ctx->errbuf, ctx->errbuf_size, _TRUNCATE, fmt, ap);
#else
  vsnprintf(ctx->errbuf, ctx->errbuf_size, fmt, ap);
#endif
  va_end(ap);
  ctx->store->setErrorString(ctx->errbuf);
}

inline void skip_ws(const char*& p, const char* end)
{
  while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
}

inline bool read_u32(uint32_t& out, const char*& p, const char* end)
{
  skip_ws(p, end);
  if (p >= end) return false;
  char* ep = nullptr;
  unsigned long v = std::strtoul(p, &ep, 10);
  if (ep == p) return false;
  if (v > 0xFFFFFFFFul) return false;
  out = static_cast<uint32_t>(v);
  p = ep;
  return true;
}

inline bool read_f32(float& out, const char*& p, const char* end)
{
  skip_ws(p, end);
  if (p >= end) return false;
  char* ep = nullptr;
  float v = std::strtof(p, &ep);
  if (ep == p) return false;
  out = v;
  p = ep;
  return true;
}

// Read one whole line (up to '\n'). Returns a string_view-like pair [s,e).
// Does not include trailing '\r' or '\n'.
inline bool read_line(const char*& line_s, const char*& line_e, const char*& p, const char* end)
{
  if (p >= end) return false;
  line_s = p;
  while (p < end && *p != '\n') ++p;
  line_e = p;
  if (p < end && *p == '\n') ++p; // consume '\n'
  // trim trailing '\r'
  if (line_e > line_s && *(line_e - 1) == '\r') --line_e;
  return true;
}

inline std::string to_string_trim(const char* s, const char* e)
{
  // trim leading/trailing spaces for ID line; for content strings we keep spaces.
  while (s < e && (*s == ' ' || *s == '\t')) ++s;
  while (e > s && (*(e - 1) == ' ' || *(e - 1) == '\t')) --e;
  return std::string(s, e);
}

inline const char* intern_line_as_string(Store* store, const char* s, const char* e)
{
  return store->strings.intern(s, e);
}

// Reads a chunk header:
// - First line: chunk id (e.g., "HEAD", "MODL", "CNTB", "CNTE", "PRIM", "OBST", "INSU", "END:").
// - Next: two uints on one line (as ExportRev prints "%6u%6u\n"), but we accept any whitespace between.
inline bool read_chunk_header_txt(std::string& id, const char*& p, const char* end)
{
  const char *ls = nullptr, *le = nullptr;
  if (!read_line(ls, le, p, end)) return false;
  id = to_string_trim(ls, le);
  // After the id line, there should be two uints
  uint32_t u0 = 0, u1 = 0;
  if (!read_u32(u0, p, end)) return false;
  if (!read_u32(u1, p, end)) return false;

  // Consume the rest of the line (up to '\n') if any digits were on this line.
  // Our read_u32 skips whitespace automatically, so we may be already at '\n'.
  // To be robust, read one optional trailing line break when the next char is '\r' or '\n'.
  if (p < end) {
    const char* tmp_s = nullptr;
    const char* tmp_e = nullptr;
    // Peek: if next non-space isn't a letter (i.e., still on same line), fast-forward to EOL.
    // Simpler: if next char is '\r' or '\n', consume that line end.
    if (*p == '\r' || *p == '\n') {
      read_line(tmp_s, tmp_e, p, end); // will set tmp_s to current and move p past newline; not used
    }
  }
  (void)u0; (void)u1; // currently unused (ExportRev writes 1,1)
  return true;
}

bool parse_prim_txt(Context* ctx, const std::string& id, const char*& p, const char* end)
{
  // Parent must be a Group
  if (ctx->stack.empty() || ctx->stack.back()->kind != Node::Kind::Group) {
    set_error(ctx, "PRIM/OBST/INSU outside of a CNTB group");
    return false;
  }

  uint32_t kind = 0;
  if (!read_u32(kind, p, end)) { set_error(ctx, "PRIM: missing kind"); return false; }

  auto* g = ctx->store->newGeometry(ctx->stack.back());

  // type from chunk id
  if (id == "PRIM") g->type = Geometry::Type::Primitive;
  else if (id == "OBST") g->type = Geometry::Type::Obstruction;
  else if (id == "INSU") g->type = Geometry::Type::Insulation;
  else { set_error(ctx, "Unknown geometry chunk id '%s'", id.c_str()); return false; }

  // Transparency is not present in text; inherit from parent group
  g->transparency = ctx->stack.back()->group.transparency;

  // Read 3 rows x 4 floats (ExportRev prints rows from column-major)
  float r[3][4];
  for (int i = 0; i < 3; ++i) {
    if (!read_f32(r[i][0], p, end) || !read_f32(r[i][1], p, end) ||
        !read_f32(r[i][2], p, end) || !read_f32(r[i][3], p, end)) {
      set_error(ctx, "PRIM: missing matrix row %d", i);
      return false;
    }
    // consume end of line if present
    if (p < end && (*p == '\r' || *p == '\n')) {
      const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end);
    }
  }
  // Fill column-major 3x4: data indices: [0]=r0c0,[1]=r1c0,[2]=r2c0,[3]=r0c1,[4]=r1c1,[5]=r2c1,[6]=r0c2,[7]=r1c2,[8]=r2c2,[9]=r0c3,[10]=r1c3,[11]=r2c3
  g->M_3x4.data[0] = r[0][0]; g->M_3x4.data[1] = r[1][0]; g->M_3x4.data[2] = r[2][0];
  g->M_3x4.data[3] = r[0][1]; g->M_3x4.data[4] = r[1][1]; g->M_3x4.data[5] = r[2][1];
  g->M_3x4.data[6] = r[0][2]; g->M_3x4.data[7] = r[1][2]; g->M_3x4.data[8] = r[2][2];
  g->M_3x4.data[9] = r[0][3]; g->M_3x4.data[10] = r[1][3]; g->M_3x4.data[11] = r[2][3];

  // Read bbox local: two vec3 lines
  for (int i = 0; i < 6; ++i) {
    if (!read_f32(g->bboxLocal.data[i], p, end)) { set_error(ctx, "PRIM: missing bboxLocal"); return false; }
    // After each vec3 (i=2 and i=5), consume end of line if present
    if (i == 2 || i == 5) {
      if (p < end && (*p == '\r' || *p == '\n')) {
        const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end);
      }
    }
  }
  g->bboxWorld = transform(g->M_3x4, g->bboxLocal);

  switch (kind) {
    case 1: // Pyramid
      g->kind = Geometry::Kind::Pyramid;
      if (!read_f32(g->pyramid.bottom[0], p, end) ||
          !read_f32(g->pyramid.bottom[1], p, end) ||
          !read_f32(g->pyramid.top[0],    p, end) ||
          !read_f32(g->pyramid.top[1],    p, end)) { set_error(ctx, "Pyramid: bad bottom/top"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      if (!read_f32(g->pyramid.offset[0], p, end) ||
          !read_f32(g->pyramid.offset[1], p, end) ||
          !read_f32(g->pyramid.height,     p, end)) { set_error(ctx, "Pyramid: bad offset/height"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 2: // Box
      g->kind = Geometry::Kind::Box;
      if (!read_f32(g->box.lengths[0], p, end) ||
          !read_f32(g->box.lengths[1], p, end) ||
          !read_f32(g->box.lengths[2], p, end)) { set_error(ctx, "Box: bad lengths"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 3: // RectangularTorus
      g->kind = Geometry::Kind::RectangularTorus;
      if (!read_f32(g->rectangularTorus.inner_radius, p, end) ||
          !read_f32(g->rectangularTorus.outer_radius, p, end) ||
          !read_f32(g->rectangularTorus.height,       p, end) ||
          !read_f32(g->rectangularTorus.angle,        p, end)) { set_error(ctx, "RectangularTorus: bad params"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 4: // CircularTorus
      g->kind = Geometry::Kind::CircularTorus;
      if (!read_f32(g->circularTorus.offset, p, end) ||
          !read_f32(g->circularTorus.radius, p, end) ||
          !read_f32(g->circularTorus.angle,  p, end)) { set_error(ctx, "CircularTorus: bad params"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 5: // EllipticalDish
      g->kind = Geometry::Kind::EllipticalDish;
      if (!read_f32(g->ellipticalDish.baseRadius, p, end) ||
          !read_f32(g->ellipticalDish.height,     p, end)) { set_error(ctx, "EllipticalDish: bad params"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 6: // SphericalDish
      g->kind = Geometry::Kind::SphericalDish;
      if (!read_f32(g->sphericalDish.baseRadius, p, end) ||
          !read_f32(g->sphericalDish.height,     p, end)) { set_error(ctx, "SphericalDish: bad params"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 7: // Snout
      g->kind = Geometry::Kind::Snout;
      if (!read_f32(g->snout.radius_b, p, end) ||
          !read_f32(g->snout.radius_t, p, end) ||
          !read_f32(g->snout.height,   p, end) ||
          !read_f32(g->snout.offset[0],p, end) ||
          !read_f32(g->snout.offset[1],p, end)) { set_error(ctx, "Snout: bad first line"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      if (!read_f32(g->snout.bshear[0], p, end) ||
          !read_f32(g->snout.bshear[1], p, end) ||
          !read_f32(g->snout.tshear[0], p, end) ||
          !read_f32(g->snout.tshear[1], p, end)) { set_error(ctx, "Snout: bad shear"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 8: // Cylinder
      g->kind = Geometry::Kind::Cylinder;
      if (!read_f32(g->cylinder.radius, p, end) ||
          !read_f32(g->cylinder.height, p, end)) { set_error(ctx, "Cylinder: bad params"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 9: // Sphere (ExportRev never writes this)
      set_error(ctx, "Sphere (kind=9) not supported in text format");
      return false;

    case 10: // Line
      g->kind = Geometry::Kind::Line;
      if (!read_f32(g->line.a, p, end) ||
          !read_f32(g->line.b, p, end)) { set_error(ctx, "Line: bad params"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
      break;

    case 11: { // FacetGroup
      g->kind = Geometry::Kind::FacetGroup;

      if (!read_u32(g->facetGroup.polygons_n, p, end)) { set_error(ctx, "FacetGroup: missing polygons_n"); return false; }
      if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }

      g->facetGroup.polygons = (Polygon*)ctx->store->arena.alloc(sizeof(Polygon) * g->facetGroup.polygons_n);
      for (uint32_t pi = 0; pi < g->facetGroup.polygons_n; ++pi) {
        auto& poly = g->facetGroup.polygons[pi];

        if (!read_u32(poly.contours_n, p, end)) { set_error(ctx, "FacetGroup: missing contours_n"); return false; }
        if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }

        poly.contours = (Contour*)ctx->store->arena.alloc(sizeof(Contour) * poly.contours_n);
        for (uint32_t ci = 0; ci < poly.contours_n; ++ci) {
          auto& cont = poly.contours[ci];

          if (!read_u32(cont.vertices_n, p, end)) { set_error(ctx, "FacetGroup: missing vertices_n"); return false; }
          if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }

          cont.vertices = (float*)ctx->store->arena.alloc(3 * sizeof(float) * cont.vertices_n);
          cont.normals  = (float*)ctx->store->arena.alloc(3 * sizeof(float) * cont.vertices_n);

          for (uint32_t vi = 0; vi < cont.vertices_n; ++vi) {
            // vertex vec3
            for (int k = 0; k < 3; ++k) {
              if (!read_f32(cont.vertices[3 * vi + k], p, end)) { set_error(ctx, "FacetGroup: bad vertex"); return false; }
            }
            if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
            // normal vec3
            for (int k = 0; k < 3; ++k) {
              if (!read_f32(cont.normals[3 * vi + k], p, end)) { set_error(ctx, "FacetGroup: bad normal"); return false; }
            }
            if (p < end && (*p == '\r' || *p == '\n')) { const char *ls=nullptr,*le=nullptr; read_line(ls, le, p, end); }
          }
        }
      }
      break;
    }

    default:
      set_error(ctx, "Unknown primitive kind %u", kind);
      return false;
  }

  return true;
}

bool parse_cntb_txt(Context* ctx, const char*& p, const char* end)
{
  // Parent must exist (Model or Group)
  if (ctx->stack.empty() || (ctx->stack.back()->kind != Node::Kind::Model && ctx->stack.back()->kind != Node::Kind::Group)) {
    set_error(ctx, "CNTB without valid parent (Model/Group)");
    return false;
  }

  auto* parent = ctx->stack.back();
  Node* g = ctx->store->newNode(parent, Node::Kind::Group);

  // Inherit transparency if parent is Group (same as parseRVM)
  if (parent->kind == Node::Kind::Group) {
    g->group.transparency = parent->group.transparency;
  }

  // name (full line)
  const char *ls = nullptr, *le = nullptr;
  if (!read_line(ls, le, p, end)) { set_error(ctx, "CNTB: missing name"); return false; }
  g->group.name = intern_line_as_string(ctx->store, ls, le);

  // translation vec3 in mm -> convert to meters
  if (!read_f32(g->group.translation[0], p, end) ||
      !read_f32(g->group.translation[1], p, end) ||
      !read_f32(g->group.translation[2], p, end)) { set_error(ctx, "CNTB: missing translation"); return false; }
  g->group.translation[0] *= 0.001f;
  g->group.translation[1] *= 0.001f;
  g->group.translation[2] *= 0.001f;
  if (p < end && (*p == '\r' || *p == '\n')) { read_line(ls, le, p, end); }

  // material
  if (!read_u32(g->group.material, p, end)) { set_error(ctx, "CNTB: missing material"); return false; }
  if (p < end && (*p == '\r' || *p == '\n')) { read_line(ls, le, p, end); }

  ctx->stack.push_back(g);

  // Children: loop until CNTE
  while (p < end) {
    std::string cid;
    if (!read_chunk_header_txt(cid, p, end)) { set_error(ctx, "CNTB: unexpected EOF while reading children"); return false; }
    if (cid == "CNTE") {
      // No extra payload in text; balanced end of group
      break;
    } else if (cid == "CNTB") {
      if (!parse_cntb_txt(ctx, p, end)) return false;
    } else if (cid == "PRIM" || cid == "OBST" || cid == "INSU") {
      if (!parse_prim_txt(ctx, cid, p, end)) return false;
    } else {
      set_error(ctx, "CNTB: unexpected chunk '%s'", cid.c_str());
      return false;
    }
  }

  ctx->stack.pop_back();
  return true;
}

bool parse_modl_txt(Context* ctx, const char*& p, const char* end)
{
  // Parent must be File
  if (ctx->stack.empty() || ctx->stack.back()->kind != Node::Kind::File) {
    set_error(ctx, "MODL without HEAD/File");
    return false;
  }

  // If previous top is Model, pop it (since MODL has no explicit end marker)
  if (ctx->stack.back()->kind == Node::Kind::Model) {
    ctx->stack.pop_back();
  }

  auto* m = ctx->store->newNode(ctx->stack.back(), Node::Kind::Model);
  ctx->stack.push_back(m);

  // project and name (each a full line)
  const char *ls = nullptr, *le = nullptr;
  if (!read_line(ls, le, p, end)) { set_error(ctx, "MODL: missing project"); return false; }
  m->model.project = intern_line_as_string(ctx->store, ls, le);
  if (!read_line(ls, le, p, end)) { set_error(ctx, "MODL: missing name"); return false; }
  m->model.name = intern_line_as_string(ctx->store, ls, le);

  return true;
}

bool parse_head_txt(Context* ctx, const char*& p, const char* end)
{
  // Start a new File node and push to stack
  if (!ctx->stack.empty()) {
    set_error(ctx, "HEAD encountered but stack not empty");
    return false;
  }
  auto* f = ctx->store->newNode(nullptr, Node::Kind::File);
  ctx->stack.push_back(f);

  const char *ls = nullptr, *le = nullptr;
  if (!read_line(ls, le, p, end)) { set_error(ctx, "HEAD: missing info"); return false; }
  f->file.info = intern_line_as_string(ctx->store, ls, le);
  if (!read_line(ls, le, p, end)) { set_error(ctx, "HEAD: missing note"); return false; }
  f->file.note = intern_line_as_string(ctx->store, ls, le);
  if (!read_line(ls, le, p, end)) { set_error(ctx, "HEAD: missing date"); return false; }
  f->file.date = intern_line_as_string(ctx->store, ls, le);
  if (!read_line(ls, le, p, end)) { set_error(ctx, "HEAD: missing user"); return false; }
  f->file.user = intern_line_as_string(ctx->store, ls, le);

  // No version/encoding in text; set encoding empty, set path
  f->file.encoding = ctx->store->strings.intern("");
  f->file.path = ctx->store->strings.intern(ctx->path ? ctx->path : "");

  return true;
}

} // namespace

bool parseREV(Store* store, Logger logger, const char* path, const void* ptr, size_t size)
{
  char buf[1024];
  Context ctx;
  ctx.store = store;
  ctx.logger = logger;
  ctx.path = path;
  ctx.errbuf = buf;
  ctx.errbuf_size = sizeof(buf);

  const char* base = reinterpret_cast<const char*>(ptr);
  const char* p = base;
  const char* end = base + size;

  // First chunk must be HEAD
  std::string id;
  if (!read_chunk_header_txt(id, p, end)) { set_error(&ctx, "Empty or invalid file"); return false; }
  if (id != "HEAD") { set_error(&ctx, "Expected HEAD, got '%s'", id.c_str()); return false; }
  if (!parse_head_txt(&ctx, p, end)) return false;

  // Next chunks: MODL, CNTB..., END:
  while (p < end) {
    if (!read_chunk_header_txt(id, p, end)) { set_error(&ctx, "Unexpected EOF while reading top-level chunks"); return false; }
    if (id == "END:") {
      // Pop model if left on stack
      if (!ctx.stack.empty() && ctx.stack.back()->kind == Node::Kind::Model) ctx.stack.pop_back();
      break;
    } else if (id == "MODL") {
      if (!parse_modl_txt(&ctx, p, end)) return false;
    } else if (id == "CNTB") {
      if (!parse_cntb_txt(&ctx, p, end)) return false;
    } else if (id == "CNTE") {
      // Some producers might put stray CNTE (rare). We can skip but warn.
      ctx.logger(1, "parseREV: Unexpected CNTE at root level, ignoring.");
    } else if (id == "PRIM" || id == "OBST" || id == "INSU") {
      // Should not appear at root; but if it does, error
      set_error(&ctx, "Geometry chunk '%s' outside of any group", id.c_str());
      return false;
    } else {
      set_error(&ctx, "Unrecognized chunk '%s'", id.c_str());
      return false;
    }
  }

  // Clean stack: should have just [File] or empty after popping
  if (!ctx.stack.empty() && ctx.stack.back()->kind == Node::Kind::Model) ctx.stack.pop_back();
  if (!ctx.stack.empty() && ctx.stack.back()->kind == Node::Kind::File) ctx.stack.pop_back();
  if (!ctx.stack.empty()) {
    ctx.logger(1, "parseREV: non-empty stack at end (ignored)");
  }

  store->updateCounts();
  return true;
}