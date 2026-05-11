#include "ssh_session.h"
#include "config.h"
#include <libssh/libssh.h>

SSHSession SSH;

bool SSHSession::connect(const char* host, int port,
                         const char* user, const char* pass,
                         char* errBuf, size_t errLen) {
    disconnect();

    ssh_session sess = ssh_new();
    if (!sess) { snprintf(errBuf, errLen, "ssh_new() failed"); return false; }

    ssh_options_set(sess, SSH_OPTIONS_HOST, host);
    ssh_options_set(sess, SSH_OPTIONS_PORT, &port);
    ssh_options_set(sess, SSH_OPTIONS_USER, user);

    if (ssh_connect(sess) != SSH_OK) {
        snprintf(errBuf, errLen, "Connect: %s", ssh_get_error(sess));
        ssh_free(sess);
        return false;
    }

    if (ssh_userauth_password(sess, nullptr, pass) != SSH_AUTH_SUCCESS) {
        snprintf(errBuf, errLen, "Auth: %s", ssh_get_error(sess));
        ssh_disconnect(sess); ssh_free(sess);
        return false;
    }

    ssh_channel chan = ssh_channel_new(sess);
    if (!chan || ssh_channel_open_session(chan) != SSH_OK) {
        snprintf(errBuf, errLen, "channel: %s", ssh_get_error(sess));
        if (chan) ssh_channel_free(chan);
        ssh_disconnect(sess); ssh_free(sess);
        return false;
    }

    ssh_channel_request_pty_size(chan, "xterm", TERM_COLS, TERM_ROWS);

    if (ssh_channel_request_shell(chan) != SSH_OK) {
        snprintf(errBuf, errLen, "shell: %s", ssh_get_error(sess));
        ssh_channel_close(chan); ssh_channel_free(chan);
        ssh_disconnect(sess); ssh_free(sess);
        return false;
    }

    ssh_channel_set_blocking(chan, 0);

    _session = sess;
    _channel = chan;
    return true;
}

void SSHSession::disconnect() {
    if (_channel) {
        auto chan = (ssh_channel)_channel;
        ssh_channel_send_eof(chan);
        ssh_channel_close(chan);
        ssh_channel_free(chan);
        _channel = nullptr;
    }
    if (_session) {
        auto sess = (ssh_session)_session;
        ssh_disconnect(sess);
        ssh_free(sess);
        _session = nullptr;
    }
    _sock = -1;
}

int SSHSession::send(const char* data, size_t len) {
    if (!_channel) return -1;
    return ssh_channel_write((ssh_channel)_channel, data, (uint32_t)len);
}

int SSHSession::receive(char* buf, size_t maxLen) {
    if (!_channel) return -1;
    auto chan = (ssh_channel)_channel;
    if (ssh_channel_is_eof(chan) || !ssh_channel_is_open(chan)) return -1;
    int rc = ssh_channel_read_nonblocking(chan, buf, (uint32_t)maxLen, 0);
    return (rc == SSH_ERROR) ? -1 : rc;
}

void SSHSession::resizePty(int cols, int rows) {
    if (_channel)
        ssh_channel_change_pty_size((ssh_channel)_channel, cols, rows);
}
