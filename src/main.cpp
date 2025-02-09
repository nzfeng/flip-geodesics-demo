#include "geometrycentral/surface/edge_length_geometry.h"
#include "geometrycentral/surface/flip_geodesics.h"
#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/mesh_graph_algorithms.h"
#include "geometrycentral/surface/meshio.h"
#include "geometrycentral/surface/polygon_soup_mesh.h"
#include "geometrycentral/surface/surface_mesh.h"
#include "geometrycentral/surface/vertex_position_geometry.h"
#include "geometrycentral/utilities/timing.h"

#include "polyscope/point_cloud.h"
#include "polyscope/polyscope.h"
#include "polyscope/surface_mesh.h"

#include "args/args.hxx"
#include "imgui.h"

using namespace geometrycentral;
using namespace geometrycentral::surface;

// const std::string DIRECTORY="sdf-heat-method";
const std::string DIRECTORY = "gwn-curved";

// == Geometry-central data
std::unique_ptr<ManifoldSurfaceMesh> mesh;
std::unique_ptr<VertexPositionGeometry> geometry;

// Polyscope visualization handle, to quickly add data to the surface
polyscope::SurfaceMesh* psMesh;

// An edge network while processing flips
std::unique_ptr<FlipEdgeNetwork> edgeNetwork;

bool DISPLAY_CURVE = false;
std::vector<SurfacePoint> curveNodes;
std::vector<std::vector<std::array<int, 2>>> curveEdges;

// UI parameters
std::string loadedFilename = "";
bool withGUI = true;
bool iterativeShortenUseIterationCap = false;
int iterativeShortenIterationCap = 1;
bool straightenAtMarked = true;
bool useIterativeShortenLengthLim = false;
float iterativeShortenLengthLim = 0.5;

int nBezierIters = 3;

bool vizAllIntrinsicEdges = false;
float angleEPS = 1e-5;
float splitAngleDeg = 10;
float refineAreaThresh = std::numeric_limits<float>::infinity();
float refineAngleThresh = 25.;
int maxInsertions = -1;

// Fancy path construction
bool fancyPathClosed = false;
std::vector<Vertex> fancyPathVerts;
std::vector<std::pair<size_t, int>> fancyPathVertsPs;
VertexData<double> fancyPathVertexNumbers;
bool fancyPathMarkVerts = false;
int OFFSET_IDX = 0;
bool SEQUENCE_OF_NODES = false;


/*
 * Read input curve on the surface.
 * Curve is represented as a series of SurfacePoints; edges are represented as pairs of node indices (this
 * representation is more convenient for curves with intersections.)
 */
void readInputCurves(SurfaceMesh& mesh, const std::string& filename, std::vector<SurfacePoint>& curveNodes,
                     std::vector<std::vector<std::array<int, 2>>>& curveEdges) {

  std::ifstream curr_file(filename.c_str());
  std::string line;
  std::string X;  // re-written variable for holding the leading (indicator) char
  size_t idx;     // re-written variable for holding element index
  double tEdge;   // re-written variable for holding halfedge param
  double a, b, c; // barycentric coords for SurfacePoints within a face
  SurfacePoint pt;
  int p1, p2;
  bool flip;
  std::vector<bool> flips;

  if (curr_file.is_open()) {
    while (!curr_file.eof()) {
      getline(curr_file, line);
      // Ignore any newlines
      if (line == "") {
        continue;
      }
      std::istringstream iss(line);
      iss >> X;
      if (X == "v") {
        iss >> idx;
        pt = SurfacePoint(mesh.vertex(idx));
        curveNodes.push_back(pt);
      } else if (X == "e") {
        iss >> idx >> tEdge;
        pt = SurfacePoint(mesh.edge(idx), tEdge);
        curveNodes.push_back(pt);
      } else if (X == "f") {
        iss >> idx >> a >> b >> c;
        Vector3 faceCoords = {a, b, c};
        pt = SurfacePoint(mesh.face(idx), faceCoords);
        curveNodes.push_back(pt);
      } else if (X == "l") {
        iss >> p1 >> p2;
        curveEdges[curveEdges.size() - 1].push_back({p1, p2});
      } else if (X == "n") {
        iss >> flip; // empty char should be read as false (I think)
        flips.push_back(flip);
        curveEdges.push_back(std::vector<std::array<int, 2>>());
      }
    }
    curr_file.close();

    assert(curveEdges.size() == flips.size());
    for (size_t i = 0; i < flips.size(); i++) {
      if (!flips[i]) continue;
      for (size_t j = 0; j < curveEdges[i].size(); j++) {
        curveEdges[i][j] = {curveEdges[i][j][1], curveEdges[i][j][0]};
      }
      std::reverse(curveEdges[i].begin(), curveEdges[i].end());
    }
  } else {
    std::cerr << "Could not open file <" << filename << ">" << std::endl;
  }
}

