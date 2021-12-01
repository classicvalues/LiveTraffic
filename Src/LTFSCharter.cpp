/// @file       LTFSCharter.cpp
/// @brief      FSCharter: Requests and processes FSC tracking data
/// @see        https://fscharter.net/
/// @details    Implements FSCConnection:\n
///             - Takes care of login (OAuth)\n
///             - Provides a proper REST-conform URL\n
///             - Interprets the response and passes the tracking data on to LTFlightData.\n
/// @author     Birger Hoppe
/// @copyright  (c) 2021 Birger Hoppe
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

// All includes are collected in one header
#include "LiveTraffic.h"

//
// MARK: FSCharter Environment Configuration
//

/// Defines all relevant aspects of an FSCharter environment
struct FSCEnvTy {
    std::string     server;     ///< server domain, like "fscharter.net"
    unsigned        client_id=0;///< client_id when connecting using OAuth
    /// encoded client_secret for OAuth connection
    std::string     client_secrect_enc;
};

/// Type of array the environment configuration is stored in
typedef std::array<FSCEnvTy, 2> FSCEnvArrTy;

/// The list of available configurations
static FSCEnvArrTy FSC_ENV = {
    FSCEnvTy{"fscharter.net",        1,  "dDRCOUo4R1dUcDNpTk9SOUcyanpTNlRGclF6TFJVYnNIZnpVMHV0dA==" },
    FSCEnvTy{"master.fscharter.net", 1,  "d2RMMzdLa25McVRoZ0ZtR2kwQUU0cnNpaHFaQjNFU0U5T0lmWk9LTg==" },
};

//
//MARK: FSCharter
//

// Constructor
FSCConnection::FSCConnection () :
LTChannel(DR_CHANNEL_FSCHARTER, FSC_NAME),
LTOnlineChannel(),
LTFlightDataChannel()
{
    // purely informational
    urlName  = FSC_CHECK_NAME;
    urlLink  = FSC_CHECK_URL;
    urlPopup = FSC_CHECK_POPUP;
}


// Get FSC-specific status string
std::string FSCConnection::GetStatusStr () const
{
    switch (fscStatus) {
        case FSC_STATUS_OK:             return "Connected";
        case FSC_STATUS_NONE:           return "Starting...";
        case FSC_STATUS_LOGIN_FAILED:   return "Login failed!";
        case FSC_STATUS_LOGGING_IN:     return "Logging in...";
    }
    return "?";
}


// get status info, considering FSC-specific texts for login phases
std::string FSCConnection::GetStatusText () const
{
    if (!IsValid() || !IsEnabled() || fscStatus == FSC_STATUS_OK)
        return LTChannel::GetStatusText();
    else
        return GetStatusStr();
}


/// Initialize CURL, adding in FSC-required headers
bool FSCConnection::InitCurl ()
{
    // Standard-init first (repeated call will just return true without effect)
    if (!LTOnlineChannel::InitCurl())
        return false;
    
    // if there is a header already remove it first
    if (pCurlHeader)
        curl_slist_free_all(pCurlHeader);
    pCurlHeader = nullptr;
    
    // if we have a token then we add it to the header
    if ( !token.empty() && !token_type.empty() ) {
        char chBuf[2048];
        snprintf(chBuf, sizeof(chBuf), FSC_HEADER_AUTHORIZATION,
                 token_type.c_str(),
                 token.c_str());
        pCurlHeader = curl_slist_append(pCurlHeader, chBuf);
    }
    
    // we always need to say that we send/receive JSON format
    pCurlHeader = curl_slist_append(pCurlHeader, FSC_HEADER_JSON_SEND);
    LOG_ASSERT(pCurlHeader);
    pCurlHeader = curl_slist_append(pCurlHeader, FSC_HEADER_JSON_ACCEPT);
    
    // set the header
    LOG_ASSERT(pCurl);
    curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pCurlHeader);
    return true;
}


void FSCConnection::CleanupCurl ()
{
    LTOnlineChannel::CleanupCurl();
    if (pCurlHeader)
        curl_slist_free_all(pCurlHeader);
    pCurlHeader = nullptr;
}

// put together the URL to fetch based on current view position
std::string FSCConnection::GetURL (const positionTy& pos)
{
    char url[128] = "";
    switch (fscStatus) {
        // Standard operations: Return the request for fetching tracking data
        case FSC_STATUS_OK:
        {
            // TODO: Replace with actual FSC URL calculation
            // we add 10% to the bounding box to have some data ready once the plane is close enough for display
            boundingBoxTy box (pos, double(dataRefs.GetFdStdDistance_m()) * 1.10);
            snprintf(url, sizeof(url),
                     FSC_URL,
                     FSC_ENV.at(dataRefs.GetFSCEnv()).server.c_str(),
                     box.se.lat(),              // lamin
                     box.nw.lon(),              // lomin
                     box.nw.lat(),              // lamax
                     box.se.lon() );            // lomax
            return std::string(url);
        }
            
        // Not yet logged in, return the login request
        case FSC_STATUS_NONE:
        case FSC_STATUS_LOGGING_IN:
            // Set the status
            fscStatus = FSC_STATUS_LOGGING_IN;
            // put together the actual URL
            snprintf(url, sizeof(url),
                     FSC_LOGIN,
                     FSC_ENV.at(dataRefs.GetFSCEnv()).server.c_str());
            return std::string(url);
            
        // Error: Do nothing any longer
        case FSC_STATUS_LOGIN_FAILED:
            return "";
    }
    return "";
}


