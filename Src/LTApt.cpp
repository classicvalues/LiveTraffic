/// @file       LTApt.cpp
/// @brief      Access to X-Plane's `apt.dat` file(s) and data
/// @details    Scans `apt.dat` file for airport, runway, and taxiway information.\n
///             Finds potential runway for an auto-land flight.\n
///             Finds center lines on runways and taxiways to snap positions to.
/// @author     Birger Hoppe
/// @copyright  (c) 2020 Birger Hoppe
/// @copyright  Permission is hereby granted, free of charge, to any person obtaining a
///             copy of this software and associated documentation files (the "Software"),
///             to deal in the Software without restriction, including without limitation
///             the rights to use, copy, modify, merge, publish, distribute, sublicense,
///             and/or sell copies of the Software, and to permit persons to whom the
///             Software is furnished to do so, subject to the following conditions:\n
///             The above copyright notice and this permission notice shall be included in
///             all copies or substantial portions of the Software.\n
///             THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
///             IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
///             FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
///             AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
///             LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
///             OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
///             THE SOFTWARE.

#include "LiveTraffic.h"

// File paths

/// Path to the `scenery_packs.ini` file, which defines order and activation status of scenery packs
#define APTDAT_SCENERY_PACKS "Custom Scenery/scenery_packs.ini"
/// How a line in `scenery_packs.ini` file needs to start in order to be processed by us
#define APTDAT_SCENERY_LN_BEGIN "SCENERY_PACK "
/// Path to add after the scenery pack location read from the ini file
#define APTDAT_SCENERY_ADD_LOC "Earth nav data/apt.dat"
/// Path to the global airports file under Resources / Default
#define APTDAT_RESOURCES_DEFAULT "Resources/default scenery/default apt dat/"

// Log output
#define WARN_APTDAT_NOT_OPEN "Can't open '%s': %s"
#define WARN_APTDAT_FAILED   "Could not open ANY apt.dat file. No runway/taxiway info available to guide ground traffic."
#define WARN_APTDAT_READ_FAIL "Could not completely read '%s'. Some runway/taxiway info will be missing to guide ground traffic: %s"

/// This flag stops the file reading thread
volatile bool bStopThread = false;

class Apt;

//
// MARK: Airports, Runways and Taxiways
//

/// Vector of indexes into another vector (e.g. indexes into the vector of edges, sorted by angle)
typedef std::vector<size_t> vecIdxTy;

/// @brief A node of a taxi way
/// @details Depending on scenery and search range we might need to read and store
///          tenth of thousans of these, so we limit the members as much as possible,
///          e.g. we don't use ::positionTy but only lat/lon/x/z.
class TaxiNode {
public:
    double      lat;                    ///< latitude
    double      lon;                    ///< longitude
    vecIdxTy    vecEdges;               ///< vector of edges connecting to this node, stored as indexes into Apt::vecTaxiEdges
    // attributes needed by Dijkstra's shortest path algorithm
    double      pathLen;                ///< current best known path length to this node
    size_t      prevIdx;                ///< previous node on shortest path
    bool        bVisited;               ///< has node been fully analyzed
public:
    /// Default constructor leaves all empty
    TaxiNode () : lat(NAN), lon(NAN) {}
    /// Typical constructor requires a location
    TaxiNode (double _lat, double _lon) : lat(_lat), lon(_lon) {}
    /// Destructor does not actually do anything, but is recommended in a good virtual class definition
    virtual ~TaxiNode () {}
    
    /// Initialize Dijkstra attribues
    void InitDijkstraAttr ()
    { pathLen = HUGE_VAL; prevIdx = ULONG_MAX; bVisited = false; }

    /// Is node valid in terms of geographic coordinates?
    bool HasGeoCoords () const { return !std::isnan(lat) && !std::isnan(lon); }

    /// Compares to given lat/long
    bool CompEqualLatLon (double _lat, double _lon) const
    { return dequal(lat, _lat) && dequal(lon, _lon); }
    
    /// Comparison function for equality based on lat/lon
    static bool CompEqualLatLon (const TaxiNode& a, const TaxiNode& b)
    { return a.CompEqualLatLon(b.lat, b.lon); }

    /// Equality is based solely on geographic position
    bool operator == (const TaxiNode& o) const
    { return CompEqualLatLon(o.lat, o.lon); }
};

/// Vector of taxi nodes
typedef std::vector<TaxiNode> vecTaxiNodesTy;

/// A runway endpoint is a special node of which we need to know the altitude
class RwyEndPt : public TaxiNode {
public:
    std::string id;                     ///< rwy identifier, like "23" or "05R"
    double      alt_m = NAN;            ///< ground altitude in meter
    vecIdxTy    vecTaxiNodes;           ///< nodes of taxiways leaving this direction of the rwy

public:
    /// Default constructor leaves all empty
    RwyEndPt () : TaxiNode () {}
    /// Typical constructor fills id and location
    RwyEndPt (const std::string& _id, double _lat, double _lon) :
    TaxiNode(_lat, _lon), id(_id) {}
    /// Destructor does not actually do anything, but is recommended in a good virtual class definition
    virtual ~RwyEndPt () {}

    /// Compute altitude if not yet known
    void ComputeAlt (XPLMProbeRef& yProbe)
    {
        if (std::isnan(alt_m))
            alt_m = YProbe_at_m(positionTy(lat,lon,0.0), yProbe);
    }
};

/// Vector of runway endpoints
typedef std::vector<RwyEndPt> vecRwyEndPtTy;

/// @brief An edge in the taxi / rwy network, connected two nodes
/// @details TaxiEdge can only store _indexes_ into the vector of nodes,
///          which is Apt::vecTaxiNodes. tt cannot directly store pointers or references,
///          as the memory location might change when the vector reorganizes due to
///          additions.\n
///          This also means that some functions otherwise better suites here are now
///          moved to Apt as only Apt has access to all vectors.
class TaxiEdge {
public:
    /// Taxiway or runway?
    enum nodeTy {
        UNKNOWN_WAY = 0,                ///< edge is of undefined type
        RUN_WAY = 1,                    ///< edge is for runway
        TAXI_WAY,                       ///< edge is for taxiway
    };
    
protected:
    nodeTy type;                        ///< type of node (runway, taxiway)
    size_t      a = 0;                  ///< from node (index into vecTaxiNodes)
    size_t      b = 0;                  ///< to node (index into vecTaxiNodes)
public:
    double angle;                       /// angle/heading from a to b
    double dist_m;                      /// distance in meters between a and b
public:
    /// Constructor
    TaxiEdge (nodeTy _t, size_t _a, size_t _b, double _angle, double _dist_m) :
    type(_t), a(_a), b(_b), angle(_angle), dist_m(_dist_m)
    {
        Normalize();
    }
    
    /// Normalize myself to 0 <= angle < 180
    void Normalize ()
    {
        if (angle >= 180.0) {
            std::swap(a,b);
            angle -= 180.0;
        }
    }
    
    /// Return the node's type
    nodeTy GetType () const { return type; }
    
    /// Equality is based on type and nodes
    bool operator == (const TaxiEdge& o) const
    { return type == o.type && a == o.a && b == o.b; }
    
    // Poor man's polymorphism: rwy endpoints are stored at a different place
    // than taxiway nodes. And we only store indexes as pointers are
    // unreliabe. The following functions return the proper object.
    /// Return the a node, ie. the starting point of the edge
    const TaxiNode& GetA (const Apt& apt) const;
    /// Return the b node, ie. the ending point of the edge
    const TaxiNode& GetB (const Apt& apt) const;
    /// Return the first runway endpoint of a runway
    const RwyEndPt& GetRwyEP_A (const Apt& apt) const
    { return dynamic_cast<const RwyEndPt&>(GetA(apt)); }
    /// Return the second runway endpoint of a runway
    const RwyEndPt& GetRwyEP_B (const Apt& apt) const
    { return dynamic_cast<const RwyEndPt&>(GetB(apt)); }
    
    /// Return the angle, adjust in a way that it points away from node n (which must be either TaxiEdge::a or TaxiEdge::b)
    double GetAngleFrom (size_t n) const { return n == a ? angle : angle + 180.0; }
    /// Returns the edge's angle, which is closest to the given heading
    double GetAngleByHead (double heading) const
    { return std::abs(HeadingDiff(heading, angle)) < 90.0 ? angle : angle + 180.0; }
    /// Return the taxi node, that is the "start" when heading in the given direction
    const TaxiNode& startByHeading (const Apt& apt, double heading) const
    { return std::abs(HeadingDiff(heading, angle)) < 90.0 ? GetA(apt) : GetB(apt); }
    /// Return the taxi node, that is the "end" when heading in the given direction
    const TaxiNode& endByHeading (const Apt& apt, double heading) const
    { return std::abs(HeadingDiff(heading, angle)) < 90.0 ? GetB(apt) : GetA(apt); }

    size_t startNode() const { return a; }      ///< index of start node
    size_t endNode()   const { return b; }      ///< index of end node
    size_t startByHeading (double heading) const  ///< Return the index of that node that is the edge's start if if looking in given direction
    { return std::abs(HeadingDiff(heading, angle)) < 90.0 ? a : b; }
    size_t endByHeading (double heading) const  ///< Return the index of that node that is the edge's end if if looking in given direction
    { return std::abs(HeadingDiff(heading, angle)) < 90.0 ? b : a; }
    