/*
 * Register input curves as a curve network to display.
 * This function is for if the curves are defined implicitly via SurfacePoints on a mesh.
 */
void displayCurves(VertexPositionGeometry& geometry, polyscope::SurfaceMesh* psMesh,
                   const std::vector<SurfacePoint>& curveNodes,
                   const std::vector<std::vector<std::array<int, 2>>>& curveEdges, const std::string& name) {


  std::vector<Vector3> positions;
  std::vector<std::array<int, 2>> edgeInds;

  // Don't render points that aren't used in any edges.
  for (size_t i = 0; i < curveEdges.size(); i++) {
    for (size_t j = 0; j < curveEdges[i].size(); j++) {
      if (curveEdges[i][j][1] != -1) {
        Vector3 a = curveNodes[curveEdges[i][j][0]].interpolate(geometry.inputVertexPositions);
        Vector3 b = curveNodes[curveEdges[i][j][1]].interpolate(geometry.inputVertexPositions);
        positions.push_back(a);
        positions.push_back(b); // will repeat nodes
        int n = positions.size();
        edgeInds.push_back({n - 2, n - 1});
      } else {
        Vector3 a = curveNodes[curveEdges[i][j][0]].interpolate(geometry.inputVertexPositions);
        positions.push_back(a);
      }
    }
  }

  auto pathQ = psMesh->addSurfaceGraphQuantity(name, positions, edgeInds);
  pathQ->setEnabled(true);
  pathQ->setColor({0, 0, 0});
  pathQ->setRadius(0.001);
}

// ====== Path related stuff

void updatePathViz() {
  if (!edgeNetwork) {
    polyscope::error("tried to visualize path, but no path exists");
    return;
  }

  // remove everything
  psMesh->removeAllQuantities();

  if (DISPLAY_CURVE) displayCurves(*geometry, psMesh, curveNodes, curveEdges, "input curves");

  auto pathQ = psMesh->addSurfaceGraphQuantity("path edges", edgeNetwork->getPathPolyline3D());
  pathQ->setEnabled(true);
  pathQ->setColor(polyscope::render::RGB_RED);
  pathQ->setRadius(0.001);

  if (vizAllIntrinsicEdges) {
    auto edgeQ = psMesh->addSurfaceGraphQuantity("intrinsic edges", edgeNetwork->getAllEdgePolyline3D());
    edgeQ->setEnabled(true);
    edgeQ->setColor(polyscope::render::RGB_ORANGE);
    edgeQ->setRadius(0.0005);
  }

  { // Marked vertices
    std::vector<Vector3> cloud;
    for (Vertex v : edgeNetwork->mesh.vertices()) {
      if (edgeNetwork->isMarkedVertex[v]) {
        SurfacePoint p = edgeNetwork->tri->vertexLocations[v];
        Vector3 p3d = p.interpolate(geometry->inputVertexPositions);
        cloud.push_back(p3d);
      }
    }
    // Visualize balls at marked
    auto psCloud = polyscope::registerPointCloud("marked vertices", cloud);
    psCloud->setPointColor(polyscope::render::RGB_BLACK);
  }
}

