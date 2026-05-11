#pragma once
#include <Arduino.h>

// Wraps a libssh2 password-authenticated interactive shell session.
// connect() is blocking; send/receive are non-blocking after connect().
class SSHSession {
public:
    // Returns true on success. Leaves non-blocking mode active.
    bool connect(const char* host, int port,
                 const char* user, const char* pass,
                 char* errBuf, size_t errLen);

    void disconnect();

    // Returns bytes sent, or -1 on error
    int  send(const char* data, size_t len);

    // Returns bytes read into buf, 0 if no data, -1 on error/EOF
    int  receive(char* buf, size_t maxLen);

    bool isConnected() const { return _channel != nullptr; }

    // Resize the remote PTY (call after terminal dimensions change)
    void resizePty(int cols, int rows);

private:
    int              _sock    = -1;
    void*            _session = nullptr;   // LIBSSH2_SESSION*
    void*            _channel = nullptr;   // LIBSSH2_CHANNEL*
};

extern SSHSession SSH;
