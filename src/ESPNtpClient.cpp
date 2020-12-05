#include "ESPNtpClient.h"


#define DBG_PORT Serial

#ifdef DEBUG_NTPCLIENT

#ifdef ESP8266
const char* extractFileName (const char* path);
#define DEBUG_LINE_PREFIX() DBG_PORT.printf_P (PSTR("[%lu][%s:%d] %s() Heap: %lu | "),::millis(),extractFileName(__FILE__),__LINE__,__FUNCTION__,(unsigned long)ESP.getFreeHeap())
#define DEBUGLOG(text,...) DEBUG_LINE_PREFIX();DBG_PORT.printf_P(PSTR(text),##__VA_ARGS__);DBG_PORT.println()
#elif defined ESP32
#define ARDUHAL_NTP_LOG(format)  "[%s:%u] %d %s(): " format "\r\n", pathToFileName(__FILE__), __LINE__, (unsigned long)ESP.getFreeHeap(), __FUNCTION__
#define DEBUGLOG(format, ...) log_printf(ARDUHAL_NTP_LOG(format), ##__VA_ARGS__)
#else
#define DEBUGLOG(...) DBG_PORT.printf(__VA_ARGS__);DBG_PORT.println();
#endif // ESP8266
#else
#define DEBUGLOG(...)
#endif // DEBUG_NTPCLIENT

#ifdef ESP8266
const char* IRAM_ATTR extractFileName (const char* path) {
    size_t i = 0;
    size_t pos = 0;
    char* p = (char*)path;
    while (*p) {
        i++;
        if (*p == '/' || *p == '\\') {
            pos = i;
        }
        p++;
    }
    return path + pos;
}
#endif

  /**
    * @brief NTP Timestamp Format
    * The prime epoch, or base date of era 0, is 0 h 1 January 1900 UTC, when all bits are zero
    */
typedef struct {
    int32_t secondsOffset; ///< @brief 32-bit seconds field spanning 136 years since 1-Jan-1900 00:00 UTC
    uint32_t fraction; ///< @brief 32-bit fraction field resolving 232 picoseconds (1/2^32)
} timestamp64_t;

  /**
    * @brief Short NTP Timestamp Format
    * 
    * Used for precission, dispersion, etc
    */
typedef struct {
    int16_t secondsOffset; ///< @brief 16-bit seconds field spanning 18 hours
    uint16_t fraction; ///< @brief 16-bit fraction field resolving 15.3 microseconds (1/2^16)
} timestamp32_t;


typedef struct __attribute__ ((packed, aligned (1))) {
    uint8_t flags;
    uint8_t peerStratum;
    uint8_t pollingInterval;
    int8_t clockPrecission;
    timestamp32_t rootDelay;
    timestamp32_t dispersion;
    uint8_t refID[4];
    timestamp64_t reference;
    timestamp64_t origin;
    timestamp64_t receive;
    timestamp64_t transmit;
} NTPUndecodedPacket_t;

NTPClient NTP;

const int seventyYears = 2208988800UL; // From 1900 to 1970

int32_t flipInt32 (int32_t number) {
    uint8_t output[sizeof (int32_t)];
    uint8_t* input = (uint8_t*)&number;
    //DEBUGLOG ("Input number %08X", number);

    for (unsigned int i = 1; i <= sizeof (int32_t); i++) {
        output[i - 1] = input[sizeof (int32_t) - i];
    }

    //DEBUGLOG ("Output number %08X", *(int32_t*)output);
    int32_t *result = (int32_t*)output;
    return *result;
}

int16_t flipInt16 (int16_t number) {
    uint8_t output[sizeof (int16_t)];
    uint8_t* input = (uint8_t*)&number;
    //DEBUGLOG ("Input number %08X", number);

    for (unsigned int i = 1; i <= sizeof (int16_t); i++) {
        output[i - 1] = input[sizeof (int16_t) - i];
    }

    //DEBUGLOG ("Output number %08X", *(int32_t*)output);
    int16_t* result = (int16_t*)output;
    return *result;
}

char* dumpNTPPacket (byte* data, size_t length, char* buffer, int len) {
    int remaining = len - 1;
    int index = 0;
    int written;

    for (size_t i = 0; i < length; i++) {
        if (remaining > 0) {
            written = snprintf (buffer + index, remaining, "%02X ", data[i]);
            index += written;
            remaining -= written;
            if ((i + 1) % 16 == 0) {
                written = snprintf (buffer + index, remaining, "\n");
                index += written;
                remaining -= written;
            } else if ((i + 1) % 4 == 0) {
                written = snprintf (buffer + index, remaining, "| ");
                index += written;
                remaining -= written;
            }
        }
    }
    return buffer;
}

