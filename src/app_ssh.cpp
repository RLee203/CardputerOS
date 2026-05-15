#include "app_ssh.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include "terminal.h"
#include "profiles.h"
#include "wifi_mgr.h"
#include "ssh_session.h"
#include <M5Cardputer.h>

extern Terminal term;

enum class SshState {
    PROFILE_LIST, PROFILE_NEW, PROFILE_DELETE_CONFIRM,
    SSH_CONNECTING, TERMINAL
};

static SshState sshState  = SshState::PROFILE_LIST;
static bool     sshDirty  = true;

static int    profSel   = 0;
static int    delTarget = -1;

// Simplified form: user@host, Port, Password
static const int FORM_FIELDS = 3;
static const char* formLabels[FORM_FIELDS] = { "user@host", "Port", "Pass" };
static String formValues[FORM_FIELDS];
static int    formField = 0;

static char   sshErr[80];
static String termInput;
static int    termCursor = 0;
static bool   passMode   = false;   // echoing * for password prompts
static int    passStars  = 0;       // how many * shown so far

static void setSshState(SshState s) {
    sshState = s;
    sshDirty = true;
}

// ── Helpers ────────────────────────────────────────────────────────────────

static void drawBanner(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setTextColor(C_FG, C_BG);
    d.drawRect(0, 0, SCREEN_W, SCREEN_H, C_FG);
    int tw = strlen(title) * FONT_W;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setCursor((SCREEN_W - tw) / 2, 6);
    d.print(title);
    d.drawFastHLine(0, 14, SCREEN_W, C_FG);
}

static void drawListItem(int y, const char* text, bool selected) {
    auto& d = M5Cardputer.Display;
    if (selected) {
        d.fillRect(1, y, SCREEN_W - 2, FONT_H + 2, C_HIGHLIGHT);
        d.setTextColor(C_INPUT, C_HIGHLIGHT);
    } else {
        d.fillRect(1, y, SCREEN_W - 2, FONT_H + 2, C_BG);
        d.setTextColor(C_FG, C_BG);
    }
    d.setCursor(4, y + 1);
    d.print(text);
}

// ── Profile list ───────────────────────────────────────────────────────────

static void renderProfileList() {
    drawBanner("[ SSH Profiles ]");
    auto& d = M5Cardputer.Display;
    int y = 18;
    int n = Profiles.count();
    if (n == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, y);
        d.print("(no profiles — press Enter)");
        y += FONT_H + 4;
    }
    for (int i = 0; i < n; i++) {
        const auto& p = Profiles.get(i);
        char buf[42];
        snprintf(buf, sizeof(buf), "%s@%s:%d", p.user.c_str(), p.host.c_str(), p.port);
        drawListItem(y, buf, i == profSel);
        y += FONT_H + 4;
    }
    drawListItem(y, "[+ New Connection]", profSel == n);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Ent=conn fn+D=del fn+Q=home");
}

static void handleProfileList() {
    if (sshDirty) { renderProfileList(); sshDirty = false; }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) { goHome(); return; }

    int total = Profiles.count() + 1;
    if (ev.up)   { if (profSel > 0)        { profSel--; sshDirty = true; } }
    if (ev.down) { if (profSel < total - 1){ profSel++; sshDirty = true; } }

    if (ev.enter) {
        if (profSel == Profiles.count()) {
            formValues[0] = "";
            formValues[1] = "22";
            formValues[2] = "";
            formField = 0;
            setSshState(SshState::PROFILE_NEW);
        } else {
            setSshState(SshState::SSH_CONNECTING);
        }
    }

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if ((c == 'd' || c == 'D') && Profiles.count() > 0 && profSel < Profiles.count()) {
                delTarget = profSel;
                setSshState(SshState::PROFILE_DELETE_CONFIRM);
                return;
            }
            if (c == 'q' || c == 'Q') { goHome(); return; }
        }
    }
}

// ── Delete confirm ─────────────────────────────────────────────────────────

