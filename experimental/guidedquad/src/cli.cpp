// SPDX-License-Identifier: MIT
#include "guidedquad/guide_graph.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using guidedquad::TriangleMesh;

static TriangleMesh read_obj(const std::string &name) {
  std::ifstream file(name);
  if (!file) throw std::runtime_error("cannot open input OBJ");
  TriangleMesh mesh;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    std::string tag;
    in >> tag;
    if (tag == "v") {
      guidedquad::Vec3 p;
      if (!(in >> p.x >> p.y >> p.z)) throw std::runtime_error("invalid OBJ vertex");
      mesh.vertices.push_back(p);
    } else if (tag == "f") {
      std::array<std::uint32_t, 3> f{};
      for (int i = 0; i < 3; ++i) {
        std::string token;
        if (!(in >> token)) throw std::runtime_error("only triangulated OBJ is supported");
        long index = std::stol(token.substr(0, token.find('/')));
        if (index <= 0) throw std::runtime_error("OBJ indices must be positive");
        f[i] = static_cast<std::uint32_t>(index - 1);
      }
      std::string extra;
      if (in >> extra && extra[0] != '#') throw std::runtime_error("triangulate OBJ faces first");
      mesh.triangles.push_back(f);
    }
  }
  return mesh;
}

template<typename T>
static std::vector<T> read_values(const std::string &name) {
  std::ifstream file(name);
  if (!file) throw std::runtime_error("cannot open values file");
  std::vector<T> values;
  T val;
  while (file >> val) values.push_back(val);
  if (!file.eof()) throw std::runtime_error("invalid numeric values file");
  return values;
}

static void write_obj(const guidedquad::GuideGraph &g, const std::string &name) {
  std::ofstream file(name);
  if (!file) throw std::runtime_error("cannot write output OBJ");
  file << "# GuidedQuad V0.1: guide polylines only, not a quad mesh\n";
  for (const auto &n : g.nodes) {
    file << "v " << n.position.x << ' ' << n.position.y << ' ' << n.position.z << '\n';
  }
  for (const auto &p : g.paths) {
    if (p.nodes.size() < 2) continue;
    file << 'l';
    for (std::uint32_t n : p.nodes) file << ' ' << n + 1;
    file << '\n';
  }
}

int main(int argc, char **argv) {
  if (argc < 5) {
    std::cerr << "Usage: guidedquad_cli facesets input.obj face_ids.txt output.obj\n"
              << "       guidedquad_cli contours input.obj scalar_per_vertex.txt output.obj [threshold]\n";
    return 2;
  }
  try {
    const std::string mode(argv[1]);
    const auto mesh = read_obj(argv[2]);
    guidedquad::GuideGraph graph;
    if (mode == "facesets") {
      graph = guidedquad::extract_region_boundaries(mesh, read_values<int>(argv[3]));
    } else if (mode == "contours") {
      graph = guidedquad::extract_scalar_contours(mesh, read_values<double>(argv[3]),
                                                   argc > 5 ? std::stod(argv[5]) : 0.0);
    } else {
      throw std::invalid_argument("mode must be facesets or contours");
    }
    write_obj(graph, argv[4]);
    std::cout << "Guides: " << graph.nodes.size() << " nodes, "
              << graph.edges.size() << " edges, " << graph.paths.size()
              << " paths, " << graph.warnings.size() << " warnings\n";
  } catch (const std::exception &e) {
    std::cerr << "GuidedQuad error: " << e.what() << '\n';
    return 1;
  }
}