    size_t otherNode(size_t n) const { return n == a ? b : a; } ///< returns the "other" node (`n` should be TaxiEdge::a or TaxiEdge::b)

    /// sets a new end node, usually when splitting edges
    void SetEndNode (size_t _b, double _angle, double _dist_m)
    {
        b = _b;
        angle = _angle;
        dist_m = _dist_m;
        Normalize();
    }
};

/// Vector of taxi edges
typedef std::vector<TaxiEdge> vecTaxiEdgeTy;

/// Represents an airport as read from apt.dat
class Apt {
protected:
    static XPLMProbeRef YProbe;         ///< Y Probe for terrain altitude computation
    std::string id;                     ///< ICAO code or other unique id
    boundingBoxTy bounds;               ///< bounding box around airport, calculated from rwy and taxiway extensions
    double alt_m = NAN;                 ///< the airport's altitude
    vecTaxiNodesTy vecTaxiNodes;        ///< vector of taxi network nodes
    vecRwyEndPtTy  vecRwyEndPts;        ///< vector of runway endpoints
    vecTaxiEdgeTy  vecTaxiEdges;        ///< vector of taxi network edges, each connecting any two nodes
    vecIdxTy       vecTaxiEdgesIdxHead; ///< vector of indexes into Apt::vecTaxiEdges, sorted by TaxiEdge::angle

public:
    /// Constructor expects an id
    Apt (const std::string& _id = "") : id(_id) {}
    
    /// Id of the airport, typicall the ICAO code
    std::string GetId () const { return id; }
    /// Is any id defined? (Used as indicator while reading in `apt.dat`)
    bool HasId () const { return !id.empty(); }
    
    /// Valid airport definition requires an id and some taxiways / runways
    bool IsValid () const { return HasId() && HasTaxiWays() && HasRwyEndpoints(); }
    
    /// Return a reasonable altitude...effectively one of the rwy ends' altitude
    double GetAlt_m () const { return alt_m; }
    
    // --- MARK: Taxiways
    
    /// The vector of taxi network nodes
    const vecTaxiNodesTy& GetTaxiNodesVec () const { return vecTaxiNodes; }
    /// The list of taxi network edges
    const vecTaxiEdgeTy& GetTaxiEdgeVec () const { return vecTaxiEdges; }
    
    /// Any taxiways/runways defined?
    bool HasTaxiWays () const { return !vecTaxiEdges.empty(); }
    
    /// return taxi node with "close-by" location
    vecTaxiNodesTy::const_iterator GetSimilarTaxiNode (double lat, double lon, size_t dontCombineWith) const
    {
        constexpr double latDiff = Dist2Lat(APT_MAX_SIMILAR_NODE_DIST_M);
        const double lonDiff = Dist2Lon(APT_MAX_SIMILAR_NODE_DIST_M, lat);
        const TaxiNode* pNotThis = (dontCombineWith == ULONG_MAX ? nullptr :
                                    &vecTaxiNodes.at(dontCombineWith));
        return std::find_if(vecTaxiNodes.cbegin(),
                            vecTaxiNodes.cend(),
                            [=](const TaxiNode& n)
                            { return ((!pNotThis || pNotThis != &n) &&
                                      std::abs(n.lat - lat) <= latDiff &&
                                      std::abs(n.lon - lon) <= lonDiff); });
    }
    
    /// @brief Add a new taxi network node
    /// @return Index of node in Apt::vecTaxiNodes
    size_t AddTaxiNode (double lat, double lon, size_t dontCombineWith = ULONG_MAX)
    {
        // Is there a similar close-by node already?
        vecTaxiNodesTy::const_iterator cbIter = GetSimilarTaxiNode(lat, lon, dontCombineWith);
        if (cbIter != vecTaxiNodes.cend())
            return std::distance(vecTaxiNodes.cbegin(), cbIter);
        
        bounds.enlarge_pos(lat, lon);           // Potentially expands the airport's boundary
        vecTaxiNodes.emplace_back(lat, lon);    // Add the node to the back of the list
        return vecTaxiNodes.size()-1;           // return the index
    }
    
    /// @brief Add a new taxi network node at a given index position
    void AddTaxiNodeFixed (double lat, double lon, size_t idx)
    {
        // Potentially expands the airport's boundary
        bounds.enlarge_pos(lat, lon);

        // Expected case: Just the next index
        if (idx == vecTaxiNodes.size())
            vecTaxiNodes.emplace_back(lat, lon);
        else {
            // make sure the vector is large enough
            if (idx > vecTaxiNodes.size())
                vecTaxiNodes.resize(idx+1);
            // then assign the value
            vecTaxiNodes[idx] = TaxiNode(lat,lon);
        }
    }
    
    /// @brief Add a new taxi network edge, which must connect 2 existing nodes
    /// @return Index into Apt::vecTaxiEdges, or ULONG_MAX if unsuccessful
    size_t AddTaxiEdge (size_t n1, size_t n2, double _dist = NAN)
    {
        // Actual nodes must be valid, throws exception if not
        TaxiNode& a = vecTaxiNodes.at(n1);
        TaxiNode& b = vecTaxiNodes.at(n2);
        if (!a.HasGeoCoords() || !b.HasGeoCoords())
        {
            LOG_MSG(logDEBUG, "apt.dat: Node %lu or &lu invalid! Edge not added.", n1, n2);
            return ULONG_MAX;
        }
        
        // Add the edge
        if (std::isnan(_dist))
            _dist = DistLatLon(a.lat, a.lon, b.lat, b.lon);
        vecTaxiEdges.emplace_back(TaxiEdge::TAXI_WAY, n1, n2,
                                  CoordAngle(a.lat, a.lon, b.lat, b.lon),
                                  _dist);
        
        // Tell the nodes they've got a new connection
        const size_t eIdx = vecTaxiEdges.size()-1;
        a.vecEdges.push_back(eIdx);
        b.vecEdges.push_back(eIdx);
        
        return vecTaxiEdges.size()-1;
    }
    
    /// Recalc heading and angle of a given edge
    void RecalcTaxiEdge (TaxiEdge& e)
    {
        const TaxiNode& a = e.GetA(*this);
        const TaxiNode& b = e.GetB(*this);
        e.angle  = CoordAngle(a.lat, a.lon, b.lat, b.lon);
        e.dist_m = DistLatLon(a.lat, a.lon, b.lat, b.lon);
        
        // Did this change the orientation? Then we need to swap a<->b
        e.Normalize();
    }
    
    /// Split an edge by inserting a given node
    void SplitEdge (size_t eIdx, size_t insNode)
    {
        // 1. Remember the original target edge
        TaxiEdge& e = vecTaxiEdges.at(eIdx);
        if (insNode == e.startNode() || insNode == e.endNode())
            return;
        size_t joinOrigB = e.endNode();
        TaxiNode& origB = vecTaxiNodes[joinOrigB];
        
        // 2. Short-cut existing node at new joint
        const TaxiNode& a = e.GetA(*this);
        TaxiNode& b = vecTaxiNodes.at(insNode);
        e.SetEndNode(insNode,
                     CoordAngle(a.lat, a.lon, b.lat, b.lon),
                     DistLatLon(a.lat, a.lon, b.lat, b.lon));
        // Node insNode/b now got one more edge connection, origB currently one less
        b.vecEdges.push_back(eIdx);
        for (auto iter = origB.vecEdges.begin();    // std::remove(_if) should have done the job...but it simply didn't
             iter != origB.vecEdges.end();)
        {
            if (*iter == eIdx)
                iter = origB.vecEdges.erase(iter);
            else
                ++iter;
        }
        
        // 3. Add new edge between insNode and joinOrigB
        AddTaxiEdge(insNode, joinOrigB);
    }
    
    /// @brief Fill the indirect vector, which sorts edges by heading
    void SortTaxiEdges ()
    {
        // If the indirect array doesn't seem to have correct size
        // then we need to create that first
        if (vecTaxiEdges.size() != vecTaxiEdgesIdxHead.size()) {
            vecTaxiEdgesIdxHead.resize(vecTaxiEdges.size());
            for (size_t i = 0; i < vecTaxiEdgesIdxHead.size(); ++i)
                vecTaxiEdgesIdxHead[i] = i;
        }
        
        // Now sort the index array by the angle of the linked edge
        std::sort(vecTaxiEdgesIdxHead.begin(),
                  vecTaxiEdgesIdxHead.end(),
                  [&](size_t a, size_t b)
                  { return vecTaxiEdges[a].angle < vecTaxiEdges[b].angle; });
    }
    