bool NTPClient::begin (const char* ntpServerName) {
    err_t result;
    
    if (!setNtpServerName (ntpServerName) || !strnlen (ntpServerName, SERVER_NAME_LENGTH)) {
        DEBUGLOG ("Invalid NTP server name");
        return false;
    }

    udp = udp_new ();
    if (!udp){
        DEBUGLOG ("Failed to create NTP socket");
        return false;
    }
    
    if (WiFi.isConnected ()) {
        ip_addr_t localAddress;
#ifdef ESP32
        localAddress.u_addr.ip4.addr = WiFi.localIP ();
        localAddress.type = IPADDR_TYPE_V4;
#else
        localAddress.addr = WiFi.localIP ();
#endif
        result = udp_bind (udp, &localAddress, DEFAULT_NTP_PORT);
        if (result) {
            DEBUGLOG ("Failed to bind to NTP port. %d", result);
            return false;
        }

        udp_recv (udp, &NTPClient::s_recvPacket, this);
        isConnected = true;
    }
    lastSyncd.tv_sec = 0;
    lastSyncd.tv_usec = 0;
    
    DEBUGLOG ("Time sync started");
    
    //Start loop and receiver tasks
#ifdef ESP32
    xTaskCreateUniversal (
        &NTPClient::s_getTimeloop, /* Task function. */
        "NTP receiver", /* name of task. */
        2048, /* Stack size of task */
        this, /* parameter of the task */
        1, /* priority of the task */
        &loopHandle, /* Task handle to keep track of created task */
        CONFIG_ARDUINO_RUNNING_CORE);
    
    xTaskCreateUniversal (
        &NTPClient::s_receiverTask, /* Task function. */
        "NTP receiver", /* name of task. */
        3072, /* Stack size of task */
        this, /* parameter of the task */
        1, /* priority of the task */
        NULL, /* Task handle to keep track of created task */
        CONFIG_ARDUINO_RUNNING_CORE);
#else
    loopTimer.attach_ms (ESP8266_LOOP_TASK_INTERVAL, &NTPClient::s_getTimeloop, (void*)this);
    receiverTimer.attach_ms (ESP8266_RECEIVER_TASK_INTERVAL, &NTPClient::s_receiverTask, (void*)this);
#endif
    
    DEBUGLOG ("First time sync request");
    getTime ();
    
    return true;

}

void NTPClient::processPacket (struct pbuf* packet) {
    NTPPacket_t ntpPacket;
    bool offsetApplied = false;
    static bool wasPartial;
    
    if (!packet) {
        DEBUGLOG ("Received packet empty");
    }
    DEBUGLOG ("Data lenght %d", packet->len);

    if (!ntpRequested) {
        DEBUGLOG ("Unrequested response");
        //pbuf_free (packet);
        return;
    }
    
    ntpRequested = false;
    
    if (packet->len < NTP_PACKET_SIZE) {
        DEBUGLOG ("Response Error");
        status = unsyncd;
        DEBUGLOG ("Status set to UNSYNCD");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = responseError;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            event.info.offset = 0;
            event.info.delay = 0;
            onSyncEvent (event);
        }  
        //pbuf_free (packet);
        return;
    }

    responseTimer.detach ();
    
    if (!decodeNtpMessage ((uint8_t*)packet->payload, packet->len, &ntpPacket)){
        DEBUGLOG ("Null pointer packet");
        return;
    }
    timeval tvOffset = calculateOffset (&ntpPacket);
    
    int64_t offset_us = (int64_t)tvOffset.tv_sec * 1000000L + (int64_t)tvOffset.tv_usec;
    offsetSum += offset_us;
    round++;
    offsetAve = offsetSum / round;
    DEBUGLOG ("offset %lld -- sum %lld -- round %u -- average %lld", offset_us, offsetSum, round, offsetAve);
    
    if (round >= numAveRounds) {
        tvOffset.tv_sec = offsetAve / 1000000L;
        tvOffset.tv_usec = offsetAve - tvOffset.tv_sec * 1000000;
        //Serial.printf ("\nResult offset = %ld.%06ld\n\n", tvOffset.tv_sec, tvOffset.tv_usec);
        round = 0;
        offsetSum = 0;
    } else {
        actualInterval = ntpTimeout; // Set retry period equal to timeout
        return;
    }
    
    if (abs (offsetAve) < timeSyncThreshold) {
        DEBUGLOG ("Offset under threshold. Not updating");
        status = syncd;
        numDispersionErrors = 0;
        actualInterval = longInterval;
        numSyncRetry = 0;
        DEBUGLOG ("Offset %0.3f ms is under threshold %ld. Not updating", offsetAve / 1000.0, timeSyncThreshold);
        if (wasPartial){
            wasPartial = false;
            if (onSyncEvent) {
                NTPEvent_t event;
                event.event = timeSyncd;
                event.info.offset = offsetAve / 1000000.0;
                event.info.serverAddress = ntpServerIPAddress;
                event.info.port = DEFAULT_NTP_PORT;
                event.info.delay = delay;
                event.info.dispersion = ntpPacket.dispersion;
                onSyncEvent (event);
            }

        } else {
            if (onSyncEvent) {
                NTPEvent_t event;
                event.event = syncNotNeeded;
                event.info.offset = offsetAve / 1000000.0;
                event.info.dispersion = ntpPacket.dispersion;
                event.info.serverAddress = ntpServerIPAddress;
                event.info.port = DEFAULT_NTP_PORT;
                onSyncEvent (event);
            }
        }
        return;
    }
        
    if (!checkNTPresponse (&ntpPacket, offsetAve)) {
        numDispersionErrors++;
        DEBUGLOG ("Not valid or inaccurate response #%d", numDispersionErrors);
        if (numDispersionErrors > maxDispersionErrors) {
            numDispersionErrors = 0;
            
            if (onSyncEvent) {
                NTPEvent_t event;
                event.event = accuracyError;
                event.info.offset = offsetAve / 1000000.0;
                event.info.dispersion = ntpPacket.dispersion;
                event.info.serverAddress = ntpServerIPAddress;
                event.info.port = DEFAULT_NTP_PORT;
                onSyncEvent (event);
            }
                            
            // if (status == syncd) {
            //     actualInterval = longInterval;
            // } else {
            actualInterval = shortInterval;
            // }
            DEBUGLOG ("Status = %s. Next sync in %d seconds", status == syncd ? "SYNCD" : "UNSYNCD", actualInterval);
        }
        return;
    } else {
        numDispersionErrors = 0;
        DEBUGLOG ("Valid NTP response");
    }

    if (!adjustOffset (&tvOffset)) {
        DEBUGLOG ("Error applying offset");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = syncError;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            event.info.offset = (float)tvOffset.tv_sec + (float)tvOffset.tv_usec / 1000000.0;
            onSyncEvent (event);
        }
    }
    offsetApplied = true;

    if (tvOffset.tv_sec != 0 || abs (tvOffset.tv_usec) > minSyncAccuracyUs) { // Offset bigger than 10 ms
        DEBUGLOG ("Minimum accuracy not reached. Repeating sync");
        if (numSyncRetry < maxNumSyncRetry) {
            status = partialSync;
            numSyncRetry++;
            wasPartial = true;
        } else {
            status = syncd;
            numSyncRetry = 0;
            wasPartial = false;
        }
    } else {
        DEBUGLOG ("Status set to SYNCD");
        DEBUGLOG ("Next sync programmed for %d seconds", getLongInterval ());
        status = syncd;
        numSyncRetry = 0;
        if (wasPartial){
            offsetApplied = true;
        }
        //Serial.printf ("Status: %d wasPartial: %d offsetApplied %d Offset %0.3f\n", status, wasPartial, offsetApplied, ((float)tvOffset.tv_sec + (float)tvOffset.tv_usec / 1000000.0) * 1000);
        wasPartial = false;
    }
    if (status == partialSync) {
        actualInterval = ntpTimeout; //shortInterval;
    } else {
        actualInterval = longInterval;
    }
    DEBUGLOG ("Interval set to = %d", actualInterval);
    DEBUGLOG ("Sync frequency set low");
    DEBUGLOG ("Successful NTP sync at %s", getTimeDateString (getLastNTPSync ()));
    if (!firstSync.tv_sec) {
        firstSync = lastSyncd;
    }
    if (offsetApplied && onSyncEvent) {
        NTPEvent_t event;
        if (status == partialSync){
            event.event = partlySync;
            event.info.retrials = numSyncRetry;
            //event.info.offset = offset;
        } else {
            event.event = timeSyncd;
        }
        event.info.offset = (float)tvOffset.tv_sec + (float)tvOffset.tv_usec / 1000000.0;
        event.info.delay = delay;
        event.info.dispersion = ntpPacket.dispersion;
        event.info.serverAddress = ntpServerIPAddress;
        event.info.port = DEFAULT_NTP_PORT;
        onSyncEvent (event);
    }
}