void createPathFromPoints() {

  long long int iVStart = psMesh->selectVertex();
  long long int iVEnd = psMesh->selectVertex();

  if (iVStart == -1 || iVEnd == -1) return;

  edgeNetwork =
      FlipEdgeNetwork::constructFromDijkstraPath(*mesh, *geometry, mesh->vertex(iVStart), mesh->vertex(iVEnd));
  if (edgeNetwork == nullptr) {
    polyscope::warning("could not initialize edge path between vertices");
    return;
  }
  edgeNetwork->posGeom = geometry.get();

  // MY CODE: write to file
  if (!SEQUENCE_OF_NODES) {
    std::string filename = "../../" + DIRECTORY + "/data/curve.txt";
    std::ofstream myfile(filename);
    std::vector<std::vector<SurfacePoint>> int_path = edgeNetwork->getPathPolyline();
    if (!fancyPathClosed) {
      int idx = 0;
      for (size_t i = 0; i < int_path.size(); i++) {
        myfile << "n\n";
        for (size_t j = 0; j < int_path[i].size(); j++) {
          SurfacePoint pt = int_path[i][j];
          myfile << "v " << pt.vertex.getIndex() << "\n";
          // This code doesn't check if the curve is a loop; if so, delete the last SurfacePoint (because it
          // will be a repeat of the 1st) and change the 2nd endpoint of the last line segment to the 0th
          // index.
          if (j < int_path[i].size() - 1) {
            myfile << "l " << idx + OFFSET_IDX << " " << idx + 1 + OFFSET_IDX << "\n";
          }
          idx++;
        }
      }
    } else {
      int idx = 0;
      assert(int_path.size() == 1);
      for (size_t i = 0; i < int_path.size(); i++) {
        myfile << "n\n";
        size_t N = int_path[i].size();
        for (size_t j = 0; j < N; j++) {
          SurfacePoint pt = int_path[i][j % (N - 1)];
          if (j % (N - 1) == j) {
            myfile << "v " << pt.vertex.getIndex() << "\n";
          }

          if (j < int_path[i].size() - 1) {
            myfile << "l " << j + OFFSET_IDX << " " << (j + 1) % (N - 1) + OFFSET_IDX << "\n";
          }
        }
      }
    }
    myfile.close();
  } else {
    // Output curve as a sequence of (unique) surface points
    std::ofstream myfile("../../contact-parameterization/data/curve.txt");
    std::vector<std::vector<SurfacePoint>> int_path = edgeNetwork->getPathPolyline();

    for (size_t i = 0; i < int_path.size(); i++) {
      myfile << "n\n";
      size_t N = int_path[i].size();
      if (fancyPathClosed) N -= 1;
      for (size_t j = 0; j < N; j++) {
        SurfacePoint pt = int_path[i][j];
        if (pt.type == SurfacePointType::Vertex) {
          myfile << "v " << pt.vertex.getIndex() << "\n";
        } else if (pt.type == SurfacePointType::Edge) {
          myfile << "e " << pt.edge.getIndex() << " " << pt.tEdge << "\n";
        } else if (pt.type == SurfacePointType::Face) {
          myfile << "f " << pt.face.getIndex() << " " << pt.faceCoords[0] << " " << pt.faceCoords[1] << " "
                 << pt.faceCoords[2] << "\n";
        }
      }
    }
    myfile.close();
  }

  updatePathViz();
}

void createPathFromEdgeSet() {

  EdgeData<bool> edgeSet(*mesh, false);
  VertexData<bool> extraMarkedVertices(*mesh, false);

  { // Load the edge set from file
    std::ifstream inStream("path_edges.txt");
    if (!inStream) {
      polyscope::error("could not read path_edges.txt");
      return;
    }

    for (std::string line; std::getline(inStream, line);) {
      std::istringstream lineStream(line);

      size_t indA, indB;
      lineStream >> indA;
      lineStream >> indB;

      for (Halfedge he : mesh->vertex(indA).incomingHalfedges()) {
        if (he.vertex().getIndex() == indB) {
          edgeSet[he.edge()] = true;
        }
      }
    }
  }

  { // Load extra marked vertices from file
    std::ifstream inStream("marked_vertices.txt");
    if (!inStream) {
      polyscope::warning("could not read marked_vertices.txt");
    }

    for (std::string line; std::getline(inStream, line);) {
      std::istringstream lineStream(line);

      size_t ind;
      lineStream >> ind;
      extraMarkedVertices[mesh->vertex(ind)] = true;
    }
  }

  edgeNetwork = FlipEdgeNetwork::constructFromEdgeSet(*mesh, *geometry, edgeSet, extraMarkedVertices);
  if (edgeNetwork == nullptr) {
    polyscope::warning("could not initialize path from file");
    return;
  }
  edgeNetwork->posGeom = geometry.get();

  updatePathViz();
}