    /// @brief Returns a list of taxiways matching a given heading range
    /// @param _headSearch The heading we search for and which the edge has to match
    /// @param _angleTolerance Maximum difference between `_headSearch` and TaxiEdge::angle to be considered a match
    /// @param[out] lst Matching egdes are added to this list,
    /// @param _restrictType (Optional) Restrict returned edges to this type, or `UNKNOWN_WAY` to not restrict results
    /// @return Anything found? Basically: `!vec.empty()`
    bool FindEdgesForHeading (double _headSearch,
                              double _angleTolerance,
                              vecIdxTy& lst,
                              TaxiEdge::nodeTy _restrictType = TaxiEdge::UNKNOWN_WAY) const
    {
        // vecTaxiEdges is sorted by heading (see AddApt)
        // and TaxiEdge::heading is normalized to [0..180).
        // So we can more quickly find potential matches by
        // looking in that range of edges only around our target heading pos.heading()
        // "Normalize" search heading even further to [0..180)
        bool bHeadInverted = false;
        if (_headSearch >= 180.0) {
            _headSearch -= 180.0;
            bHeadInverted = true;
        }
        // We allow for some tolerance
        const double headBegin = _headSearch - _angleTolerance;     // might now be < 0 !
        const double headEnd   = _headSearch + _angleTolerance;     // might now be >= 180 !
        
        // We need one or two search ranges
        std::vector< std::pair<double,double> > vecRanges;
        // normal case: just one search range
        if (0.0 <= headBegin && headEnd < 180.0) {
            vecRanges.emplace_back(headBegin,   headEnd);
        } else if (headBegin < 0.0) {
            const double headBeginInv = headBegin + 180.0;              // inverse...if headBegin < 0 then this is the start point in the upper range close to 180°
            vecRanges.emplace_back(0.0,         headEnd);
            vecRanges.emplace_back(headBeginInv,180.0);
        } else {        // headEnd >= 180.0
            const double headEndInv   = headEnd   - 180.0;              // inverse...if headEnd >= 180 then this is the end point in the lower range close to 0°
            vecRanges.emplace_back(0.0,         headEndInv);
            vecRanges.emplace_back(headBegin,   180.0);
        }
        
        // search all (up to 2) heading ranges now
        for (const std::pair<double,double>& rngPair: vecRanges)
        {
            // within that heading range, add all matching edges
            for (vecIdxTy::const_iterator iter =
                 std::lower_bound(vecTaxiEdgesIdxHead.cbegin(),
                                  vecTaxiEdgesIdxHead.cend(),
                                  rngPair.first,
                                  [&](const size_t& idx, double _angle)
                                  { return vecTaxiEdges[idx].angle < _angle; });
                 iter != vecTaxiEdgesIdxHead.cend() && vecTaxiEdges[*iter].angle <= rngPair.second;
                 ++iter)
            {
                // Check for type limitation, then add to `vec`
                const TaxiEdge& e = vecTaxiEdges[*iter];
                if (_restrictType == TaxiEdge::UNKNOWN_WAY ||
                    _restrictType == e.GetType())
                    lst.push_back(*iter);
            }
        }
        
        // Found anything?
        return !lst.empty();
    }
    
    
    /// @brief Find closest taxi edge matching the passed position including its heading
    /// @details Calculations are done based on approximate  distances between
    ///          geographic world coordinates, measured in meter.
    ///          The passed-in position is considered the (0|0) point,
    ///          while the nodes to be analyzed are converted to distances to this point
    ///          before passed on to the DistPointToLineSqr() function.
    ///          The resulting base point is then converted back to geo world coords.
    /// @param pos Search position, only nearby nodes with a similar heading are considered
    /// @param[out] basePt Receives the coordinates of the base point in case of a match. Only positionTy::lat, positionTy::lon, and positionTy::edgeIdx will be modified.
    /// @param _maxDist_m Maximum distance in meters between `pos` and edge to be considered a match
    /// @param _angleTolerance Maximum difference between `pos.heading()` and TaxiEdge::angle to be considered a match
    /// @param _angleToleranceExt Second priority tolerance, considered only if such a node is more than 5m closer than one that better fits angle
    /// @param pSkipEdge (optional) Do not return this edge
    /// @param pEdgeIdx (optional) receives index of found edge
    /// @return Pointer to closest taxiway edge or `nullptr` if no match was found
    const TaxiEdge* FindClosestEdge (const positionTy& pos,
                                     positionTy& basePt,
                                     int _maxDist_m,
                                     double _angleTolerance,
                                     double _angleToleranceExt,
                                     const TaxiEdge* pSkipEdge = nullptr,
                                     size_t* pEdgeIdx = nullptr) const
    {
        const TaxiEdge* bestEdge = nullptr;
        size_t bestEdgeIdx = EDGE_UNKNOWN;
        double best_from_x = NAN;
        double best_from_y = NAN;
        double best_to_x   = NAN;
        double best_to_y   = NAN;
        double bestPrioDist= NAN;
        distToLineTy bestDist;
        // maxDist^2, used in comparisons
        const double maxDist2 = (double)sqr(_maxDist_m);
        // This is what we add to the square distance for second prio match...
        // ...it is not exactly (dist+5m)^2 = dist^2 + 2 * 5 * dist + 5 ^ 2
        // ...but as close as we can get when we want to avoid sqrt for performance reasons
        constexpr double SCND_PRIO_ADD = 3 * ART_EDGE_ANGLE_EXT_DIST + ART_EDGE_ANGLE_EXT_DIST*ART_EDGE_ANGLE_EXT_DIST;
        
        // Get a list of edges matching pos.heading()
        vecIdxTy lstEdges;
        const double headSearch = HeadingNormalize(pos.heading());
        if (!FindEdgesForHeading(headSearch,
                                 std::max(_angleTolerance, _angleToleranceExt),
                                 lstEdges))
            return nullptr;
        
        // Analyze the edges to find the closest edge
        for (size_t eIdx: lstEdges)
        {
            // Skip edge if wanted so
            const TaxiEdge& e = vecTaxiEdges[eIdx];
            if (pSkipEdge == &e) continue;
            
            // Fetch from/to nodes from the edge
            const TaxiNode& from  = e.startByHeading(*this, headSearch);
            const TaxiNode& to    = e.endByHeading(*this, headSearch);
            const double edgeAngle = e.GetAngleByHead(headSearch);

            // Compute temporary "coordinates", relative to the search position
            const double from_x = Lon2Dist(from.lon - pos.lon(), pos.lat());      // x is eastward
            const double from_y = Lat2Dist(from.lat - pos.lat());                 // y is northward
            const double to_x   = Lon2Dist(to.lon   - pos.lon(), pos.lat());
            const double to_y   = Lat2Dist(to.lat   - pos.lat());

            // Distance to this edge
            distToLineTy dist;
            DistPointToLineSqr(0.0, 0.0,            // plane's position is now by definition in (0|0)
                               from_x, from_y,      // edge's starting point
                               to_x, to_y,          // edge's end point
                               dist);
            
            // If too far away, skip
            if (dist.dist2 > maxDist2)
                continue;
            
            // Distinguish between first prio angle match and second prio angle match
            double prioDist = dist.dist2;
            if (std::abs(HeadingDiff(edgeAngle, headSearch)) > _angleTolerance)
                prioDist += SCND_PRIO_ADD;
            
            // If priorized distance is farther than best we know: skip
            if (prioDist >= bestPrioDist)
                continue;
            
            // If base of shortest path to point is too far outside actual line
            if (dist.DistSqrOfBaseBeyondLine() > maxDist2)
                continue;
            
            // We have a new best match!
            bestEdge = &e;
            bestEdgeIdx  = eIdx;
            best_from_x  = from_x;
            best_from_y  = from_y;
            best_to_x    = to_x;
            best_to_y    = to_y;
            bestPrioDist = prioDist;
            bestDist     = dist;
        }
        
        // Nothing found?
        if (!bestEdge)
            return nullptr;
        
        // Compute base point on the line,
        // ie. the point on the line with shortest distance
        // to pos
        double base_x = NAN, base_y = NAN;
        DistResultToBaseLoc(best_from_x, best_from_y,   // edge's starting point
                            best_to_x, best_to_y,       // edge's end point
                            bestDist,
                            base_x, base_y);            // base point's local coordinates

        // Now only convert back from our local pos-based coordinate system
        // to geographic world coordinates
        basePt.lon() = pos.lon() + Dist2Lon(base_x, pos.lat());
        basePt.lat() = pos.lat() + Dist2Lat(base_y);
        basePt.edgeIdx = bestEdgeIdx;
        
        // return the found egde
        if (pEdgeIdx)
            *pEdgeIdx = bestEdgeIdx;
        return bestEdge;
    }
    
    /// @brief Find open ends in taxiway network (nodes with just one connection) and try connecting them to some edge
    /// @details This shall\n
    ///          a) connect runways to taxiways
    ///          b) taxiway joints (which don't happen to have a directly overlapping node)
    void JoinOpenTaxiEdges ()
    {
        // Loop the nodes and find nodes which have just one edge
        for (size_t i=0; i < vecTaxiNodes.size(); ++i)
        {
            // skip if there is 0 or 2+ connections
            // (0 shouldn't actually happen...but we can't heal that here either)
            TaxiNode& n = vecTaxiNodes[i];
            if (n.vecEdges.size() != 1)
                continue;
            
            // The edge we deal with - skip if runway
            TaxiEdge& e = vecTaxiEdges[n.vecEdges.front()];
            if (e.GetType() == TaxiEdge::RUN_WAY)
                continue;
            
            // The angle of the taxi way segment (looking away from the single-ended node)
            const double taxiAngle = e.GetAngleFrom(i);

            // Try finding _another_ edge this one can connect to
            size_t joinIdx = EDGE_UNKNOWN;
            positionTy pos(n.lat, n.lon, 0.0, NAN, e.angle);
            const TaxiEdge* pJoinE = FindClosestEdge(pos, pos,
                                                     APT_JOIN_MAX_DIST_M,
                                                     APT_JOIN_ANGLE_TOLERANCE,
                                                     APT_JOIN_ANGLE_TOLERANCE_EXT,
                                                     &e,
                                                     &joinIdx);
            if (!pJoinE)
                continue;
            
            // Found an edge. Or a runwway, that is, which we handle differently
            if (pJoinE->GetType() == TaxiEdge::RUN_WAY) {
                // We just add this open node to the beginning edge
                // of the runway. So...which end then?
                RwyEndPt& rwyStart = dynamic_cast<RwyEndPt&>(const_cast<TaxiNode&>(pJoinE->startByHeading(*this, taxiAngle)));
                rwyStart.vecTaxiNodes.push_back(i);
            } else {
                // We found just another taxi edge, which we combine:
                // We'll now split that edge by inserting the
                // open node, which we move to the base position,
                // so that it is exactly on the edge we split:
                
                // Move the open node to the base location, ie. to the closes
                // point on the pJoinE edge (which is at max APT_JOIN_MAX_DIST_M meters away)
                n.lat = pos.lat();
                n.lon = pos.lon();
                // This has slightly changed the edge e, recalc distance and angle
                RecalcTaxiEdge(e);
                
                // Split pJoinE at the base position, now n (whose index is i)
                SplitEdge(joinIdx, i);
                
                // To ensure FindClosestEdge works we need to sort
                SortTaxiEdges();
            }
        }
        
    }
    