void NTPClient::s_recvPacket (void* arg, struct udp_pcb* pcb, struct pbuf* p,
                              const ip_addr_t* addr, u16_t port) {
    NTPPacket_t ntpPacket;
    
    NTPClient* self = reinterpret_cast<NTPClient*>(arg);
    gettimeofday (&(self->packetLastReceived), NULL);
    DEBUGLOG ("NTP Packet received from %s:%d", ipaddr_ntoa (addr), port);
    self->lastNtpResponsePacket = p;
    self->responsePacketValid = true;
}

void NTPClient::s_receiverTask (void* arg){
    NTPClient* self = reinterpret_cast<NTPClient*>(arg);
#ifdef ESP32
    for (;;) {
#endif
        if (self->responsePacketValid) {
            self->processPacket (self->lastNtpResponsePacket);
            pbuf_free (self->lastNtpResponsePacket);
            self->responsePacketValid = false;
        }
#ifdef ESP32
        const TickType_t xDelay = 100 / portTICK_PERIOD_MS;
        vTaskDelay (xDelay);
    }
#endif
}

char* NTPClient::getUptimeString () {
    uint16_t days;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;

    time_t uptime = getUptime ();

    seconds = uptime % SECS_PER_MIN;
    uptime -= seconds;
    minutes = (uptime % SECS_PER_HOUR) / SECS_PER_MIN;
    uptime -= minutes * SECS_PER_MIN;
    hours = (uptime % SECS_PER_DAY) / SECS_PER_HOUR;
    uptime -= hours * SECS_PER_HOUR;
    days = uptime / SECS_PER_DAY;

    snprintf (strBuffer, sizeof (strBuffer) - 1, "%4u days %02d:%02d:%02d", days, hours, minutes, seconds);

    return strBuffer;
}

