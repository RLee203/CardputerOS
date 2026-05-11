#pragma once
#include <Arduino.h>

enum class WifiState { IDLE, CONNECTING, CONNECTED, FAILED };

struct WifiNet {
    String ssid;
    String pass;
};

class WifiManager {
public:
    static constexpr int MAX_NETS = 5;

    bool      begin();                                           // load saved networks from flash
    WifiState connect(int idx);                                  // connect to saved network by index
    WifiState connect(const String& ssid, const String& pass);  // connect to arbitrary network (blocking)
    void      addNet(const String& ssid, const String& pass);   // add/update network and save to flash
    void      removeNet(int idx);                               // forget network and save to flash
    void      disconnect();                                     // disconnect WiFi (keeps saved list)

    int            netCount() const { return _count; }
    const WifiNet& net(int i)  const { return _nets[i]; }
    String         localIP()   const;
    WifiState      state()     const;

private:
    WifiNet _nets[MAX_NETS];
    int     _count = 0;
    bool    save();
};

extern WifiManager WifiMgr;