    /// @brief Find shortest path in taxi network with a maximum length between 2 nodes
    /// @see https://en.wikipedia.org/wiki/Dijkstra's_algorithm
    /// @param _start Start node in either Apt::vecTaxiNodes or Apt::vecRwyEndPts
    /// @param bStartIsRwy Defines if _start denotes a standard taxiway node or a rwy endpoint
    /// @param _end End node in Apt::vecTaxiNodes (not a runway end!)
    /// @return List of node indexes _including_ `_end` and `_start` in _reverse_ order
    vecIdxTy ShortestPath (size_t _startN, bool bStartIsRwy, size_t _endN, double _maxLen)
    {
        // Sanity check: _start and _end should differ
        if (!bStartIsRwy && _startN == _endN)
            return vecIdxTy();
        
        // Initialize the Dijkstra values in the nodes array
        for (TaxiNode& n: vecTaxiNodes)
            n.InitDijkstraAttr();

        // This array stores nodes we need to visit
        // (have an initial distance, but aren't fully visited yet)
        vecIdxTy vecVisit;

        // The start place(s) is either the given taxiway node, or
        // all taxiway nodes connected to the given runway endpoint
        if (bStartIsRwy) {
            const RwyEndPt& startRwyEP = vecRwyEndPts.at(_startN);
            for (size_t n: startRwyEP.vecTaxiNodes) {
                vecTaxiNodes[n].pathLen = 0.0;
                vecTaxiNodes[n].prevIdx = ULONG_MAX-1;  // we use "ULONG_MAX-1" for saying "is a start node"
                vecVisit.push_back(n);
            }
        } else {
            // start point is a taxiway node
            vecTaxiNodes.at(_startN).pathLen = 0.0;
            vecTaxiNodes.at(_startN).prevIdx = ULONG_MAX-1;  // we use "ULONG_MAX-1" for saying "is a start node"
            vecVisit.push_back(_startN);
        }

        // outer loop controls currently visited node and checks if end already found
        TaxiNode& endN = vecTaxiNodes[_endN];
        while (!vecVisit.empty() && endN.prevIdx == ULONG_MAX)
        {
            // fetch node with shortest yet known distance
            // (this isn't awfully efficient, but keeping a separate map or prio-queue
            //  sorted while updating nodes in the next loop
            //  is not simple either. I expect vecVisit to stay short
            //  due to cut-off at _maxLen, so I've decided this way:)
            vecIdxTy::iterator shortestIter = vecVisit.begin();
            double shortestDist = vecTaxiNodes[*shortestIter].pathLen;
            for (vecIdxTy::iterator i = std::next(shortestIter);
                 i != vecVisit.end(); ++i)
            {
                if (vecTaxiNodes[*i].pathLen < shortestDist) {
                    shortestIter = i;
                    shortestDist = vecTaxiNodes[*i].pathLen;
                }
            }
            const size_t shortestNIdx  = *shortestIter;
            TaxiNode& shortestN = vecTaxiNodes[shortestNIdx];
            
            // This one is now already counted as "visited" so no more updates to its pathLen!
            shortestN.bVisited = true;
            vecVisit.erase(shortestIter);

            // Update all connected nodes with best possible distance
            for (size_t eIdx: shortestN.vecEdges)
            {
                const TaxiEdge& e = vecTaxiEdges[eIdx];
                size_t updNIdx    = e.otherNode(shortestNIdx);
                TaxiNode& updN    = vecTaxiNodes[updNIdx];
                
                // if aleady visited then no need to re-assess
                if (updN.bVisited)
                    continue;
                
                // Calculate the yet known best distance to this node
                const double lenToUpd = shortestDist + e.dist_m;
                if (lenToUpd > _maxLen ||               // too far out?
                    updN.pathLen <= lenToUpd)           // node has a faster path already
                    continue;

                // Update this node with new best values
                updN.pathLen = lenToUpd;        // best new known distance
                updN.prevIdx = shortestNIdx;     // predecessor to achieve that distance
                
                // Have we reached the wanted end node?
                if (updNIdx == _endN)
                    break;
                
                // this node is now ready to be visited
                push_back_unique(vecVisit, updNIdx);
            }
        }
        
        // Found nothing? -> return empty list
        if (endN.prevIdx == ULONG_MAX)
            return vecIdxTy();
        
        // put together the nodes between _start and _end in the right order
        vecVisit.clear();
        for (size_t nIdx = _endN;
             vecTaxiNodes.at(nIdx).prevIdx != ULONG_MAX-1;  // nIdx is not a start node
             nIdx = vecTaxiNodes[nIdx].prevIdx)             // move on to previous node on shortest path
        {
            LOG_ASSERT(nIdx < vecTaxiNodes.size());
            vecVisit.push_back(nIdx);
        }
        return vecVisit;
    }
    
    /// @brief Find best matching taxi edge based on passed-in position/heading info
    bool SnapToTaxiway (LTFlightData& fd, dequePositionTy::iterator& posIter)
    {
        // The position we consider and that we potentially change
        // by snapping to a taxiway
        positionTy& pos = *posIter;
        const double old_lat = pos.lat(), old_lon = pos.lon();
        
        // Find the closest edge and right away move pos there
        size_t eIdx = EDGE_UNKNOWN;
        const TaxiEdge* pEdge = FindClosestEdge(pos, pos,
                                                dataRefs.GetFdSnapTaxiDist_m(),
                                                ART_EDGE_ANGLE_TOLERANCE,
                                                ART_EDGE_ANGLE_TOLERANCE_EXT,
                                                nullptr,
                                                &eIdx);
        
        // Nothing found?
        if (!pEdge) {
            posIter->edgeIdx = EDGE_UNAVAIL;
            return false;
        }
        
        // found a match, say hurray
        if (dataRefs.GetDebugAcPos(fd.key())) {
            LOG_MSG(logDEBUG, "Snapped to taxiway from (%.5f, %.5f) to (%.5f, %.5f)",
                    old_lat, old_lon, pos.lat(), pos.lon());
        }
            
        // this is now an artificially moved position, don't touch any further
        // (we don't mark positions on a runway yet...would be take off or rollout to be distinguished)
        if (pEdge->GetType() != TaxiEdge::RUN_WAY)
            pos.flightPhase = LTAPIAircraft::FPH_TAXI;
        else
            // Edge actually is on a runway. A runway edge cannot serve
            // as a `end` position for path search, so we exit here
            return true;
        
        // --- Insert shortest path along taxiways ---
        
        // We either need an aircraft (with a current `to` position)
        // or a predecessor in the fd.posDeque to come up with a path
        if (!fd.hasAc() && posIter == fd.posDeque.begin())
            return true;
        
        // The previous pos before *posIter:
        // Either the predecessor in fd.posDeque, if it exists,
        // or the plane's `to` position
        const positionTy& prevPos = (posIter == fd.posDeque.begin() ?
                                     fd.pAc->GetToPos() : *(std::prev(posIter)));
        // That pos must be on an edge, too
        if (!prevPos.HasTaxiEdge())
            return true;

        // That previous edge isn't by chance the same we just now found? Then the shortest path is to go straight...
        if (eIdx == prevPos.edgeIdx)
            return true;
        
        // previous edge's relevant node (the end node of a taxi edge, but the start node of a rwy)
        const TaxiEdge& prevE = vecTaxiEdges[prevPos.edgeIdx];
        const size_t prevErelN =
        prevE.GetType() == TaxiEdge::RUN_WAY ?      // is a rwy?
        prevE.startByHeading(prevPos.heading()) :   // use its starting node
        prevE.endByHeading(prevPos.heading());      // otherwise use the edge's end node
        // current edge's start node
        const size_t currEstartN = pEdge->startByHeading(pos.heading());
        
        // for the maximum allowed path length let's consider taxiing speed:
        // We shouldn't need to go faster than 1.5 x model's taxi speed
        const LTAircraft::FlightModel& mdl = fd.pAc ? fd.pAc->mdl :
        LTAircraft::FlightModel::FindFlightModel(fd.statData.acTypeIcao);
        const double maxLen = (pos.ts() - prevPos.ts()) * mdl.MAX_TAXI_SPEED * 1.5;
        
        // let's try finding a shortest path
        vecIdxTy vecPath = ShortestPath(prevErelN,
                                        prevE.GetType() == TaxiEdge::RUN_WAY,
                                        currEstartN,
                                        maxLen);
        
        // Some path found?
        if (vecPath.size() >= 2)
        {
            // length of total path as returned (this excludes the distance from prevPos to start, and from end to pos)
            double pathLen = vecTaxiNodes[currEstartN].pathLen;
            double startTS = NAN;
            // Add the end leg, ie. from end of path to pos
            {
                const TaxiNode& n = vecTaxiNodes[vecPath.front()];  // end of path
                pathLen += DistLatLon(n.lat, n.lon, pos.lat(), pos.lon());
            }
            
            // Adjust the startTS (as prevPos is not equal to start of path,
            // we need time to travel that short distance)

            // In case we leave a rwy for a taxiway the first node in vecPath
            // is the first taxiway node, which is potentially way down the runway.
            // We need to allow for some time to reach the taxiway node
            // from the position on the rwy
            if (prevE.GetType() == TaxiEdge::RUN_WAY)
            {
                // Assuming taxiing works with taxiing speed, how long would we need?
                const double taxiTime = pathLen / mdl.MAX_TAXI_SPEED;
                startTS = pos.ts() - taxiTime;
                
                // If that startTS now is _before_ rwy position...
                if (startTS < prevPos.ts() + SIMILAR_TS_INTVL)
                    // NOK, we just assume something...we leave the rwy a few seconds after previous position:
                    startTS = prevPos.ts() + SIMILAR_TS_INTVL;
            } else {
                // Otherwise we are on a taxiway node
                const TaxiNode& n = vecTaxiNodes[vecPath.back()];       // start of path
                const double prevToStartDist = DistLatLon(prevPos.lat(), prevPos.lon(), n.lat, n.lon);
                const double speed = (prevToStartDist + pathLen) / (pos.ts() - prevPos.ts());
                // Allow for some time to go from prevPos to start of path:
                startTS = prevPos.ts() + prevToStartDist / speed;
            }
            
            // the time we have from start of the path to pos
            const double pathTime = pos.ts() - startTS;
            
            // path is returned in reverse order, so work on it reversely
            for (vecIdxTy::const_reverse_iterator iter = vecPath.crbegin();
                 iter != vecPath.crend();
                 ++iter)
            {
                // create a proper position and insert it into fd's posDeque
                const TaxiNode& n = vecTaxiNodes[*iter];
                positionTy insPos (n.lat, n.lon, NAN,   // lat, lon, altitude
                                   startTS + pathTime * n.pathLen / pathLen,
                                   NAN,                 // heading will be populated later
                                   0.0, 0.0,            // on the ground no pitch/roll
                                   positionTy::GND_ON,
                                   positionTy::UNIT_WORLD,
                                   positionTy::UNIT_DEG,
                                   LTAircraft::FPH_TAXI);
                insPos.edgeIdx = EDGE_UNAVAIL;          // don't want to call SnapToTaxiway for this new pos!
                
                // Insert before the position that was passed in
                posIter = fd.posDeque.insert(posIter, insPos);  // posIter now points to inserted element
                ++posIter;                                      // posIter points to originally passed in element again
            }
            
            if (dataRefs.GetDebugAcPos(fd.key())) {
                LOG_MSG(logDEBUG, "Inserted %lu taxiway nodes", vecPath.size());
            }
        }
        
        // snapping successful
        return true;
    }
    