void NTPClient::s_getTimeloop (void* arg) {
#ifdef ESP32
    for (;;) {
#endif // ESP32
        //DEBUGLOG ("Running periodic task");
        NTPClient* self = reinterpret_cast<NTPClient*>(arg);
        static time_t lastGotTime;

        if (self->isConnected) {
            if (WiFi.isConnected ()) {
                if (::millis () - lastGotTime >= self->actualInterval) {
                    lastGotTime = ::millis ();
                    DEBUGLOG ("Periodic loop. Millis = %d", lastGotTime);
                    self->getTime ();
                }
            } else {
                DEBUGLOG ("DISCONNECTED");
                udp_remove (self->udp);
                self->isConnected = false;
            }
        } else {
            if (WiFi.isConnected ()) {
                DEBUGLOG ("CONNECTED. Binding");

                self->udp = udp_new ();
                if (!self->udp) {
                    DEBUGLOG ("Failed to create NTP socket");
                    return; // false;
                }

                ip_addr_t localAddress;
#ifdef ESP32
                localAddress.u_addr.ip4.addr = WiFi.localIP ();
                localAddress.type = IPADDR_TYPE_V4;
#else // ESP8266
                localAddress.addr = WiFi.localIP ();
#endif // ESP32
                err_t result = udp_bind (self->udp, &localAddress, DEFAULT_NTP_PORT);
                if (result) {
                    DEBUGLOG ("Failed to bind to NTP port. %d", result);
                    //return; //false;
                }

                udp_recv (self->udp, &NTPClient::s_recvPacket, self);
                self->getTime ();
                self->isConnected = true;
            }
        }
#ifdef ESP32
        const TickType_t xDelay = 100 / portTICK_PERIOD_MS;
        vTaskDelay (xDelay);
    }
#endif // ESP32
}

void NTPClient::getTime () {
    err_t result;
    
    result = WiFi.hostByName (getNtpServerName (), ntpServerIPAddress);
    if (!result) {
        DEBUGLOG ("HostByName error %d", (int)result);

        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidAddress;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;

            onSyncEvent (event);        
        }
    }
    if (ntpServerIPAddress == IPAddress(INADDR_NONE)) {
        DEBUGLOG ("IP address unset. Aborting");
        actualInterval = shortInterval;
        DEBUGLOG ("Set interval to = %d", actualInterval);
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidAddress;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
        return;
    }
    
    ip_addr ntpAddr;
#ifdef ESP32
    ntpAddr.type = IPADDR_TYPE_V4;
    ntpAddr.u_addr.ip4.addr = ntpServerIPAddress;
#else
    ntpAddr.addr = ntpServerIPAddress;
#endif
    DEBUGLOG ("NTP server IP address %s", ipaddr_ntoa (&ntpAddr));
    result = udp_connect (udp, &ntpAddr, DEFAULT_NTP_PORT);
    if (result == ERR_USE) {
        DEBUGLOG ("Port already used");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidPort;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
    }
    if (result == ERR_RTE) {
        DEBUGLOG ("Port already used");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidAddress;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
    }
    
    DEBUGLOG ("Sending UDP packet");
    NTPStatus_t prevStatus = status;
    ntpRequested = true;
    DEBUGLOG ("Status set to REQUESTED");
    responseTimer.once_ms (ntpTimeout, &NTPClient::s_processRequestTimeout, static_cast<void*>(this));
    
    if (!sendNTPpacket ()) {
        responseTimer.detach ();
        DEBUGLOG ("NTP request error");
        status = prevStatus;
        DEBUGLOG ("Status recovered due to UDP send error");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = errorSending;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
        return;
    }
    if (onSyncEvent) {
        NTPEvent_t event;
        event.event = requestSent;
        event.info.serverAddress = ntpServerIPAddress;
        event.info.port = DEFAULT_NTP_PORT;
        onSyncEvent (event);
    }
    //udp_disconnect (udp);
    
}

boolean NTPClient::sendNTPpacket () {
    err_t result;
    timeval currentime;
    pbuf* buffer;
    NTPUndecodedPacket_t packet;
    buffer = pbuf_alloc (PBUF_TRANSPORT, sizeof (NTPUndecodedPacket_t), PBUF_RAM);
    if (!buffer){
        DEBUGLOG ("Cannot allocate UDP packet buffer");
        return false;
    }
    buffer->len = sizeof (NTPUndecodedPacket_t);
    buffer->tot_len = sizeof (NTPUndecodedPacket_t);

    memset (&packet, 0, sizeof (NTPUndecodedPacket_t));
    
    packet.flags = 0b11100011;
    packet.peerStratum = 0;
    packet.pollingInterval = 6;
    packet.clockPrecission = 0xEC; // 1 us

    gettimeofday (&currentime, NULL);
    
    DEBUGLOG ("sendNTPpacket");
    
    if (currentime.tv_sec != 0) {
        packet.transmit.secondsOffset = flipInt32 (currentime.tv_sec + seventyYears);
        DEBUGLOG ("Current time: %ld.%ld", currentime.tv_sec, currentime.tv_usec);
        uint32_t timestamp_us = (uint32_t)((double)(currentime.tv_usec) / 1000000.0 * (double)0x100000000);
        DEBUGLOG ("timestamp_us = 0x%08X %lu", timestamp_us, timestamp_us);
        packet.transmit.fraction = flipInt32 (timestamp_us);
        DEBUGLOG ("Transmit: 0x%08X : 0x%08X", packet.transmit.secondsOffset, packet.transmit.fraction);
        
    } else {
        packet.transmit.secondsOffset = 0;
        packet.transmit.fraction = 0;
    }

#ifdef DEBUG_NTPCLIENT
    const int sizeStr = 200;
    char strPacketBuffer[sizeStr];
    DEBUGLOG ("NTP Packet\n%s", dumpNTPPacket ((uint8_t*)&packet, sizeof (NTPUndecodedPacket_t), strPacketBuffer, sizeStr));
#endif

    DEBUGLOG ("Sendign packet");
    memcpy (buffer->payload, &packet, sizeof (NTPUndecodedPacket_t));
    result = udp_send (udp, buffer);
    pbuf_free (buffer);
    if (result == ERR_OK) {
        DEBUGLOG ("UDP packet sent");
        return true;
    } else {
        return false;
    }
}