void createPathFromObjLines() {

  auto findHalfedge = [&](Vertex vA, Vertex vB) {
    for (Halfedge he : vA.outgoingHalfedges()) {
      if (he.twin().vertex() == vB) return he;
    }
    return Halfedge();
  };

  std::vector<std::vector<Halfedge>> paths;

  { // Load the edge set from file
    std::ifstream inStream(loadedFilename);
    if (!inStream) {
      polyscope::error("could not read: " + loadedFilename);
      return;
    }

    for (std::string line; std::getline(inStream, line);) {
      if (line.size() < 2 || line[0] != 'l' || line[1] != ' ') continue; // look for line ('l' first char)

      std::istringstream lineStream(line.substr(2)); // all but first char and space

      // parse out list of indices
      std::vector<size_t> inds;
      std::string token;
      while (std::getline(lineStream, token, ' ')) {
        std::stringstream tokenStream(token);
        size_t i;
        tokenStream >> i;
        inds.push_back(i - 1);
      }

      // build vertices
      paths.emplace_back();
      std::vector<Halfedge>& path = paths.back();
      for (size_t i = 1; i < inds.size(); i++) {
        Vertex vA = mesh->vertex(inds[i - 1]);
        Vertex vB = mesh->vertex(inds[i]);

        Halfedge he = findHalfedge(vA, vB);
        if (he == Halfedge()) {
          polyscope::warning("vertices " + std::to_string(inds[i - 1]) + " and " + std::to_string(inds[i]) +
                             " are not connected");
          return;
        }
        path.push_back(he);
      }

      // Try to close a loop
      Halfedge lastHe = findHalfedge(mesh->vertex(inds.back()), mesh->vertex(inds.front()));
      if (lastHe != Halfedge()) {
        std::cout << "    closing loop with halfedge " << lastHe << std::endl;
        path.push_back(lastHe);
      }

      std::cout << "  ...found path with " << path.size() << " segments." << std::endl;
    }
  }

  std::cout << "Loaded line list with " << paths.size() << " paths." << std::endl;

  edgeNetwork.reset(new FlipEdgeNetwork(*mesh, *geometry, paths));
  if (edgeNetwork == nullptr) {
    polyscope::warning("could not initialize path from file");
    return;
  }
  edgeNetwork->posGeom = geometry.get();

  updatePathViz();
}

void createPathFromDijkstraList() {

  // Create an  (initially-empty) edge network
  edgeNetwork = std::unique_ptr<FlipEdgeNetwork>(new FlipEdgeNetwork(*mesh, *geometry, {}));
  edgeNetwork->posGeom = geometry.get();

  { // Load the edge set from file
    std::ifstream inStream("path_pairs.txt");
    if (!inStream) {
      polyscope::error("could not read path_pairs.txt");
      return;
    }

    for (std::string line; std::getline(inStream, line);) {
      std::istringstream lineStream(line);

      size_t indA, indB;
      lineStream >> indA;
      lineStream >> indB;

      Vertex vA = edgeNetwork->tri->intrinsicMesh->vertex(indA);
      Vertex vB = edgeNetwork->tri->intrinsicMesh->vertex(indB);

      std::cout << "loading path from " << vA << " to " << vB << std::endl;

      std::vector<Halfedge> path = shortestEdgePath(*edgeNetwork->tri, vA, vB);
      edgeNetwork->addPath(path);
    }
  }

  updatePathViz();
}