static void handleProfileDeleteConfirm() {
    auto& d = M5Cardputer.Display;
    if (sshDirty) {
        drawBanner("[ Delete Profile ]");
        char buf[64];
        snprintf(buf, sizeof(buf), "Delete \"%s\"?", Profiles.get(delTarget).name.c_str());
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, 22); d.print(buf);
        d.setTextColor(C_FG, C_BG);
        d.setCursor(4, 40); d.print("Enter=YES   Fn+Q=No");
        sshDirty = false;
    }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.enter) {
        Profiles.remove(delTarget);
        if (profSel >= Profiles.count() && profSel > 0) profSel--;
        setSshState(SshState::PROFILE_LIST);
    }
    if (ev.fnKey) {
        for (char c : ev.chars)
            if (c == 'q' || c == 'Q') { setSshState(SshState::PROFILE_LIST); return; }
    }
}

// ── New connection form (simplified: user@host / port / pass) ──────────────

static void renderNewProfileForm() {
    drawBanner("[ New Connection ]");
    auto& d = M5Cardputer.Display;
    int y = 18;
    for (int i = 0; i < FORM_FIELDS; i++) {
        bool active = (i == formField);
        bool isPass = (i == 2);
        String display = isPass ? String(formValues[i].length(), '*') : formValues[i];
        char line[48];
        snprintf(line, sizeof(line), "%-8s: %-28s", formLabels[i], display.c_str());
        if (active) {
            d.fillRect(0, y - 1, SCREEN_W, FONT_H + 2, C_HIGHLIGHT);
            d.setTextColor(C_INPUT, C_HIGHLIGHT);
        } else {
            d.fillRect(0, y - 1, SCREEN_W, FONT_H + 2, C_BG);
            d.setTextColor(C_DIM, C_BG);
        }
        d.setCursor(2, y);
        d.print(line);
        y += FONT_H + 6;
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Tab=next  Enter=connect  Fn+Q=back");
}

static void handleNewProfile() {
    if (sshDirty) { renderNewProfileForm(); sshDirty = false; }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) { setSshState(SshState::PROFILE_LIST); return; }

    auto& cur = formValues[formField];

    if (ev.del && cur.length() > 0) { cur.remove(cur.length() - 1); sshDirty = true; }
    for (char c : ev.chars) { cur += c; sshDirty = true; }

    if (ev.up && formField > 0) { formField--; sshDirty = true; return; }

    if (ev.tab || (ev.enter && formField < FORM_FIELDS - 1)) {
        formField++;
        sshDirty = true;
        return;
    }

    if (ev.enter && formField == FORM_FIELDS - 1) {
        // Parse user@host
        String uathost = formValues[0];
        int at = uathost.indexOf('@');
        String user = (at >= 0) ? uathost.substring(0, at) : "root";
        String host = (at >= 0) ? uathost.substring(at + 1) : uathost;
        if (host.length() == 0) return;

        Profile p;
        p.user = user;
        p.host = host;
        p.port = formValues[1].toInt();
        if (p.port <= 0) p.port = SSH_DEFAULT_PORT;
        p.pass = formValues[2];
        p.name = user + "@" + host;
        Profiles.add(p);
        profSel = Profiles.count() - 1;
        setSshState(SshState::SSH_CONNECTING);
        return;
    }

    if (ev.fnKey) {
        for (char c : ev.chars)
            if (c == 'q' || c == 'Q') { setSshState(SshState::PROFILE_LIST); return; }
    }
}

// ── SSH connecting ─────────────────────────────────────────────────────────

static void handleSshConnecting() {
    if (sshDirty) {
        M5Cardputer.Display.fillScreen(C_BG);
        M5Cardputer.Display.setFont(&fonts::Font0);
        term.begin();
        const auto& p = Profiles.get(profSel);
        term.printf("Connecting to %s@%s:%d ...\n",
                    p.user.c_str(), p.host.c_str(), p.port);
        sshDirty = false;

        bool ok = SSH.connect(p.host.c_str(), p.port,
                              p.user.c_str(), p.pass.c_str(),
                              sshErr, sizeof(sshErr));
        if (ok) {
            term.println("Connected.\n");
            delay(300);
            term.clear();
            term.drawStatusBar(
                (p.user + "@" + p.host).c_str(),
                WifiMgr.localIP().c_str());
            termInput  = "";
            termCursor = 0;
            passMode   = false;
            passStars  = 0;
            setSshState(SshState::TERMINAL);
        } else {
            term.printf("\r\nError: %s\n", sshErr);
            term.println("Press any key to return.");
            while (true) {
                auto ev = readKeys();
                if (ev.changed) { setSshState(SshState::PROFILE_LIST); return; }
                delay(50);
            }
        }
    }
}