void ICACHE_RAM_ATTR NTPClient::s_processRequestTimeout (void* arg) {
    NTPClient* self = reinterpret_cast<NTPClient*>(arg);
    self->processRequestTimeout ();
}

void ICACHE_RAM_ATTR NTPClient::processRequestTimeout () {
    status = unsyncd;
    DEBUGLOG ("Status set to UNSYNCD");
    ntpRequested = false;
    responseTimer.detach ();
    DEBUGLOG ("NTP response Timeout");
    if (onSyncEvent) {
        NTPEvent_t event;
        event.event = noResponse;
        event.info.serverAddress = ntpServerIPAddress;
        event.info.port = DEFAULT_NTP_PORT;
        onSyncEvent (event);
    }
    // if (status==syncd) {
    //     actualInterval = longInterval;
    // } else {
    //     actualInterval = shortInterval;
    // }
    //DEBUGLOG ("Set interval to = %d", actualInterval);
}

bool NTPClient::setNtpServerName (const char* serverName) {
    if (!serverName) {
        return false;
    }
    if (!strlen (serverName)) {
        return false;
    }
    DEBUGLOG ("NTP server set to %s\n", serverName);
    memset (ntpServerName, 0, SERVER_NAME_LENGTH);
    strncpy (ntpServerName, serverName, strnlen (serverName, SERVER_NAME_LENGTH));
    return true;
}

bool NTPClient::setInterval (int interval) {
    unsigned int newInterval = interval * 1000;
    if (interval >= MIN_NTP_INTERVAL) {
        if (longInterval != newInterval) {
            longInterval = newInterval;
            DEBUGLOG ("Sync interval set to %d s", interval);
            if (syncStatus () == syncd) {
                actualInterval = longInterval;
                DEBUGLOG ("Set interval to = %d", actualInterval);
            }
        }
        return true;
    } else {
        longInterval = MIN_NTP_INTERVAL * 1000;
        if (syncStatus () == syncd) {
            actualInterval = longInterval;
            DEBUGLOG ("Set interval to = %d", actualInterval);
        }
        DEBUGLOG ("Too low value. Sync interval set to minimum: %d s", MIN_NTP_INTERVAL);
        return false;
    }
}

bool NTPClient::setInterval (int shortInterval, int longInterval) {
    int newShortInterval = shortInterval * 1000;
    int newLongInterval = longInterval * 1000;
    if (shortInterval >= MIN_NTP_INTERVAL && longInterval >= MIN_NTP_INTERVAL) {
        this->shortInterval = newShortInterval;
        this->longInterval = newLongInterval;
        if (syncStatus () != syncd) {
            actualInterval = this->shortInterval;

        } else {
            actualInterval = this->longInterval;
        }
        DEBUGLOG ("Interval set to = %d", actualInterval);
        DEBUGLOG ("Short sync interval set to %d s\n", shortInterval);
        DEBUGLOG ("Long sync interval set to %d s\n", longInterval);
return true;
    } else {
        DEBUGLOG ("Too low interval values");
        return false;    
    }
}


bool NTPClient::setNTPTimeout (uint16_t milliseconds) {

    if (milliseconds >= MIN_NTP_TIMEOUT) {
        ntpTimeout = milliseconds;
        DEBUGLOG ("Set NTP timeout to %u ms", milliseconds);
        return true;
    }
    DEBUGLOG ("NTP timeout should be higher than %u ms. You've tried to set %u ms", MIN_NTP_TIMEOUT, milliseconds);
    return false;

}

void NTPClient::dumpNtpPacketInfo (NTPPacket_t* decPacket) {
    Serial.print ("------ Decoded NTP message -------\n");
    Serial.printf ("LI = %u\n", decPacket->flags.li);
    Serial.printf ("Version = %u\n", decPacket->flags.vers);
    Serial.printf ("Mode = %u\n", decPacket->flags.mode);
    Serial.printf ("Peer Stratum = %u\n", decPacket->peerStratum);
    Serial.printf ("Polling Interval = %u s\n", decPacket->pollingInterval);
    Serial.printf ("Clock Precission = %0.3f us\n", decPacket->clockPrecission * 1000000.0);
    Serial.printf ("Root delay: %0.3f ms\n", decPacket->rootDelay * 1000.0);
    Serial.printf ("Dispersion: %0.3f ms\n", decPacket->dispersion * 1000.0);
    if (decPacket->peerStratum > 1) {
        Serial.printf ("refID: %u.%u.%u.%u\n", decPacket->refID[0], decPacket->refID[1], decPacket->refID[2], decPacket->refID[3]);
    } else {
        Serial.printf ("refID: %.*s\n", 4, (char*)(decPacket->refID));
    }
    Serial.printf ("Reference: %s\n", getTimeDateString (decPacket->reference));
    Serial.printf ("Origin: %s\n", getTimeDateString (decPacket->origin));
    Serial.printf ("Receive: %s\n", getTimeDateString (decPacket->receive));
    Serial.printf ("Transmit: %s\n", getTimeDateString (decPacket->transmit));
}