void createPathFromSeg() {

  // read face segmentation inds
  std::ifstream inStream("cut.seg");
  if (!inStream) {
    polyscope::warning("could not read cut.seg");
  }
  FaceData<int> segs(*mesh);
  size_t iF = 0;
  for (std::string line; std::getline(inStream, line);) {
    std::istringstream lineStream(line);
    int ind;
    lineStream >> ind;
    if (iF >= mesh->nFaces()) {
      polyscope::warning("segmentation file doesn't match number of faces");
      return;
    }
    segs[iF] = ind;
    iF++;
  }

  psMesh->addFaceScalarQuantity("segmentation", segs);

  // Make cut along boundary
  EdgeData<bool> edgeSet(*mesh, false);
  for (Halfedge he : mesh->halfedges()) {
    if (he.edge().isBoundary()) continue;
    if (segs[he.face()] != segs[he.twin().face()]) {
      edgeSet[he.edge()] = true;
    }
  }

  VertexData<bool> extraMarkedVertices(*mesh, false); // none

  edgeNetwork = FlipEdgeNetwork::constructFromEdgeSet(*mesh, *geometry, edgeSet, extraMarkedVertices);
  if (edgeNetwork == nullptr) {
    polyscope::warning("could not initialize path from file");
    return;
  }
  edgeNetwork->posGeom = geometry.get();

  updatePathViz();
}

void createPathFromUVCut() {

  // (re)-load the obj file, and pull out corner coords
  PolygonSoupMesh reloadMesh(loadedFilename);
  if (reloadMesh.paramCoordinates.empty()) {
    polyscope::warning("could not load UVs from mesh file");
    return;
  }

  CornerData<Vector2> uvCoords(*mesh);
  for (Face f : mesh->faces()) {
    size_t iFace = f.getIndex();
    size_t iC = 0;
    for (Corner c : f.adjacentCorners()) {
      Vector2 uv = reloadMesh.paramCoordinates[iFace][iC];
      uvCoords[c] = uv;
      iC++;
    }
  }
  psMesh->addParameterizationQuantity("loaded UVs", uvCoords);

  // Detect cut as gap in UVs
  EdgeData<bool> edgeSet(*mesh, false);
  for (Halfedge he : mesh->halfedges()) {
    if (he.edge().isBoundary()) continue;
    if (uvCoords[he.corner()] != uvCoords[he.twin().next().corner()]) {
      edgeSet[he.edge()] = true;
    }
  }

  VertexData<bool> extraMarkedVertices(*mesh, false); // none

  edgeNetwork = FlipEdgeNetwork::constructFromEdgeSet(*mesh, *geometry, edgeSet, extraMarkedVertices);
  if (edgeNetwork == nullptr) {
    polyscope::warning("could not initialize path from file");
    return;
  }
  edgeNetwork->posGeom = geometry.get();

  updatePathViz();
}

void checkPath() {
  if (edgeNetwork == nullptr) {
    polyscope::warning("no path network");
    return;
  }

  edgeNetwork->validate();

  double minAngle = edgeNetwork->minAngleIsotopy();
  std::cout << "min angle = " << minAngle << std::endl;
  if (minAngle < (M_PI - edgeNetwork->EPS_ANGLE)) {
    polyscope::warning("min angle in path is " + std::to_string(minAngle));
  }
}

void makeDelaunay() {
  if (edgeNetwork == nullptr) {
    polyscope::warning("no path network");
    return;
  }
  edgeNetwork->makeDelaunay();
  updatePathViz();
}

