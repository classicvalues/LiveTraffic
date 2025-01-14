/// @file       LTOpenGlider.h
/// @brief      Open Glider Network: Requests and processes live tracking data
/// @see        http://live.glidernet.org/
/// @details    Defines OpenGliderConnection:
///             - Direct TCP connection to aprs.glidernet.org:14580 (preferred)
///               - connects to the server
///               - sends a dummy login for read-only access
///               - listens to incoming tracking data
///
/// @see        http://wiki.glidernet.org/wiki:subscribe-to-ogn-data
///
/// @details    Alternatively, and as a fallback if APRS fails:
///             - Request/Reply Interface
///               - Provides a proper REST-conform URL
///               - Interprets the response and passes the tracking data on to LTFlightData.
///
/// @see        https://github.com/glidernet/ogn-live#backend
///
/// @details    Also downloads and performs searches in the aircraft list
/// @see        http://ddb.glidernet.org/download/
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

#ifndef LTOpenGlider_h
#define LTOpenGlider_h

#include "LTChannel.h"

//
//MARK: OpenGlider Constants
//

#define OPGLIDER_CHECK_NAME     "Live Glidernet"
#define OPGLIDER_CHECK_URL      "http://live.glidernet.org/#c=%.5f,%.5f&z=9&s=1"
#define OPGLIDER_CHECK_POPUP    "Check Open Glider Network's coverage"

#define OPGLIDER_NAME           "Open Glider Network"
#define OPGLIDER_URL            "http://live.glidernet.org/lxml.php?a=0&b=%.3f&c=%.3f&d=%.3f&e=%.3f"

#define OGN_AC_LIST_URL         "http://ddb.glidernet.org/download/"
#define OGN_AC_LIST_FILE        "Resources/OGNAircraft.lst"

//    a="lat      ,lon     ,CN ,reg   ,alt_m,ts      ,age_s,trk,speed_km_h,vert_m_per_s,a/c type,receiver,device id,OGN registration id"
// <m a="49.815819,7.957970,ADA,D-HYAF,188  ,21:20:27,318  ,343,11        ,-2.0        ,3       ,Waldalg3,3E1205   ,24064512"/>

/// Field indexes in live.glidernet.org's response
enum GliderNetFieldsTy {
    GNF_LAT         = 0,        ///< latitude
    GNF_LON,                    ///< longitude
    GNF_CN,                     ///< CN ("Wettbewerbskennung"), either registered, or some short form of the OGN registration id
    GNF_REG,                    ///< either official registration, or the (daily changing) OGN registration id
    GNF_ALT_M,                  ///< altitude in meter
    GNF_TS,                     ///< timestamp (zulu)
    GNF_AGE_S,                  ///< seconds since last received message (beacon)
    GNF_TRK,                    ///< track in degrees
    GNF_SPEED_KM_H,             ///< ground speed in km/h
    GNF_VERT_M_S,               ///< vertical speed in m/s
    GNF_FLARM_ACFT_TYPE,        ///< Flarm aircraft type (see ::FlarmAircraftTy)
    GNF_RECEIVER_ID,            ///< receiver id (of the station providing this received data)
    GNF_FLARM_DEVICE_ID,        ///< unique FLARM device id of the sender, optional, can be 0
    GNF_OGN_REG_ID,             ///< OGN registration id (expect to renew every day, so considered temporary)
    GNF_COUNT                   ///< always last, counts the number of fields
};

/// @brief OGN Aircraft type
/// @see https://github.com/wbuczak/ogn-commons-java/blob/master/src/main/java/org/ogn/commons/beacon/AircraftType.java
/// @see http://forums.skydemon.aero/Topic16427.aspx
enum FlarmAircraftTy : unsigned {
    FAT_UNKNOWN     = 0,        ///< unknown
    FAT_GLIDER      = 1,        ///< Glider / Sailplane / Motor-Glider
    FAT_TOW_PLANE   = 2,        ///< Tow / Tug Plane (usually a L1P type of plane)
    FAT_HELI_ROTOR  = 3,        ///< Helicopter, Rotorcraft
    FAT_PARACHUTE   = 4,        ///< Parachute
    FAT_DROP_PLANE  = 5,        ///< Drop Plane for parachutes (not rarely a L2T type of plane)
    FAT_HANG_GLIDER = 6,        ///< Hangglider
    FAT_PARA_GLIDER = 7,        ///< Paraglider
    FAT_POWERED_AC  = 8,        ///< Powered Aircraft
    FAT_JET_AC      = 9,        ///< Jet Aircraft
    FAT_UFO         = 10,       ///< Flying Saucer, UFO (well, yea...specification says so...not sure how the aliens can get hold of a FLARM sender before reaching earth, though...and _if_ they are interested in being tracked at all)
    FAT_BALLOON     = 11,       ///< Balloon
    FAT_AIRSHIP     = 12,       ///< Airship
    FAT_UAV         = 13,       ///< unmanned aerial vehicle
    FAT_STATIC_OBJ  = 15,       ///< static object (ignored)
};