NTPPacket_t* NTPClient::decodeNtpMessage (uint8_t* messageBuffer, size_t length, NTPPacket_t* decPacket) {
    NTPUndecodedPacket_t recPacket;
    int32_t timestamp_s;
    uint32_t timestamp_us;

    if (length < NTP_PACKET_SIZE) {
        return NULL;
    }

    memcpy (&recPacket, messageBuffer, NTP_PACKET_SIZE);

    DEBUGLOG ("Decoded NTP message");
#ifdef DEBUG_NTPCLIENT
    char buffer[250];
#endif
    DEBUGLOG ("\n%s", dumpNTPPacket (messageBuffer, length, buffer, 250));

    decPacket->flags.li = recPacket.flags >> 6;
    DEBUGLOG ("LI = %u", decPacket->flags.li);

    decPacket->flags.vers = recPacket.flags >> 3 & 0b111;
    DEBUGLOG ("Version = %u", decPacket->flags.vers);

    decPacket->flags.mode = recPacket.flags & 0b111;
    DEBUGLOG ("Mode = %u", decPacket->flags.mode);

    decPacket->peerStratum = recPacket.peerStratum;
    DEBUGLOG ("Peer Stratum = %u", decPacket->peerStratum);

    decPacket->pollingInterval = pow(2, recPacket.pollingInterval);
    DEBUGLOG ("Polling Interval = %u", decPacket->pollingInterval);

    decPacket->clockPrecission = pow(2,recPacket.clockPrecission);
    DEBUGLOG ("Clock Precission = %0.3f us", decPacket->clockPrecission * 1000000);

    int16_t ts16_s = flipInt16 (recPacket.rootDelay.secondsOffset);
    uint16_t ts16_us = flipInt16 (recPacket.rootDelay.fraction);
    decPacket->rootDelay = (float)ts16_s + (float)ts16_us / (float)0x10000;
    DEBUGLOG ("Root delay: 0x%08X", recPacket.rootDelay);
    DEBUGLOG ("Root delay: %0.3f ms", decPacket->rootDelay * 1000);

    ts16_s = flipInt16 (recPacket.dispersion.secondsOffset);
    ts16_us = flipInt16 (recPacket.dispersion.fraction);
    decPacket->dispersion = (float)ts16_s + (float)ts16_us / (float)0x10000;
    DEBUGLOG ("Dispersion: 0x%08X", recPacket.dispersion);
    DEBUGLOG ("Dispersion: %0.3f ms", decPacket->dispersion * 1000);

    memcpy (&(decPacket->refID), &(recPacket.refID), 4);
    if (decPacket->peerStratum > 1) {
        DEBUGLOG ("refID: %u.%u.%u.%u", decPacket->refID[0], decPacket->refID[1], decPacket->refID[2], decPacket->refID[3]);
    } else {
        DEBUGLOG ("refID: %.*s", 4, (char*)(decPacket->refID));
    }

    // Reference timestamp
    timestamp_s = flipInt32 (recPacket.reference.secondsOffset);
    timestamp_us = flipInt32 (recPacket.reference.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);
    if (timestamp_s) {
        decPacket->reference.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->reference.tv_sec = 0;
    }
    decPacket->reference.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Reference: seconds %08X fraction %08X", recPacket.reference.secondsOffset, recPacket.reference.fraction);
    //DEBUGLOG ("Reference: %d.%06ld", decPacket->reference.tv_sec, decPacket->reference.tv_usec);
    DEBUGLOG ("Reference: %s.%06ld", ctime (&(decPacket->reference.tv_sec)), decPacket->reference.tv_usec);

    // Origin timestamp
    timestamp_s = flipInt32 (recPacket.origin.secondsOffset);
    timestamp_us = flipInt32 (recPacket.origin.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);
    if (timestamp_s) {
        decPacket->origin.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->origin.tv_sec = 0;
    }
    decPacket->origin.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Origin: seconds %08X fraction %08X", recPacket.origin.secondsOffset, recPacket.origin.fraction);
    //DEBUGLOG ("Origin: %d.%06ld", decPacket->origin.tv_sec, decPacket->origin.tv_usec);
    DEBUGLOG ("Origin: %s.%06ld", ctime (&(decPacket->origin.tv_sec)), decPacket->origin.tv_usec);

    // Receive timestamp
    timestamp_s = flipInt32 (recPacket.receive.secondsOffset);
    timestamp_us = flipInt32 (recPacket.receive.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);
    if (timestamp_s) {
        decPacket->receive.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->receive.tv_sec = 0;
    }
    decPacket->receive.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Receive: seconds %08X fraction %08X", recPacket.receive.secondsOffset, recPacket.receive.fraction);
    //DEBUGLOG ("Receive: %d.%06ld", decPacket->receive.tv_sec, decPacket->receive.tv_usec);
    DEBUGLOG ("Receive: %s.%06ld", ctime (&(decPacket->receive.tv_sec)), decPacket->receive.tv_usec);

    // Transmit timestamp
    timestamp_s = flipInt32 (recPacket.transmit.secondsOffset);
    timestamp_us = flipInt32 (recPacket.transmit.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);

    if (timestamp_s) {
        decPacket->transmit.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->transmit.tv_sec = 0;
    }
    decPacket->transmit.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Transmit: seconds %08X fraction %08X", recPacket.transmit.secondsOffset, recPacket.transmit.fraction);
    //DEBUGLOG ("Transmit: %d.%06ld", decPacket->transmit.tv_sec, decPacket->transmit.tv_usec);
    DEBUGLOG ("Transmit: %s.%06ld", ctime (&(decPacket->transmit.tv_sec)), decPacket->transmit.tv_usec);
    
    decPacket->destination = packetLastReceived;

    return decPacket;
}

