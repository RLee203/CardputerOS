#pragma once
#include <Arduino.h>
#include "config.h"

struct Profile {
    String name;
    String host;
    int    port;
    String user;
    String pass;
};

class ProfileManager {
public:
    bool   begin();                    // mount FS, load profiles
    int    count()  const;
    const Profile& get(int idx) const;

    bool   add(const Profile& p);
    bool   remove(int idx);
    bool   save();

private:
    Profile _profiles[MAX_PROFILES];
    int     _count = 0;

    bool load();
};

extern ProfileManager Profiles;