/// @brief APRS Address type
/// @see https://github.com/wbuczak/ogn-commons-java/blob/master/src/main/java/org/ogn/commons/beacon/AddressType.java
enum APRSAddressTy : unsigned {
    APRS_ADDR_RANDOM    = 0,    ///< changing (random) address generated by the device
    APRS_ADDR_ICAO,             ///< ICAO address
    APRS_ADDR_FLARM,            ///< FLARM hardware address
    APRS_ADDR_OGN,              ///< OGN tracker's hardware address
};

//
// MARK: OpenGliderConnection
//

/// @brief Mapping structure for ids of planes that don't want to be identified
/// @see http://wiki.glidernet.org/opt-in-opt-out
/// @details The idea is to create a random id, which we send to LiveTraffic,
///          and only keep the anonymous id here in this map.
struct OGNAnonymousIdMapTy {
    unsigned long   anonymId = 0;   ///< anonymous id that we assign
    std::string     anonymCall;     ///< short call sign that we assign
    
    /// Constructor generates id so that structure is always filled
    OGNAnonymousIdMapTy () { GenerateNextId(); }
    void GenerateNextId ();                     ///< assigns the next anonymous id and generates also a call sign
};


/// Connection to OGN via APRS or HTTP
class OpenGliderConnection : public LTOnlineChannel, LTFlightDataChannel
{
protected:
    // APRS connection to receives tracking data
    std::thread thrAprs;            ///< thread for the APRS/TCP receiver
    TCPConnection tcpAprs;          ///< TCP connection to aprs.glidernet.org
    volatile bool bStopAprs=false;  ///< stop signal to the thread
    positionTy aprsPos;             ///< the search position with which we are connected to the tcp server
#if APL == 1 || LIN == 1
    /// the self-pipe to shut down the APRS thread gracefully
    SOCKET aprsPipe[2] = { INVALID_SOCKET, INVALID_SOCKET };
#endif
    std::string aprsData;           ///< received/unprocessed APRS data
    float aprsLastData = NAN;       ///< last time (XP network time) we received _any_ APRS data
    float aprsLastKeepAlive = 0;    ///< last time (XP network time) we send a keep-alive to APRS
    bool bFailoverToHttp = false;   ///< set if we had too much trouble on the APRS channel, then we try the HTTP R/R channel
    
    /// The map for mapping original to anonymous id
    std::map<std::string,OGNAnonymousIdMapTy> mapAnonymousId;

public:
    /// Constructor
    OpenGliderConnection ();
    /// Destructor closes the a/c list file
    ~OpenGliderConnection () override;
    /// All the cleanup we usually need
    void Cleanup ();
    /// Invokes APRS thread, or returns URL to fetch current data from live.glidernet.org
    std::string GetURL (const positionTy& pos) override;
    /// @brief Processes the fetched data
    bool ProcessFetchedData (mapLTFlightDataTy& fdMap) override;
    bool IsLiveFeed() const override { return true; }
    LTChannelType GetChType() const override { return CHT_TRACKING_DATA; }
    std::string GetStatusText () const override;  ///< return a human-readable staus
    bool FetchAllData(const positionTy& pos) override { return LTOnlineChannel::FetchAllData(pos); }
    void DoDisabledProcessing() override { Cleanup(); }
    void Close () override               { Cleanup(); }