/// Puts together the body for the oauth request if we are in that state
void FSCConnection::ComputeBody ()
{
    switch (fscStatus) {
        // just return empty if we are in a "normal" state
        case FSC_STATUS_OK:
        case FSC_STATUS_LOGIN_FAILED:
            requBody.clear();
            break;
            
        case FSC_STATUS_NONE:
        case FSC_STATUS_LOGGING_IN:
        {
            // Credentials
            std::string username, password;
            dataRefs.GetFSCharterCredentials(username, password);
            str_replaceAll(password, "\\", "\\\\");         // replace backslashes with double backslashes
            str_replaceAll(password, "\"", "\\\"");         // escape double quotes with a backslash
            
            // Put together the request body
            requBody = "{\"grant_type\": \"password\",\"client_id\": \"";
            requBody += std::to_string(FSC_ENV.at(dataRefs.GetFSCEnv()).client_id);
            requBody += "\",\"client_secret\": \"";
            requBody += DecodeBase64(FSC_ENV.at(dataRefs.GetFSCEnv()).client_secrect_enc);
            requBody += "\",\"username\": \"";
            requBody += username;
            requBody += "\",\"password\": \"";
            requBody += password;
            requBody += "\",\"scope\": \"\"}";            
            break;
        }
    }
}

// update shared flight data structures with received flight data
bool FSCConnection::ProcessFetchedData (mapLTFlightDataTy& fdMap)
{
    // any a/c filter defined for debugging purposes?
    std::string acFilter ( dataRefs.GetDebugAcFilter() );
    
    // data is expected to be in netData string
    // short-cut if there is nothing
    if ( !netDataPos ) return true;
    
    //
    // --- Login response ---
    //
    if (fscStatus == FSC_STATUS_LOGGING_IN)
    {
        // try parsing as JSON
        JSON_Value* pRoot = json_parse_string(netData);
        JSON_Object* pObj = pRoot ? json_object(pRoot) : nullptr;

        // Failed?
        if (httpResponse != HTTP_OK) {
            fscStatus = FSC_STATUS_LOGIN_FAILED;
            // try to get reason from response
            std::string msg = pObj ? jog_s(pObj, "message") : "";
            SHOW_MSG(logERR, "FSCharter login failed! %s", msg.c_str());
            SetValid(false);
            return false;
        }
        
        // parsing as JSON OK?
        if (!pRoot) { LOG_MSG(logERR,ERR_JSON_PARSE); IncErrCnt(); return false; }
        if (!pObj) { LOG_MSG(logERR,ERR_JSON_MAIN_OBJECT); IncErrCnt(); return false; }

        // look for and return values from the response
        token_type      = jog_s(pObj, "token_type");
        token           = jog_s(pObj, "access_token");
        
        // both must have been found!
        if (token_type.empty() || token.empty()) {
            fscStatus = FSC_STATUS_LOGIN_FAILED;
            SHOW_MSG(logERR, "FSCharter login returned empty token!");
            SetValid(false);
            return false;
        }
        
        // Success!
        fscStatus = FSC_STATUS_OK;
        LOG_MSG(logINFO, "FSCharter login succeeded");
        return true;
    }
    
    //
    // --- Standard Tracking Data ---
    //
    
    // Only proceed in case HTTP response was OK
    if (httpResponse != HTTP_OK) {
        // There are a few typical responses that may happen when FSCharter
        // is just temporarily unresponsive. But in all _other_ cases
        // we increase the error counter.
        if (httpResponse != HTTP_BAD_GATEWAY        &&
            httpResponse != HTTP_NOT_AVAIL          &&
            httpResponse != HTTP_GATEWAY_TIMEOUT    &&
            httpResponse != HTTP_TIMEOUT)
            IncErrCnt();
        return false;
    }
    

    // TODO: Reimplement for FSCharter
    // now try to interpret it as JSON
    JSON_Value* pRoot = json_parse_string(netData);
    if (!pRoot) { LOG_MSG(logERR,ERR_JSON_PARSE); IncErrCnt(); return false; }
    
    // let's cycle the aircraft
    // first get the structre's main object
    JSON_Object* pObj = json_object(pRoot);
    if (!pObj) { LOG_MSG(logERR,ERR_JSON_MAIN_OBJECT); IncErrCnt(); return false; }
    
    // Cut-off time: We ignore tracking data, which is "in the past" compared to simTime
    const double tsCutOff = dataRefs.GetSimTime();

    // We need to calculate distance to current camera later on
    const positionTy viewPos = dataRefs.GetViewPos();

    // fetch the aircraft array
    JSON_Array* pJAcList = json_object_get_array(pObj, OPSKY_AIRCRAFT_ARR);
    if (!pJAcList) {
        // a/c array not found: can just mean it is 'null' as in
        // the empty result set: {"time":1541978120,"states":null}
        JSON_Value* pJSONVal = json_object_get_value(pObj, OPSKY_AIRCRAFT_ARR);
        if (!pJSONVal || json_type(pJSONVal) != JSONNull) {
            // well...it is something else, so it is malformed, bail out
            LOG_MSG(logERR,ERR_JSON_ACLIST,OPSKY_AIRCRAFT_ARR);
            IncErrCnt();
            return false;
        }
    }
    // iterate all aircraft in the received flight data (can be 0)
    else for ( size_t i=0; i < json_array_get_count(pJAcList); i++ )
    {
        // get the aircraft (which is just an array of values)
        JSON_Array* pJAc = json_array_get_array(pJAcList,i);
        if (!pJAc) {
            LOG_MSG(logERR,ERR_JSON_AC,i+1,OPSKY_AIRCRAFT_ARR);
            if (IncErrCnt())
                continue;
            else
                return false;
        }
        
        // the key: transponder Icao code
        LTFlightData::FDKeyTy fdKey (LTFlightData::KEY_ICAO,
                                     jag_s(pJAc, OPSKY_TRANSP_ICAO));
        
        // not matching a/c filter? -> skip it
        if ((!acFilter.empty() && (fdKey != acFilter)) )
        {
            continue;
        }
        
        // position time
        const double posTime = jag_n(pJAc, OPSKY_POS_TIME);
        if (posTime <= tsCutOff)
            continue;
        
        try {
            // from here on access to fdMap guarded by a mutex
            // until FD object is inserted and updated
            std::unique_lock<std::mutex> mapFdLock (mapFdMutex);
            
            // Check for duplicates with OGN/FLARM, potentially replaces the key type
            LTFlightData::CheckDupKey(fdKey, LTFlightData::KEY_FLARM);

            // get the fd object from the map, key is the transpIcao
            // this fetches an existing or, if not existing, creates a new one
            LTFlightData& fd = fdMap[fdKey];
            
            // also get the data access lock once and for all
            // so following fetch/update calls only make quick recursive calls
            std::lock_guard<std::recursive_mutex> fdLock (fd.dataAccessMutex);
            // now that we have the detail lock we can release the global one
            mapFdLock.unlock();

            // completely new? fill key fields
            if ( fd.empty() )
                fd.SetKey(fdKey);
            
            // fill static data
            LTFlightData::FDStaticData stat;
            stat.country =    jag_s(pJAc, OPSKY_COUNTRY);
            stat.trt     =    trt_ADS_B_unknown;
            stat.call    =    jag_s(pJAc, OPSKY_CALL);
            while (!stat.call.empty() && stat.call.back() == ' ')      // trim trailing spaces
                stat.call.pop_back();
            
            // dynamic data
            {   // unconditional...block is only for limiting local variables
                LTFlightData::FDDynamicData dyn;
                
                // non-positional dynamic data
                dyn.radar.code =  (long)jag_sn(pJAc, OPSKY_RADAR_CODE);
                dyn.gnd =               jag_b(pJAc, OPSKY_GND);
                dyn.heading =           jag_n_nan(pJAc, OPSKY_HEADING);
                dyn.spd =               jag_n(pJAc, OPSKY_SPD);
                dyn.vsi =               jag_n(pJAc, OPSKY_VSI);
                dyn.ts =                posTime;
                dyn.pChannel =          this;
                
                // position
                positionTy pos (jag_n_nan(pJAc, OPSKY_LAT),
                                jag_n_nan(pJAc, OPSKY_LON),
                                dataRefs.WeatherAltCorr_m(jag_n_nan(pJAc, OPSKY_BARO_ALT)),
                                posTime,
                                dyn.heading);
                pos.f.onGrnd = dyn.gnd ? GND_ON : GND_OFF;
                
                // Update static data
                fd.UpdateData(std::move(stat), pos.dist(viewPos));

                // position is rather important, we check for validity
                // (we do allow alt=NAN if on ground as this is what OpenSky returns)
                if ( pos.isNormal(true) )
                    fd.AddDynData(dyn, 0, 0, &pos);
                else
                    LOG_MSG(logDEBUG,ERR_POS_UNNORMAL,fdKey.c_str(),pos.dbgTxt().c_str());
            }
        } catch(const std::system_error& e) {
            LOG_MSG(logERR, ERR_LOCK_ERROR, "mapFd", e.what());
        }
    }
    
    // cleanup JSON
    json_value_free (pRoot);
    
    // success
    return true;
}


// do something while disabled?
void FSCConnection::DoDisabledProcessing ()
{
    ClearLogin();
}

// (temporarily) close a connection, (re)open is with first call to FetchAll/ProcessFetchedData
void FSCConnection::Close ()
{
    ClearLogin();
}


// Remove all traces of login
void FSCConnection::ClearLogin ()
{
    fscStatus = FSC_STATUS_NONE;
    token.clear();
    token_type.clear();
}