    // --- MARK: Runways
    
    /// The vector of runway endpoints
    const vecRwyEndPtTy& GetRwyEndPtVec () const { return vecRwyEndPts; }
    
    /// Any runway endpoints defined?
    bool HasRwyEndpoints () const { return !vecRwyEndPts.empty(); }
    
    /// Adds both rwy ends from apt.dat information fields
    void AddRwyEnds (double lat1, double lon1, double displaced1, const std::string& id1,
                     double lat2, double lon2, double displaced2, const std::string& id2)
    {
        // Original position of outer end of runway
        positionTy re1 (lat1,lon1,NAN,NAN,NAN,NAN,NAN,positionTy::GND_ON);
        positionTy re2 (lat2,lon2,NAN,NAN,NAN,NAN,NAN,positionTy::GND_ON);
        vectorTy vecRwy = re1.between(re2);

        // move by displayed threshold
        // and then by another 10% of remaining length to determine actual touch-down point
        vecRwy.dist -= displaced1;
        vecRwy.dist -= displaced2;
        re1 += vectorTy (vecRwy.angle,   displaced1 + vecRwy.dist * ART_RWY_TD_POINT_F );
        re2 += vectorTy (vecRwy.angle, -(displaced2 + vecRwy.dist * ART_RWY_TD_POINT_F));
        // Also adapt out knowledge of rwy length: 80% if previous value are left
        vecRwy.dist *= (1 - 2 * ART_RWY_TD_POINT_F);
        
        // 1st rwy end
        bounds.enlarge(re1);
        vecRwyEndPts.emplace_back(id1, re1.lat(), re1.lon());
        
        // 2nd rwy end
        bounds.enlarge(re2);
        vecRwyEndPts.emplace_back(id2, re2.lat(), re2.lon());
        
        // The edge between them, making up the actual runway
        vecTaxiEdges.emplace_back(TaxiEdge::RUN_WAY,
                                  vecRwyEndPts.size()-2,    // index of rwyEp1
                                  vecRwyEndPts.size()-1,    // index of rwyEp2
                                  vecRwy.angle,
                                  vecRwy.dist);
    }

    /// @brief Update rwy ends and airport with proper altitude
    /// @note Must be called from XP's main thread, otherwise Y probes won't work
    void UpdateAltitudes ()
    {
        // Airport: Center of boundaries
        alt_m = YProbe_at_m(bounds.center(), YProbe);
        
        // rwy ends
        for (RwyEndPt& re: vecRwyEndPts)            // for all rwy endpoints
            re.ComputeAlt(YProbe);
    }
    
    /// Destroy the YProbe
    static void DestroyYProbe ()
    {
        if (YProbe) {
            XPLMDestroyProbe(YProbe);
            YProbe = NULL;
        }
    }

    /// Return iterator to first rwy, or `GetTaxiEdgeVec().cend()` if none found
    vecTaxiEdgeTy::const_iterator FirstRwy () const
    {
        return std::find_if(vecTaxiEdges.cbegin(), vecTaxiEdges.cend(),
                            [](const TaxiEdge& te){return te.GetType() == TaxiEdge::RUN_WAY;});
    }
    
    /// Return iterator to next rwy after `i`, or `GetTaxiEdgeVec().cend()` if none found
    vecTaxiEdgeTy::const_iterator NextRwy (vecTaxiEdgeTy::const_iterator i) const
    {
        return std::find_if(++i, vecTaxiEdges.cend(),
                            [](const TaxiEdge& te){return te.GetType() == TaxiEdge::RUN_WAY;});
    }
    
    /// Returns a human-readable string with all runways, mostly for logging purposes
    std::string GetRwysString () const
    {
        std::string s;
        // loop all runways
        for (vecTaxiEdgeTy::const_iterator i = FirstRwy();
             i != vecTaxiEdges.cend();
             i = NextRwy(i))
        {
            if (!s.empty()) s += " / ";     // divider between runways
            try {
                s += i->GetRwyEP_A(*this).id;   // add ids of runways
                s += '-';
                s += i->GetRwyEP_B(*this).id;
            } catch(...) {}                 // shouldn't happen...!
        }
        return s;
    }
    
    // --- MARK: Bounding box

    /// Returns the bounding box of the airport as defined by all runways and taxiways
    const boundingBoxTy& GetBounds () const { return bounds; }
    
    /// Does airport contain this point?
    bool Contains (const positionTy& pos) const { return bounds.contains(pos); }
    
    /// Enlarge the bounding box by a few meters
    void EnlargeBounds_m (double meter) { bounds.enlarge_m(meter); }
    
    // --- MARK: Static Functions
    
    /// @brief Add airport to list of airports
    static void AddApt (Apt&& apt);


};  // class Apt

// Y Probe for terrain altitude computation
XPLMProbeRef Apt::YProbe = NULL;

/// Map of airports, key is the id (typically: ICAO code)
typedef std::map<std::string, Apt> mapAptTy;

/// Global map of airports
static mapAptTy gmapApt;

/// Lock to access global map of airports
static std::mutex mtxGMapApt;

// Add airport to list of airports
/// @details It is actually expected that `apt` is not yet known and really added to the map,
///          that's why the fancy debug log message is formatted first.
///          In the end, map::emplace certainly makes sure and wouldn't actually add duplicates.
void Apt::AddApt (Apt&& apt)
{
    // At this stage the airport is defined.
    // We'll now add as much space to the bounding box as
    // defined for taxiway snapping, so that positions
    // slightly outside the airport are still considered for searching:
    apt.EnlargeBounds_m(double(dataRefs.GetFdSnapTaxiDist_m()));
    
    // Prepare the indirect array, which sorts by edge angle
    // for faster finding of edges by heading
    apt.SortTaxiEdges();
    
    // Now connect open ends, ie. try finding joints between a node and existing edges
    apt.JoinOpenTaxiEdges();

    // Fancy debug-level logging message, listing all runways
    LOG_MSG(logDEBUG, "apt.dat: Added %s at %s with %lu runways (%s) and [%lu|%lu] taxi nodes|edges",
            apt.GetId().c_str(),
            std::string(apt.GetBounds()).c_str(),
            apt.GetRwyEndPtVec().size() / 2,
            apt.GetRwysString().c_str(),
            apt.GetTaxiNodesVec().size(),
            apt.GetTaxiEdgeVec().size() - apt.GetRwyEndPtVec().size()/2);

    // Access to the list of airports is guarded by a lock
    const std::string key = apt.GetId();          // make a copy of the key, as `apt` gets moved soon:
    {
        std::lock_guard<std::mutex> lock(mtxGMapApt);
        gmapApt.emplace(key, std::move(apt));
    }
    
#ifdef DEBUG
    if (dataRefs.GetLogLevel() == logDEBUG)
        LTAptDump(key);
#endif
}