bool NTPClient::checkNTPresponse (NTPPacket_t* ntpPacket, int64_t offsetUs) {
    //dumpNtpPacketInfo (ntpPacket);
    if (ntpPacket->flags.li!=0){
        DEBUGLOG ("Leap indicator error: %d", ntpPacket->flags.li);
        return false;
    }
    
    if (ntpPacket->flags.vers != 4) {
        DEBUGLOG ("NTP version error: %d", ntpPacket->flags.vers);
        return false;
    }

    if (ntpPacket->flags.mode != 4) {
        DEBUGLOG ("NTP mode error: %d", ntpPacket->flags.mode);
        return false;
    }
    
    if (ntpPacket->peerStratum < 1 || ntpPacket->peerStratum > 15) {
        DEBUGLOG ("Peer stratum error: %d", ntpPacket->peerStratum);
        return false;
    }

    if (status == syncd || status == partialSync) {
        //Serial.printf ("Peer precission:   %0.9f s\n", ntpPacket->clockPrecission);
        //Serial.printf ("minSyncAccuracyUs: %0.9f s\n", minSyncAccuracyUs / 10000000.0);
        if (ntpPacket->clockPrecission > (float)(minSyncAccuracyUs / 10000000.0)/* || ntpPacket->clockPrecission == 0.0*/) { // 5 zeroes, that's correct. us*1000000 / 10
            DEBUGLOG ("Peer precission error: %0.3f us > minSyncAccuracyUs/10 %0.3f", ntpPacket->clockPrecission * 1000000.0, minSyncAccuracyUs / 10.0);
            return false;
        }

        //Serial.printf ("Dispersion:        %0.6f s\n", ntpPacket->dispersion);
        //Serial.printf ("Offset:            %0.6f s\n", offsetUs / 1000000.0);
        if (ntpPacket->dispersion > abs(offsetUs / 1000000.0) || ntpPacket->dispersion == 0.0) {
            DEBUGLOG ("Dispersion error: %0.3f ms > Offset: %0.3f ms", ntpPacket->dispersion * 1000.0, (float)(offsetUs / 1000.0));
            return false;
        }    
    }

    return true;
}

timeval NTPClient::calculateOffset (NTPPacket_t* ntpPacket) {
    double t1, t2, t3, t4;
    timeval tv_offset;

    t1 = ntpPacket->origin.tv_sec + ntpPacket->origin.tv_usec / 1000000.0;
    t2 = ntpPacket->receive.tv_sec + ntpPacket->receive.tv_usec / 1000000.0;
    t3 = ntpPacket->transmit.tv_sec + ntpPacket->transmit.tv_usec / 1000000.0;
    t4 = ntpPacket->destination.tv_sec + ntpPacket->destination.tv_usec / 1000000.0;
    offset = ((t2 - t1) / 2.0 + (t3 - t4) / 2.0); // in seconds
    delay = (t4 - t1) - (t3 - t2); // in seconds

    DEBUGLOG ("T1: %f T2: %f T3: %f T4: %f", t1, t2, t3, t4);
    DEBUGLOG ("T1: %s", getTimeDateString (ntpPacket->origin));
    //DEBUGLOG ("T1: %016X  %016X", ntpPacket->origin.tv_sec, ntpPacket->origin.tv_usec);
    DEBUGLOG ("T2: %s", getTimeDateString (ntpPacket->receive));
    //DEBUGLOG ("T2: %016X  %016X", ntpPacket->receive.tv_sec, ntpPacket->receive.tv_usec);
    DEBUGLOG ("T3: %s", getTimeDateString (ntpPacket->transmit));
    //DEBUGLOG ("T3: %016X  %016X", ntpPacket->transmit.tv_sec, ntpPacket->transmit.tv_usec);
    DEBUGLOG ("T4: %s", getTimeDateString (ntpPacket->destination));
    //DEBUGLOG ("T4: %016X  %016X", ntpPacket->destination.tv_sec, ntpPacket->destination.tv_usec);
    DEBUGLOG ("Offset: %f, Delay: %f", offset, delay);

    tv_offset.tv_sec = (time_t)offset;
    tv_offset.tv_usec = (offset - (double)tv_offset.tv_sec) * 1000000.0;

    DEBUGLOG ("Calculated offset %f sec. Delay %f ms", offset, delay * 1000);

    return tv_offset;
}