void locallyShorten() {
  if (edgeNetwork == nullptr) {
    polyscope::warning("no path network");
    return;
  }

  // reset counters
  edgeNetwork->nFlips = 0;
  edgeNetwork->nShortenIters = 0;

  edgeNetwork->EPS_ANGLE = angleEPS;
  edgeNetwork->straightenAroundMarkedVertices = straightenAtMarked;

  size_t iterLim = iterativeShortenUseIterationCap ? iterativeShortenIterationCap : INVALID_IND;
  double lengthLim = useIterativeShortenLengthLim ? iterativeShortenLengthLim : 0.;

  // re-add all wedges to angle queue
  // (this is generally extra unneeded work, but makes things simpler in case e.g. settings ^^^ changes after previous
  // flips)
  edgeNetwork->addAllWedgesToAngleQueue();

  if (iterativeShortenUseIterationCap) {
    edgeNetwork->iterativeShorten(iterLim, lengthLim);
  } else {

    START_TIMING(shorten)
    edgeNetwork->iterativeShorten(iterLim, lengthLim);
    edgeNetwork->getPathPolyline();
    FINISH_TIMING_PRINT(shorten)

    checkPath();
  }

  std::cout << "shortening performed " << edgeNetwork->nShortenIters << " iterations, with a total of "
            << edgeNetwork->nFlips << " flips. " << std::endl;

  updatePathViz();

  // MY CODE: write to file
  if (!SEQUENCE_OF_NODES) {
    std::string filename = "../../" + DIRECTORY + "/data/curve.txt";
    std::ofstream myfile(filename);
    std::vector<std::vector<SurfacePoint>> int_path = edgeNetwork->getPathPolyline();
    if (!fancyPathClosed) {
      int idx = 0;
      for (size_t i = 0; i < int_path.size(); i++) {
        myfile << "n\n";
        for (size_t j = 0; j < int_path[i].size(); j++) {
          SurfacePoint pt = int_path[i][j];
          if (pt.type == SurfacePointType::Vertex) {
            myfile << "v " << pt.vertex.getIndex() << "\n";
          } else if (pt.type == SurfacePointType::Edge) {
            myfile << "e " << pt.edge.getIndex() << " " << pt.tEdge << "\n";
          } else if (pt.type == SurfacePointType::Face) {
            myfile << "f " << pt.face.getIndex() << " " << pt.faceCoords[0] << " " << pt.faceCoords[1] << " "
                   << pt.faceCoords[2] << "\n";
          }
          // This code doesn't check if the curve is a loop; if so, delete the last SurfacePoint (because it
          // will be a repeat of the 1st) and change the 2nd endpoint of the last line segment to the 0th
          // index.
          if (j < int_path[i].size() - 1) {
            myfile << "l " << idx + OFFSET_IDX << " " << idx + 1 + OFFSET_IDX << "\n";
          }
          idx++;
        }
      }
    } else {
      int idx = 0;
      assert(int_path.size() == 1);
      for (size_t i = 0; i < int_path.size(); i++) {
        myfile << "n\n";
        size_t N = int_path[i].size();
        for (size_t j = 0; j < N; j++) {
          SurfacePoint pt = int_path[i][j % (N - 1)];
          if (j % (N - 1) == j) {
            if (pt.type == SurfacePointType::Vertex) {
              myfile << "v " << pt.vertex.getIndex() << "\n";
            } else if (pt.type == SurfacePointType::Edge) {
              myfile << "e " << pt.edge.getIndex() << " " << pt.tEdge << "\n";
            } else if (pt.type == SurfacePointType::Face) {
              myfile << "f " << pt.face.getIndex() << " " << pt.faceCoords[0] << " " << pt.faceCoords[1] << " "
                     << pt.faceCoords[2] << "\n";
            }
          }
          if (j < int_path[i].size() - 1) {
            myfile << "l " << j + OFFSET_IDX << " " << (j + 1) % (N - 1) + OFFSET_IDX << "\n";
          }
        }
      }
      myfile.close();
    }
  } else {
    // Output curve as a sequence of (unique) surface points
    std::ofstream myfile("../../contact-parameterization/data/curve.txt");
    std::vector<std::vector<SurfacePoint>> int_path = edgeNetwork->getPathPolyline();

    for (size_t i = 0; i < int_path.size(); i++) {
      myfile << "n\n";
      size_t N = int_path[i].size();
      if (fancyPathClosed) N -= 1;
      for (size_t j = 0; j < N; j++) {
        SurfacePoint pt = int_path[i][j];
        if (pt.type == SurfacePointType::Vertex) {
          myfile << "v " << pt.vertex.getIndex() << "\n";
        } else if (pt.type == SurfacePointType::Edge) {
          myfile << "e " << pt.edge.getIndex() << " " << pt.tEdge << "\n";
        } else if (pt.type == SurfacePointType::Face) {
          myfile << "f " << pt.face.getIndex() << " " << pt.faceCoords[0] << " " << pt.faceCoords[1] << " "
                 << pt.faceCoords[2] << "\n";
        }
      }
    }
    myfile.close();
  }
}