/// Return the a node, ie. the starting point of the edge
const TaxiNode& TaxiEdge::GetA (const Apt& apt) const
{
    if (type == RUN_WAY)
        return apt.GetRwyEndPtVec()[a];
    else
        return apt.GetTaxiNodesVec()[a];
}

/// Return the b node, ie. the ending point of the edge
const TaxiNode& TaxiEdge::GetB (const Apt& apt) const
{
    if (type == RUN_WAY)
        return apt.GetRwyEndPtVec()[b];
    else
        return apt.GetTaxiNodesVec()[b];
}


//
// MARK: File Reading Thread
// This code runs in the thread for file reading operations
//

/// @brief Process one "120" section of an `apt.dat` file, which contains a taxi line definitions in the subsequent 111-116 lines
/// @details Starts reading in the next line, expecting nodes in lines starting with 111-116.
///          According to specs, such a section has to end with 113-116. But we don't rely on it,
///          so we are more flexible in case of errorneous files. We read until we find a line _not_ starting
///          with 111-116 and return that back to the caller to be processed again.\n
///          We only process line segments with Line Type Codes 1, 7, 51, 57 (Taxiway centerlines).\n
///          All nodes are temporarily stored in a local list. After reading some nodes are removed,
///          as in actual files nodes can be very close together (up to being identical!).
///          We combine nodes to longer egdes until the edge's angle turns more than 15° away
///          from the orginal heading. Then only the next edge begins. This thins out nodes and egdes.
///          The remaining nodes and edges are added to the apt's taxiway network.
/// @returns the next line read from the file, which is after the "120" section
static std::string ReadOneTaxiLine (std::ifstream& fIn, Apt& apt, unsigned long& lnNr)
{
    vecTaxiNodesTy vecNodes;            // temporarily stored nodes in order of appearance
    std::string ln;
    while (fIn)
    {
        // read a line from the input file
        safeGetline(fIn, ln);
        ++lnNr;

        // ignore empty lines
        if (ln.empty()) continue;
        
        // tokenize the line
        std::vector<std::string> fields = str_tokenize(ln, " \t", true);
        
        // We need at minimum 3 fields (line id, latitude, longitude)
        if (fields.size() < 3) break;
        
        // Check for any of "our" line codes (we treat them all equal)
        const int lnCod = std::stoi(fields[0]);
        if (111 <= lnCod && lnCod <= 116)
        {
            // Check for the Line Type Code to be Taxi Centerline
            int lnTypeCode = 1;         // by default we add (also goes for lnCod 115,116!)
            // In case of line codes 111, 113 the Line Type Code is in field 3
            if (lnCod == 111 || lnCod == 113) {
                if (fields.size() >= 4)
                    lnTypeCode = std::stoi(fields[3]);
            // In case of line codes 112, 114 the Line Type Code is in field 5
            } else if (lnCod == 112 || lnCod == 114) {
                if (fields.size() >= 6)
                    lnTypeCode = std::stoi(fields[5]);
            }
            
            // Taxi Centerline?
            if (lnTypeCode ==  1 || lnTypeCode ==  7 ||
                lnTypeCode == 51 || lnTypeCode == 57)
            {
                // add the node temporarily if it different from previous
                // (there are quite a number of _exactly_ equal subsequent nodes
                //  in actual apt.dat, which we filter out this way)
                const double lat = std::stod(fields[1]);
                const double lon = std::stod(fields[2]);
                if (vecNodes.empty() || !vecNodes.back().CompEqualLatLon(lat,lon))
                    vecNodes.emplace_back(lat, lon);
            } else {
                // Not a Taxi Centerline, so we don't bother any longer, stop processing
                break;
            }
        }
        else            // not any of our codes -> stop processing
            break;
    }
    
    // Reading the section is done, now process the resulting nodes
    if (vecNodes.size() >= 2)
    {
        // The indexes to be used when adding the edge.
        // idxA points to the (already added) first node,
        // idxB to the just now added second node
        size_t idxA=ULONG_MAX, idxB=ULONG_MAX;
        
        // The first node of the entire list is definitely used, add it already
        const size_t idxA_First =
        idxA = apt.AddTaxiNode(vecNodes.front().lat,
                               vecNodes.front().lon);
        
        // The very last node will also be added later.
        // Between these two:
        // Combine adges til heading changes too much.
        // add the remainder to the airport's taxi network
        double firstAngle = NAN;
        for (vecTaxiNodesTy::const_iterator iEnd   = vecNodes.cbegin();
             iEnd != std::prev(vecNodes.cend());
             ++iEnd)
        {
            const TaxiNode& a = apt.GetTaxiNodesVec()[idxA];    // last node already added to the network
            const TaxiNode& b = *iEnd;              // last node that is confirmed to be part of the edge
            const TaxiNode& c = *std::next(iEnd);   // next node, to be validated if still in the edge
            double bcAngle = CoordAngle(b.lat, b.lon, c.lat, c.lon);
            if (std::isnan(firstAngle))             // new edge has just start, this is our reference angle
                firstAngle = bcAngle;
            else if ((DistLatLonSqr(a.lat,a.lon,c.lat,c.lon) > APT_MAX_EDGE_LEN_M2) ||
                     (std::abs(HeadingDiff(firstAngle, bcAngle)) > APT_MAX_TAXI_SEGM_TURN))
            {
                // The heading of the edge between b and c
                // is more than 15° away from the first node
                // -> We stop adding to the edge, ie. we now add first->b to the network
                idxB = apt.AddTaxiNode(b.lat, b.lon);
                if (idxA != idxB) {
                    apt.AddTaxiEdge(idxA, idxB);
                    idxA = idxB;
                    firstAngle = NAN;
                }
            }
        }
        
        // The last node of the list is also always to be added
        idxB = apt.AddTaxiNode(vecNodes.back().lat,
                               vecNodes.back().lon,
                               idxA_First);     // never combine with very first node; this ensures that at least one edge will be added!
        if (idxA != idxB) {
            apt.AddTaxiEdge(idxA, idxB);
        }
    }
    
    // return the last line so it can be processed again
    return ln;
}

/// @brief Read airports in the one given `apt.dat` file
/// @details    The function process the following line types:\n
///             1 - Airport header to start a new airport and learn its name/id\n
///             100 - Runway definitions\n
///             120 - Line segments (incl. subsequent 111-116 codes), or alternatively, if no 120 code is found:\n
///             1201, 1202  - Taxi route netwirk
static void ReadOneAptFile (std::ifstream& fIn, const boundingBoxTy& box)
{
    // Walk the file
    std::string ln;
    unsigned long lnNr = 0;             // for debugging purposes we are interested to track the file's line number
    bool bProcessGivenLn = false;       // process a line returned by a sub-routine?
    // Are we reading 120 taxi centerlines or 1200 taxi route network?
    enum netwTypeTy { NETW_UNKOWN=0, NETW_CENTERLINES, NETW_TAXIROUTES } netwType = NETW_UNKOWN;
    Apt apt;
    while (!bStopThread && (bProcessGivenLn || fIn))
    {
        // Either process a given line or fetch a new one
        if (bProcessGivenLn) {
            // the line is in `ln` already, just reset the flag
            bProcessGivenLn = false;
        } else {
            // read a fresh line from the file
            ln.clear();
            safeGetline(fIn, ln);
            ++lnNr;
        }
        
        // ignore empty lines
        if (ln.empty()) continue;
        
        // test for beginning of an airport
        if (ln.size() > 10 &&
            ln[0] == '1' &&
            (ln[1] == ' ' || ln[1] == '\t'))
        {
            // found an airport's beginning
            
            // If the previous airport is valid add it to the list
            if (apt.IsValid())
                Apt::AddApt(std::move(apt));
            else
                // clear the airport object nonetheless
                apt = Apt();
            
            // separate the line into its field values
            std::vector<std::string> fields = str_tokenize(ln, " \t", true);
            if (fields.size() >= 5 &&           // line contains an airport id, and
                gmapApt.count(fields[4]) == 0)  // airport is not yet defined in map
            {
                // re-init apt object, now with the proper id defined
                apt = Apt(fields[4]);
            }
        }
        
        // test for a runway...just to find location info
        else if (apt.HasId() &&             // an airport identified and of interest?
            ln.size() > 20 &&            // line long enough?
            ln[0] == '1' &&              // starting with "100 "?
            ln[1] == '0' &&
            ln[2] == '0' &&
            (ln[3] == ' ' || ln[3] == '\t'))
        {
            // separate the line into its field values
            std::vector<std::string> fields = str_tokenize(ln, " \t", true);
            if (fields.size() == 26) {      // runway description has to have 26 fields
                const double lat = std::stod(fields[ 9]);
                const double lon = std::stod(fields[10]);
                if (-90.0 <= lat && lat <= 90.0 &&
                    -180.0 <= lon && lon < 180.0)
                {
                    // Have we accepted the airport already?
                    // Or - this being the first rwy - does the rwy lie in the search bounding box?
                    if (apt.HasTaxiWays() ||
                        box.contains(positionTy(lat,lon)))
                    {
                        // add both runway ends to the airport
                        apt.AddRwyEnds(lat, lon,
                                       std::stod(fields[11]),       // displaced
                                       fields[ 8],                  // id
                                       // other rwy end:
                                       std::stod(fields[18]),       // lat
                                       std::stod(fields[19]),       // lon
                                       std::stod(fields[20]),       // displayced
                                       fields[17]);                 // id
                    }
                    // airport is outside bounding box -> mark it uninteresting
                    else
                    {
                        // clear the airport object
                        apt = Apt();
                    }
                }   // if lat/lon in acceptable range
            }       // if line contains 26 field values
        }           // if a runway line startin with "100 "
        
        // test for the start of a taxi line segment
        // This is valid for 120 as well as 120x:
        else if (apt.HasRwyEndpoints() &&
                 ln.size() >= 3 &&
                 ln[0] == '1' &&
                 ln[1] == '2' &&
                 ln[2] == '0')
        {
            // Standard Line segment, that could be a centerline?
            if (netwType != NETW_TAXIROUTES &&                              // not yet decided for the other type of network?
                (ln.size() == 3 ||                                          // was just the text "120"
                 (ln.size() >= 4 && (ln[3] == ' ' || ln[3] == '\t'))))      // or "120 " plus more
            {
                // Read the entire line segment
                ln = ReadOneTaxiLine(fIn, apt, lnNr);
                bProcessGivenLn = true;         // process the returned line read from the file
                if (apt.HasTaxiWays())          // did we (latest now) add taxi segments?
                    netwType = NETW_CENTERLINES;
            }
            else if (netwType != NETW_CENTERLINES)
            {
                // separate the line into its field values
                std::vector<std::string> fields = str_tokenize(ln, " \t", true);
                int lnCode = std::stoi(fields[0]);
                
                // 1201 - Taxi route network node
                if (lnCode == 1201 && fields.size() >= 5) {
                    // Convert and briefly test the given location
                    const double lat = std::stod(fields[1]);
                    const double lon = std::stod(fields[2]);
                    const size_t idx = std::stoul(fields[4]);
                    if (-90.0 <= lat && lat <= 90.0 &&
                        -180.0 <= lon && lon < 180.0)
                    {
                        netwType = NETW_TAXIROUTES;
                        apt.AddTaxiNodeFixed(lat, lon, idx);
                    }   // has valid location
                }
                else if (lnCode == 1202 && fields.size() >= 3) {
                    // Convert indexes and try adding the node
                    const size_t n1 = std::stoul(fields[1]);
                    const size_t n2 = std::stoul(fields[2]);
                    apt.AddTaxiEdge(n1, n2);
                }
            }       // not NETW_CENTERLINE
        }           // "120"
    }               // for each line of the apt.dat file
    
    // If the last airport read is valid don't forget to add it to the list
    if (apt.IsValid())
        Apt::AddApt(std::move(apt));
}