// ── Terminal ───────────────────────────────────────────────────────────────

static void handleTerminal() {
    char rxBuf[SSH_RECV_BUF + 1];
    bool connectionClosed = false;
    while (true) {
        int n = SSH.receive(rxBuf, sizeof(rxBuf) - 1);
        if (n < 0) {
            connectionClosed = true;
            break;
        }
        if (n == 0) break;

        // Auto-detect password prompts → switch to star-echo mode
        if (!passMode && n >= 7) {
            rxBuf[n] = '\0';
            for (char* c = rxBuf; c <= rxBuf + n - 7; c++) {
                // Match "assword" (catches "Password:", "password:", "passphrase:")
                if ((c[0]=='a'||c[0]=='A') && (c[1]=='s'||c[1]=='S') &&
                    (c[2]=='s'||c[2]=='S') && (c[3]=='w'||c[3]=='W') &&
                    (c[4]=='o'||c[4]=='O') && (c[5]=='r'||c[5]=='R') &&
                    (c[6]=='d'||c[6]=='D')) {
                    passMode  = true;
                    passStars = 0;
                    break;
                }
            }
        }
        term.write(rxBuf, n);
    }
    if (connectionClosed) {
        SSH.disconnect();
        passMode = false;
        term.println("\r\n\n[Connection closed]");
        term.println("Press any key.");
        while (true) {
            auto ev = readKeys();
            if (ev.changed) { setSshState(SshState::PROFILE_LIST); return; }
            delay(50);
        }
    }

    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back) {
            SSH.disconnect();
            passMode = false;
            setSshState(SshState::PROFILE_LIST);
            return;
        }
        if (ev.fnKey) {
            for (char c : ev.chars) {
                if (c == 'q' || c == 'Q') {
                    SSH.disconnect();
                    passMode = false;
                    setSshState(SshState::PROFILE_LIST);
                    return;
                }
                // All other fn+letter → Ctrl+letter (e.g. fn+C = Ctrl+C)
                char ctrl = (char)(toupper((unsigned char)c) & 0x1F);
                if (ctrl > 0) SSH.send(&ctrl, 1);
            }
            // fn+nav keys set ev.up/down/left/right with ev.chars cleared — handled below
        }
        if (ev.enter) {
            SSH.send("\r", 1);
            if (passMode) {
                term.write("\r\n", 2);
                passMode  = false;
                passStars = 0;
            }
            termInput  = "";
            termCursor = 0;
        } else if (ev.del) {
            SSH.send("\x7f", 1);   // DEL — standard backspace for Linux PTY
            if (passMode && passStars > 0) {
                term.write("\b \b", 3);
                passStars--;
            }
        } else if (ev.up)    { SSH.send("\x1b[A", 3); }   // arrow keys → ANSI sequences
        else if (ev.down)  { SSH.send("\x1b[B", 3); }
        else if (ev.right) { SSH.send("\x1b[C", 3); }
        else if (ev.left)  { SSH.send("\x1b[D", 3); }
        else if (ev.tab)   { SSH.send("\t", 1); }          // tab completion
        else if (!ev.fnKey) {
            for (char c : ev.chars) {
                SSH.send(&c, 1);
                if (passMode) {
                    term.write("*", 1);
                    passStars++;
                }
            }
            termInput  = "";   // not tracking local buffer; server echoes back
            termCursor = 0;
        }
    }
    term.update();
    delay(SSH_RECV_MS);
}

// ── Public API ─────────────────────────────────────────────────────────────

void appSshEnter() {
    sshState = SshState::PROFILE_LIST;
    sshDirty = true;
}

void appSshLoop() {
    switch (sshState) {
        case SshState::PROFILE_LIST:           handleProfileList();           break;
        case SshState::PROFILE_NEW:            handleNewProfile();            break;
        case SshState::PROFILE_DELETE_CONFIRM: handleProfileDeleteConfirm();  break;
        case SshState::SSH_CONNECTING:         handleSshConnecting();         break;
        case SshState::TERMINAL:               handleTerminal();              break;
    }
}