void bezierSubdivide() {
  if (edgeNetwork == nullptr) {
    polyscope::warning("no path network");
    return;
  }

  START_TIMING(bezier)
  edgeNetwork->bezierSubdivide(nBezierIters);
  edgeNetwork->getPathPolyline();
  FINISH_TIMING_PRINT(bezier)

  updatePathViz();
}

void delaunayRefine() {

  if (edgeNetwork == nullptr) {
    polyscope::warning("no path network");
    return;
  }

  edgeNetwork->delaunayRefine(refineAreaThresh, maxInsertions == -1 ? INVALID_IND : maxInsertions, refineAngleThresh);
  std::cout << "refined mesh has " << edgeNetwork->tri->intrinsicMesh->nVertices() << " verts\n";

  updatePathViz();
}

// ====== General viz

void clearData() {
  psMesh->removeAllQuantities();
  edgeNetwork.reset();
}

void exportPathLines() {
  if (edgeNetwork == nullptr) {
    polyscope::warning("no path network");
    return;
  }

  edgeNetwork->savePathOBJLine("");
}


void buildFancyPathUI() {

  auto updateFancyPathViz = [&]() { psMesh->addVertexCountQuantity("fancy path vertices", fancyPathVertsPs); };

  if (ImGui::Button("Push Vertex")) {

    long long int iV = psMesh->selectVertex();
    if (iV != -1) {
      Vertex v = mesh->vertex(iV);
      fancyPathVerts.push_back(v);
      fancyPathVertsPs.emplace_back((size_t)iV, (int)fancyPathVertsPs.size());
      updateFancyPathViz();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Pop Vertex")) {
    if (!fancyPathVerts.empty()) {
      fancyPathVerts.pop_back();
      fancyPathVertsPs.pop_back();
    }
    updateFancyPathViz();
  }

  if (ImGui::Button("New Path From These Points")) {
    edgeNetwork = FlipEdgeNetwork::constructFromPiecewiseDijkstraPath(*mesh, *geometry, fancyPathVerts, fancyPathClosed,
                                                                      fancyPathMarkVerts);
    if (edgeNetwork == nullptr) {
      polyscope::warning("could not initialize fancy edge path between vertices");
      return;
    }
    edgeNetwork->posGeom = geometry.get();

    updatePathViz();
  }
  ImGui::Checkbox("Create Closed Path", &fancyPathClosed);
  ImGui::Checkbox("Mark interior vertices", &fancyPathMarkVerts);
}

// A user-defined callback, for creating control panels (etc)
void myCallback() {

  // my code
  if (ImGui::Button("Export OBJ")) {

    // Center
    Vector3 c = {0, 0, 0};
    for (Vertex v : mesh->vertices()) {
      c += geometry->inputVertexPositions[v];
    }
    c /= mesh->nVertices();

    // Unit scale
    double scale = 0.0;
    for (Vertex v : mesh->vertices()) {
      geometry->inputVertexPositions[v] -= c;
      scale = std::max(scale, geometry->inputVertexPositions[v].norm());
    }

    for (Vertex v : mesh->vertices()) {
      geometry->inputVertexPositions[v] /= scale;
    }

    writeSurfaceMesh(*mesh, *geometry, "my_mesh.obj");
  }

  ImGui::TextUnformatted("Input");

  if (ImGui::Button("Construct new Dijkstra path from endpoints")) {
    clearData();
    createPathFromPoints();
  }

  if (ImGui::TreeNode("Construct fancy path")) {
    buildFancyPathUI();
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Specialty loaders")) {
    if (ImGui::Button("Load edge set")) {
      clearData();
      createPathFromEdgeSet();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load line list obj")) {
      clearData();
      createPathFromObjLines();
    }
    if (ImGui::Button("Load Dijkstra list")) {
      clearData();
      createPathFromDijkstraList();
    }

    if (ImGui::Button("Load UV cut")) {
      clearData();
      createPathFromUVCut();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load seg cut")) {
      clearData();
      createPathFromSeg();
    }
    ImGui::TreePop();
  }

  ImGui::PushItemWidth(150);

  ImGui::Separator();
  ImGui::TextUnformatted("Algorithm");

  // Straightening
  if (ImGui::Button("Make geodesic")) {
    locallyShorten();
  }
  ImGui::SameLine();
  if (ImGui::Button("Check Path")) {
    checkPath();
  }

  ImGui::Checkbox("Straighten at marked", &straightenAtMarked);
  ImGui::Checkbox("Limit Iteration Count", &iterativeShortenUseIterationCap);
  if (iterativeShortenUseIterationCap) {
    ImGui::SameLine();
    ImGui::InputInt("Iters: ", &iterativeShortenIterationCap);
  }

  ImGui::Checkbox("Limit Length Decrease", &useIterativeShortenLengthLim);
  if (useIterativeShortenLengthLim) {
    ImGui::SameLine();
    ImGui::InputFloat("Relative lim: ", &iterativeShortenLengthLim);
  }

  // ====  Extras
  if (ImGui::TreeNode("Extras")) {

    if (ImGui::TreeNode("Bezier curves")) {

      if (ImGui::Button("Bezier subdivide")) {
        bezierSubdivide();
      }
      ImGui::InputInt("Bezier rounds", &nBezierIters);

      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Intrinsic mesh improvement")) {
      if (ImGui::Button("Flip to Delaunay")) {
        makeDelaunay();
      }
      ImGui::InputInt("Max insert", &maxInsertions);
      ImGui::InputFloat("Area thresh", &refineAreaThresh);
      ImGui::InputFloat("Angle thresh", &refineAngleThresh);
      if (ImGui::Button("Delaunay refine")) {
        delaunayRefine();
      }
      ImGui::TreePop();
    }

    ImGui::TreePop();
  }

  // ==== Visualization
  ImGui::Separator();
  ImGui::TextUnformatted("Visualization & Export");

  if (ImGui::Checkbox("Show Intrinsic Edges", &vizAllIntrinsicEdges)) {
    if (edgeNetwork) {
      updatePathViz();
    }
  }

  if (ImGui::Button("Export path lines")) {
    if (edgeNetwork) {
      exportPathLines();
    } else {
      polyscope::warning("no path registered");
    }
  }

  ImGui::PopItemWidth();

  ImGui::InputInt("Offset idx", &OFFSET_IDX);
  ImGui::Checkbox("Save as sequence of nodes", &SEQUENCE_OF_NODES);
}

int main(int argc, char** argv) {

  // Configure the argument parser
  args::ArgumentParser parser("Flip edges to find geodesic paths.");
  args::Positional<std::string> inputFilename(parser, "mesh", "A mesh file.");
  // optionally display an existing curve
  args::Positional<std::string> curveFilename(parser, "curve", "A curve file.");

  args::Group output(parser, "ouput");
  // args::Flag noGUI(output, "noGUI", "exit after processing and do not open the GUI", {"noGUI"});

  // Parse args
  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help&) {
    std::cout << parser;
    return 0;
  } catch (args::ParseError& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  }

  // Make sure a mesh name was given
  if (!inputFilename) {
    std::cerr << "Please specify a mesh file as argument" << std::endl;
    return EXIT_FAILURE;
  }

  // Set options
  // withGUI = !noGUI;
  withGUI = true;

  // Initialize polyscope
  if (withGUI) {
    polyscope::init();
    polyscope::state::userCallback = myCallback;
  }

  // Load mesh
  loadedFilename = args::get(inputFilename);
  std::tie(mesh, geometry) = readManifoldSurfaceMesh(loadedFilename);

  if (withGUI) {
    // Register the mesh with polyscope
    psMesh = polyscope::registerSurfaceMesh("input mesh", geometry->inputVertexPositions, mesh->getFaceVertexList(),
                                            polyscopePermutations(*mesh));

    if (curveFilename) {
      DISPLAY_CURVE = true;
      std::string curveFilepath = args::get(curveFilename);
      readInputCurves(*mesh, curveFilepath, curveNodes, curveEdges);
      displayCurves(*geometry, psMesh, curveNodes, curveEdges, "input curves");
    }
  }

  // Perform any operations requested via command line arguments

  // Give control to the gui
  if (withGUI) {
    // Give control to the polyscope gui
    polyscope::show();
  }

  return EXIT_SUCCESS;
}