/// @brief Remove airports that are now considered too far away
void PurgeApt (const boundingBoxTy& _box)
{
    // Access is guarded by a lock
    std::lock_guard<std::mutex> lock(mtxGMapApt);

    // loop all airports and remove those, whose center point is outside the box
    mapAptTy::iterator iter = gmapApt.begin();
    while (iter != gmapApt.end())
    {
        // Is airport still in box?
        Apt& apt = iter->second;
        if (apt.GetBounds().overlap(_box)) {
            // keep it, move on to next airport
            ++iter;
        } else {
            // remove it, move on to next airport
            LOG_MSG(logDEBUG, "apt.dat: Removed %s at %s",
                    apt.GetId().c_str(),
                    std::string(apt.GetBounds()).c_str());
            iter = gmapApt.erase(iter);
        }
    }
    
    LOG_MSG(logDEBUG, "Done purging, %d airports left", (int)gmapApt.size());
}

/// @brief Read airports from apt.dat files around a given center position
/// @details This function first walks along the `scenery_packs.ini` file
///          and reads all `apt.dat` files available in the scenery packs listed there in the given order.
///          Lastly, it also reads the generic `apt.dat` file given in `APTDAT_RESOURCES_DEFAULT`.
/// @see Understanding scener order: https://www.x-plane.com/kb/changing-custom-scenery-load-order-in-x-plane-10/
/// @param ctr Center position
/// @param radius Search radius around center position in meter
void AsyncReadApt (positionTy ctr, double radius)
{
    static size_t lenSceneryLnBegin = strlen(APTDAT_SCENERY_LN_BEGIN);
    
    // To avoid costly distance calculations we define a bounding box
    // just by calculating lat/lon values north/east/south/west of given pos
    // and include all airports with coordinates falling into it
    const boundingBoxTy box (ctr, radius);
    
    // --- Cleanup first: Remove too far away airports ---
    PurgeApt(box);
    
    // --- Add new airports ---
    // Count the number of files we have accessed
    int cntFiles = 0;

    // Try opening scenery_packs.ini
    std::ifstream fScenery (LTCalcFullPath(APTDAT_SCENERY_PACKS));
    while (!bStopThread && fScenery.good() && fScenery.is_open())
    {
        // read a line from scenery_packs.ini
        std::string lnScenery;
        safeGetline(fScenery, lnScenery);
        
        // we only process lines starting with "SCENERY_PACK ",
        // ie. we skip any header info and also lines with SCENERY_PACK_DISABLED
        if (lnScenery.length() <= lenSceneryLnBegin ||
            lnScenery.substr(0,lenSceneryLnBegin) != APTDAT_SCENERY_LN_BEGIN)
            continue;
        
        // the remainder is a path into X-Plane's main folder
        lnScenery.erase(0,lenSceneryLnBegin);
        lnScenery = LTCalcFullPath(lnScenery);      // make it a full path
        lnScenery += APTDAT_SCENERY_ADD_LOC;        // add the location to the actual `apt.dat` file
        
        // open that apt.dat
        std::ifstream fIn (lnScenery);
        if (fIn.good() && fIn.is_open()) {
            LOG_MSG(logDEBUG, "Reading apt.dat from %s", lnScenery.c_str());
            ReadOneAptFile(fIn, box);
            cntFiles++;
        }
        
        // problem was not just "not found" (which we ignore for scenery packs) or eof?
        if (!fIn && errno != ENOENT && !fIn.eof()) {
            char sErr[SERR_LEN];
            strerror_s(sErr, sizeof(sErr), errno);
            LOG_MSG(logERR, ERR_CFG_FILE_READ,
                    lnScenery.c_str(), sErr);
        }
        
        fIn.close();

    } // processing scenery_packs.ini
    
    // Last but not least we also process the global generic apt.dat file
    if (!bStopThread)
    {
        const std::string sFileName = LTCalcFullPath(APTDAT_RESOURCES_DEFAULT APTDAT_SCENERY_ADD_LOC);
        std::ifstream fIn (sFileName);
        if (fIn.good() && fIn.is_open()) {
            LOG_MSG(logDEBUG, "Reading apt.dat from %s", sFileName.c_str());
            ReadOneAptFile(fIn, box);
            cntFiles++;
        }

        // problem was not just eof?
        if (!fIn && !fIn.eof()) {
            char sErr[SERR_LEN];
            strerror_s(sErr, sizeof(sErr), errno);
            LOG_MSG(logERR, ERR_CFG_FILE_READ,
                    sFileName.c_str(), sErr);
        }
        
        fIn.close();
    }
    
    // Not successful in opening ANY apt.dat file?
    if (!cntFiles) {
        SHOW_MSG(logWARN, WARN_APTDAT_FAILED);
        return;
    }
    
    LOG_MSG(logDEBUG, "Done reading from %d apt.dat files, have now %d airports",
            cntFiles, (int)gmapApt.size());
}

//
// MARK: Utility Functions
//

/// Find airport, which contains passed-in position, can be `nullptr`
Apt* LTAptFind (const positionTy& pos)
{
    for (auto& pair: gmapApt)
        if (pair.second.Contains(pos))
            return &pair.second;
    return nullptr;
}

//
// MARK: X-Plane Main Thread
// This code runs in X-Plane's thread, called from XP callbacks
//

/// Is currently an async operation running to refresh the airports from apt.dat?
static std::future<void> futRefreshing;
        
/// Last position for which airports have been read
static positionTy lastCameraPos;

/// New airports added, so that a call to LTAptUpdateRwyAltitude(9 is necessary?
static bool bAptsAdded = false;
        
// Start reading apt.dat file(s)
bool LTAptEnable ()
{
    LTAptRefresh();
    return true;
}

/// Update altitudes of runways
void LTAptUpdateRwyAltitudes ()
{
    // access is guarded by a lock
    std::lock_guard<std::mutex> lock(mtxGMapApt);

    // loop all airports and their runways
    for (mapAptTy::value_type& p: gmapApt)
        p.second.UpdateAltitudes();
    
    LOG_MSG(logDEBUG, "apt.dat: Finished updating ground altitudes");
}