bool NTPClient::adjustOffset (timeval* offset) {
    timeval newtime;
    timeval currenttime;

    gettimeofday (&currenttime, NULL);

    int64_t currenttime_us = (int64_t)currenttime.tv_sec * 1000000L + (int64_t)currenttime.tv_usec;
    int64_t offset_us = (int64_t)offset->tv_sec * 1000000L + (int64_t)offset->tv_usec;

    // Serial.printf ("currenttime  %ld.%ld\n", currenttime.tv_sec, currenttime.tv_usec);
    // Serial.printf ("currenttime_us: %f\n", currenttime_us);
    // Serial.printf ("offset  %ld.%ld\n", offset->tv_sec, offset->tv_usec);
    // Serial.printf ("offset_us: %f\n", offset_us);

    int64_t newtime_us = currenttime_us + offset_us;

    newtime.tv_sec = newtime_us / 1000000L;
    newtime.tv_usec = newtime_us - ((int64_t)newtime.tv_sec * 1000000L);
    // Serial.printf ("newtime_us: %lld\n", newtime_us);
    // Serial.printf ("newtime  %ld.%ld\n", newtime.tv_sec, newtime.tv_usec);
    
    // if (_offset.tv_usec > 0) {
    //     timeradd (&currenttime, &_offset, &newtime);
    // } else {
    //     _offset.tv_usec = -_offset.tv_usec;
    //     timersub (&currenttime, &_offset, &newtime);
    // }

    if (settimeofday (&newtime, NULL)) { // hard adjustment
        return false;
    }
    //Serial.printf ("millis() offset 1: %lld\n", currenttime_us / 1000 - millis ());
    //Serial.printf ("millis() offset 2: %lld\n", newtime_us / 1000 - millis ());
    DEBUGLOG ("Offset: %lld", (newtime_us - currenttime_us));
    //Serial.printf ("Requested offset %ld.%ld\n", offset->tv_sec, offset->tv_usec);
    //Serial.printf ("Requested new time %ld.%ld\n", newtime.tv_sec, newtime.tv_usec);
    //Serial.printf ("Requested new time %s\n", ctime (&(newtime.tv_sec)));

    DEBUGLOG ("Hard adjust");

    lastSyncd = newtime;
    DEBUGLOG ("Offset adjusted");
    return true;
}

char* NTPClient::ntpEvent2str (NTPEvent_t e) {
    const int resultMaxSize = 150;
    static char result[resultMaxSize];
    switch (e.event) {
    case timeSyncd:
        snprintf (result, resultMaxSize, "%d:    Got NTP time %s from %s:%u. Offset: %0.3f ms. Delay: %0.3f ms. Dispersion: %0.3f ms",
                  e.event,
                  NTP.getTimeDateStringUs (),
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port,
                  e.info.offset * 1000,
                  e.info.delay * 1000,
                  e.info.dispersion * 1000);
        break;
    case noResponse:
        snprintf (result, resultMaxSize, "%d:   No response from NTP server %s:%u",
                  e.event,
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port);
        break;
    case invalidAddress:
        snprintf (result, resultMaxSize, "%d:   Invalid address %s",
                  e.event,
                  e.info.serverAddress.toString ().c_str ());
        break;
    case invalidPort:
        snprintf (result, resultMaxSize, "%d:   Invalid port %u",
                  e.event,
                  e.info.port);
        break;
    case requestSent:
        snprintf (result, resultMaxSize, "%d:    NTP request sent to %s:%u",
                  e.event,
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port);
        break;
    case partlySync:
        snprintf (result, resultMaxSize, "%d: #%u Partial sync %s from %s:%u. Offset: %0.3f ms. Delay: %0.3f ms. Dispersion: %0.3f ms",
                  e.event,
                  e.info.retrials,
                  NTP.getTimeDateStringUs (),
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port,
                  e.info.offset * 1000,
                  e.info.delay * 1000,
                  e.info.dispersion * 1000);
        break;
    case accuracyError:
        snprintf (result, resultMaxSize, "%d:   Accuracy error from %s:%u. Offset: %0.3f ms. Dispersion: %0.3f ms",
                  e.event,
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port,
                  e.info.offset * 1000,
                  e.info.dispersion * 1000);
        break;
    case syncNotNeeded:
        snprintf (result, resultMaxSize, "%d:    Sync not needed from %s:%u. Offset: %0.3f ms. Dispersion: %0.3f ms",
                  e.event,
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port,
                  e.info.offset * 1000,
                  e.info.dispersion * 1000);
        break;
    case errorSending:
        snprintf (result, resultMaxSize, "%d:   Error sending NTP request", e.event);
        break;
    case responseError:
        snprintf (result, resultMaxSize, "%d:   NTP response error from %s:%u",
                  e.event,
                  e.info.serverAddress.toString ().c_str (),
                  e.info.port);
        break;
    case syncError:
        snprintf (result, resultMaxSize, "%d:   Error applying sync", e.event);
        break;
    default:
        snprintf (result, resultMaxSize, "%d:   Unknown error", e.event);
    }

    return result;
}