    // APRS connection
protected:
    /// Main function for APRS connection, expected to be started in a thread
    void APRSMain (const positionTy& pos, unsigned dist_km);
    /// Send the APRS login message
    bool APRSDoLogin (const positionTy& pos, unsigned dist_km);
    /// Send a simple keep-alive message to APRS
    bool APRSSendKeepAlive ();
    /// Process received data
    bool APRSProcessData (const char* buffer);
    /// Process one line of received data
    bool APRSProcessLine (const std::string& ln);
    
    /// Start or restart a new thread for connecting to aprs.glidernet.org
    void APRSStartUpdate (const positionTy& pos, unsigned dist_km);
    /// Closes the APRS TCP connection
    void APRSClose ();
    
    // Aircraft List (Master Data)
protected:
    std::ifstream ifAcList;                 ///< Handle to the a/c list file
    size_t numRecAcList = 0;                ///< number of records in the file
    unsigned long minKeyAcList = 0;         ///< minimum key value in the file
    unsigned long maxKeyAcList = 0;         ///< maximum key value in the file
    /// @brief Tries reading aircraft information from the OGN a/c list
    /// @details Given the device id looks up the record in the DDB.
    ///          Fills the key, potentially with an anonymous key
    ///          in case the device doesn't want to be tracked.
    /// @see http://wiki.glidernet.org/opt-in-opt-out
    /// @param sDevId Device id to be looked up
    /// @param[out] key key of a/c found. KEY_FLARM, or KEY_ICAO if publishable, KEY_OGN with a generated anonymous key if not to be published
    /// @param[out] stat Filled with aircraft master data taken from DDB
    /// @return Shall the aircraft be displayed at all? (Otherwise it is marked non-trackable and we shall not show it.)
    bool LookupAcList (const std::string& sDevId,
                       LTFlightData::FDKeyTy& key,
                       LTFlightData::FDStaticData& stat);
};

//
// MARK: OGN Aircraft list file (DDB)
//

/// @brief Record structure of a record in the OGN Aircraft list file (DDB)
/// @details Data is stored in binary format so we can use seek to search in the file
struct OGN_DDB_RecTy {
    unsigned long   devId = 0;          ///< device id
    char devType     = ' ';             ///< device type (F, O, I)
    char mdl[26]     = {' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};  ///< aircraft model (text)
    char reg[9]      = {' ',' ',' ',' ',' ',' ',' ',' ',' '};  ///< registration
    char cn[3]       = {' ',' ',' '};   ///< CN
    unsigned char f  = 0;               ///< bit-encoded flags TRACKED and IDENTIFIED
    
    void SetTracked () { f |= 0x01; }               ///< set the TRACKED flag
    bool IsTracked () const { return f & 0x01; }    ///< is TRACKED flag set?
    void SetIdentified () { f |= 0x02; }            ///< set the IDENTIFIED flag
    bool IsIdentified () const { return f & 0x02; } ///< is IDENTIFIED flag set?
};

/// Hand-over structure to callback
struct OGNCbHandoverTy {
    unsigned devTypeIdx = 0;            ///< which field is the DEVICE_TYPE field?
    unsigned devIdIdx = 1;              ///< which field is the DEVICE_ID field?
    unsigned mdlIdx = 2;                ///< which field is the AIRCRAFT_MODEL field?
    unsigned regIdx = 3;                ///< which field is the REGISTRATION field?
    unsigned cnIdx = 4;                 ///< which field is the CN field?
    unsigned trackedIdx = 5;            ///< which field is the TRACKED field?
    unsigned identifiedIdx = 6;         ///< which field is the IDENTIFIED field?
    unsigned maxIdx = 6;                ///< maximum idx used? (this is the minimum length that can be processed)
    std::string readBuf;                ///< read buffer collecting responses from ddb.glidernet.org
    std::ofstream f;                    ///< file to write output to
};

//
// MARK: Global Functions
//

/// Return a descriptive text per flam a/c type
const char* OGNGetAcTypeName (FlarmAircraftTy _acTy);

/// @brief Return a matching ICAO type code per flarm a/c type
/// @details Pick one of the types defined by the user
const std::string& OGNGetIcaoAcType (FlarmAircraftTy _acTy);

/// Fill defaults for Flarm aircraft types where not existing
void OGNFillDefaultFlarmAcTypes ();

/// Fetch the aircraft list from OGN
void OGNDownloadAcList ();

#endif /* LTOpenGlider_h */