// Update the airport data with airports around current camera position
void LTAptRefresh ()
{
    // Safety check: Thread already running?
    // Future object is valid, i.e. initialized with an async operation?
    if (futRefreshing.valid() &&
        // but status is not yet ready?
        futRefreshing.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            // then stop here
            return;
        
    // Distance since last read not far enough?
    // Must have travelled at least as far as standard search radius for planes:
    const positionTy camera = DataRefs::GetViewPos();
    if (!camera.isNormal(true))                     // have no good camery position (yet)
        return;

    double radius = dataRefs.GetFdStdDistance_m();
    if (lastCameraPos.dist(camera) < radius)        // is false if lastCameraPos is NAN
    {
        // Didn't move far, so no new scan for new airports needed.
        // But do we need to check for rwy altitudes after last scan of apt.dat file?
        if (bAptsAdded) {
            LTAptUpdateRwyAltitudes();
        }
        bAptsAdded = false;
        return;
    }
    else
        lastCameraPos = camera;

    // Start the thread to read apt.dat, using current camera position as center point
    // and _double_ plane search radius as search radius
    radius *= 2;
    LOG_MSG(logDEBUG, "Starting thread to read apt.dat for airports %.1fnm around %s",
            radius / M_per_NM, std::string(lastCameraPos).c_str());
    bStopThread = false;
    futRefreshing = std::async(std::launch::async,
                               AsyncReadApt, lastCameraPos, radius);
    // need to check for rwy altitudes soon!
    bAptsAdded = true;
}

// Return the best possible runway to auto-land at
positionTy LTAptFindRwy (const LTAircraft& _ac)
{
    // --- Preparation of aircraft-related data ---
    // allowed VSI range depends on aircraft model, converted to m/s
    const double vsi_min = _ac.mdl.VSI_FINAL * ART_RWY_MAX_VSI_F * Ms_per_FTm;
    const double vsi_max = _ac.mdl.VSI_FINAL / ART_RWY_MAX_VSI_F * Ms_per_FTm;
    
    // last known go-to position of aircraft, serving as start of search
    const positionTy& from = _ac.GetToPos();
    // The heading we compare the runway with is normalized to [0..180)
    double headSearch = HeadingNormalize(from.heading());
    bool bHeadInverted = false;
    if (headSearch >= 180.0) {
        headSearch -= 180.0;
        bHeadInverted = true;
    }

    // The speed to use, cut off at a reasonable approach speed:
    const double speed_m_s = std::min (_ac.GetSpeed_m_s(),
                                       _ac.mdl.FLAPS_DOWN_SPEED * ART_APPR_SPEED_F / KT_per_M_per_S);
    
    // --- Variables holding Best Match ---
    const Apt* bestApt = nullptr;               // best matching apt
    const TaxiEdge* bestRwy = nullptr;          // best matching rwy
    const RwyEndPt* bestRwyEndPt = nullptr;     // best matching runway endpoint
    // The heading diff of the best match to its runway
    // (initialized to the max allowed value so that worse heading diffs aren't considered)
    double bestHeadingDiff = ART_RWY_MAX_HEAD_DIFF;
    // when would we arrive there?
    double bestArrivalTS = NAN;
    
    // --- Iterate the airports ---
    // Access to the list of airports is guarded by a lock
    std::lock_guard<std::mutex> lock(mtxGMapApt);

    // loop over airports
    for (mapAptTy::const_iterator iterApt = gmapApt.cbegin();
         iterApt != gmapApt.cend();
         ++iterApt)
    {
        const Apt& apt = iterApt->second;
        
        // Find the runways matching the current plane's heading
        vecIdxTy lstRwys;
        if (apt.FindEdgesForHeading(headSearch,
                                    ART_RWY_MAX_HEAD_DIFF,
                                    lstRwys,
                                    TaxiEdge::RUN_WAY))
        {
            // loop over found runways of this airport
            for (size_t eIdx: lstRwys)
            {
                // The rwy end point we are (potentially) aiming at
                const TaxiEdge& e = apt.GetTaxiEdgeVec()[eIdx];
                const RwyEndPt& rwyEP = bHeadInverted ? e.GetRwyEP_B(apt) : e.GetRwyEP_A(apt);
                
                // We need to know the runway's altitude for what comes next
                if (std::isnan(rwyEP.alt_m))
                    continue;

                // Heading towards rwy, compared to current flight's heading
                // (Find the rwy which requires least turn now.)
                const double bearing = CoordAngle(from.lat(), from.lon(), rwyEP.lat, rwyEP.lon);
                const double headingDiff = std::abs(HeadingDiff(from.heading(), bearing));
                if (headingDiff > bestHeadingDiff)      // worse than best known match?
                    continue;
                
                // 3. Vertical speed, for which we need to know distance / flying time
                const double dist = CoordDistance(from.lat(), from.lon(), rwyEP.lat, rwyEP.lon);
                const double d_ts = dist / speed_m_s;
                const double vsi = (rwyEP.alt_m - from.alt_m()) / d_ts;
                if (vsi < vsi_min || vsi > vsi_max)
                    continue;
                
                // We've got a match!
                bestApt = &apt;
                bestRwy = &e;
                bestRwyEndPt = &rwyEP;
                bestHeadingDiff = headingDiff;      // the heading diff (which would be a selection criterion on several rwys match)
                bestArrivalTS = from.ts() + d_ts;   // the arrival timestamp
            }
        }
    }
    
    // Didn't find a suitable runway?
    if (!bestRwyEndPt || !bestApt) {
        LOG_MSG(logDEBUG, "Didn't find runway for %s with heading %.0f°",
                std::string(_ac).c_str(),
                from.heading());
        return positionTy();
    }
    
    // Found a match!
    positionTy retPos = positionTy(bestRwyEndPt->lat,
                                   bestRwyEndPt->lon,
                                   bestRwyEndPt->alt_m,
                                   bestArrivalTS,
                                   bestRwy->angle + (bHeadInverted ? 180.0 : 0.0),
                                   _ac.mdl.PITCH_FLARE,
                                   0.0,
                                   positionTy::GND_ON,
                                   positionTy::UNIT_WORLD, positionTy::UNIT_DEG,
                                   LTAPIAircraft::FPH_TOUCH_DOWN);
    LOG_MSG(logDEBUG, "Found runway %s/%s at %s for %s",
            bestApt->GetId().c_str(),
            bestRwyEndPt->id.c_str(),
            std::string(retPos).c_str(),
            std::string(_ac).c_str());
    return retPos;
}


// Snaps the passed-in position to the nearest rwy or taxiway if appropriate
bool LTAptSnap (LTFlightData& fd, dequePositionTy::iterator& posIter)
{
    // Configured off?
    if (dataRefs.GetFdSnapTaxiDist_m() <= 0)
        return false;
    
    // Access to the list of airports is guarded by a lock
    std::lock_guard<std::mutex> lock(mtxGMapApt);

    // Which airport are we looking at?
    Apt* pApt = LTAptFind(*posIter);
    if (!pApt)                          // not a position in any airport's bounding box
        return false;

    // Let's snap!
    return pApt->SnapToTaxiway(fd, posIter);
}


// Cleanup
void LTAptDisable ()
{
    // Stop all threads
    bStopThread = true;
    
    // wait for refresh function
    if (futRefreshing.valid())
        futRefreshing.wait();
    
    // destroy the Y Probe
    Apt::DestroyYProbe();
}


#ifdef DEBUG
// Dumps the entire taxi network into a CSV file readable by GPS Visualizer
/// @see For a suggestion of settings for display:
/// https://www.gpsvisualizer.com/map_input?bg_map=google_openstreetmap&bg_opacity=70&form=leaflet&google_wpt_sym=diamond&trk_list=0&trk_opacity=100&trk_width=2&units=metric&width=1400&wpt_color=aqua
void LTAptDump (const std::string& _aptId)
{
    // find the airport by id
    if (gmapApt.count(_aptId) < 1) return;
    const Apt& apt = gmapApt.at(_aptId);
    
    // open the output file
    const std::string fileName (dataRefs.GetXPSystemPath() + _aptId + ".csv");
    std::ofstream out (fileName, std::ios_base::out | std::ios_base::trunc);
    // column headers
    out << "type,BOT,symbol,latitude,longitude,time,speed,course,name,desc\n";
    out.precision(8);               // precision is all digits, so we need something like 123.45678
    // Dump all nodes as Waypoints
    size_t i = 0;
    for (const TaxiNode& n: apt.GetTaxiNodesVec())
        out
        << "W,,"                                    // type, BOT
        << (n.vecEdges.size() == 0 ? "pin," :       // symbol
            n.vecEdges.size() == 1 ? "circle," :
            n.vecEdges.size() == 2 ? "square," :
            n.vecEdges.size() == 3 ? "triangle," :
            n.vecEdges.size() == 4 ? "diamond," : "star,")
        << n.lat << ',' << n.lon << ','             // latitude,longitude
        << ",,,"                                    // time,speed,course
        << "Node " << (i++) << ','                  // name
        << n.vecEdges.size() << " edges"            // desc
        << "\n";
    
    // Dump all edges as Tracks
    i = 0;
    for (const TaxiEdge& e: apt.GetTaxiEdgeVec())
    {
        const TaxiNode& a = e.GetA(apt);
        const TaxiNode& b = e.GetB(apt);
        
        out
        << "T,1,,"                                  // type, BOT, symbol
        << a.lat << ',' << a.lon << ','             // latitude,longitude
        << ",,"                                     // time,speed
        << e.angle << ','                           // course
        << "Edge " << (i++) << ','                  // name
        << "nodes " << e.startNode() << '-' << e.endNode() // desc
        << "\n";

        out
        << "T,0,,"                                  // type, BOT, symbol
        << b.lat << ',' << b.lon << ','             // latitude,longitude
        << ",,"                                     // time,speed
        << ','                                      // course
        << ','                                      // name, desc
        << "\n";

    }
        

    // Close the file
    out.close();
}
#endif
