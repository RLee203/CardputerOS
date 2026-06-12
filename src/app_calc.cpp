#include "app_calc.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <math.h>

// ── State ──────────────────────────────────────────────────────────────────
static String calcExpr   = "";
static String calcResult = "";
static bool   calcShownError = false;  // true only after Enter evaluates to error

// ── Expression evaluator (recursive descent) ──────────────────────────────

static const char* parsePtr = nullptr;
static bool        parseErr = false;

static double parseExpr();
static double parseTerm();

static void skipWs() { while (*parsePtr == ' ') parsePtr++; }

static double parseAtom() {
    skipWs();
    if (*parsePtr == '(') {
        parsePtr++;
        double v = parseExpr();
        skipWs();
        if (*parsePtr == ')') parsePtr++;
        return v;
    }
    bool neg = false;
    if (*parsePtr == '-') { neg = true; parsePtr++; skipWs(); }
    else if (*parsePtr == '+') { parsePtr++; skipWs(); }

    if (*parsePtr == '(') {
        parsePtr++;
        double v = parseExpr();
        skipWs();
        if (*parsePtr == ')') parsePtr++;
        return neg ? -v : v;
    }

    // Named function call: sqrt, sq, abs, pow, log, ln, floor, ceil
    if (isalpha((unsigned char)*parsePtr)) {
        char fname[8] = {};
        int flen = 0;
        while (isalpha((unsigned char)*parsePtr) && flen < 7)
            fname[flen++] = *parsePtr++;
        fname[flen] = '\0';
        skipWs();
        if (*parsePtr != '(') { parseErr = true; return 0; }
        parsePtr++;
        double a1 = parseExpr();
        double a2 = NAN;
        skipWs();
        if (*parsePtr == ',') { parsePtr++; a2 = parseExpr(); skipWs(); }
        if (*parsePtr == ')') parsePtr++;
        double r = 0;
        if      (strcmp(fname, "sqrt")  == 0) r = sqrt(a1);
        else if (strcmp(fname, "sq")    == 0) r = a1 * a1;
        else if (strcmp(fname, "abs")   == 0) r = fabs(a1);
        else if (strcmp(fname, "log")   == 0) r = log10(a1);
        else if (strcmp(fname, "ln")    == 0) r = log(a1);
        else if (strcmp(fname, "pow")   == 0) r = isnan(a2) ? (parseErr = true, 0.0) : pow(a1, a2);
        else if (strcmp(fname, "floor") == 0) r = floor(a1);
        else if (strcmp(fname, "ceil")  == 0) r = ceil(a1);
        else { parseErr = true; return 0; }
        if (isnan(r) || isinf(r)) { parseErr = true; return 0; }
        return neg ? -r : r;
    }

    if (!isdigit((unsigned char)*parsePtr) && *parsePtr != '.') {
        parseErr = true;
        return 0;
    }
    double v = 0;
    while (isdigit((unsigned char)*parsePtr)) v = v * 10 + (*parsePtr++ - '0');
    if (*parsePtr == '.') {
        parsePtr++;
        double frac = 0.1;
        while (isdigit((unsigned char)*parsePtr)) { v += (*parsePtr++ - '0') * frac; frac *= 0.1; }
    }
    return neg ? -v : v;
}

static double parseTerm() {
    double left = parseAtom();
    while (true) {
        skipWs();
        char op = *parsePtr;
        if (op != '*' && op != '/' && op != '%') break;
        parsePtr++;
        double right = parseAtom();
        if (op == '*') left *= right;
        else if (right == 0) { parseErr = true; return 0; }
        else if (op == '/') left /= right;
        else left = fmod(left, right);
    }
    return left;
}

static double parseExpr() {
    double left = parseTerm();
    while (true) {
        skipWs();
        char op = *parsePtr;
        if (op != '+' && op != '-') break;
        parsePtr++;
        double right = parseTerm();
        left = (op == '+') ? left + right : left - right;
    }
    return left;
}

static String evaluate(const String& expr) {
    if (expr.length() == 0) return "";
    parseErr = false;
    parsePtr = expr.c_str();
    double result = parseExpr();
    skipWs();
    if (parseErr || *parsePtr != '\0') return "Error";
    if (isnan(result) || isinf(result)) return "Error";
    if (result == (long long)result && result >= -99999999.0 && result <= 999999999.0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld", (long long)result);
        return String(buf);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%.8g", result);
    return String(buf);
}

// Returns true when the expression ends with an operator/dot — i.e. user is
// still typing and we should NOT show an error.
static bool isExprIncomplete(const String& s) {
    if (s.length() == 0) return true;
    char c = s.charAt(s.length() - 1);
    return c == '+' || c == '-' || c == '*' || c == '/'
        || c == '%' || c == '(' || c == '.' || c == ','
        || isalpha((unsigned char)c);
}

// ── Display ────────────────────────────────────────────────────────────────

// Layout (240×135 total):
//  y=  0..12   status bar         (13px)
//  y= 13..58   expression area    (46px)  right-aligned, size-2 text
//  y= 59..100  result area        (42px)  right-aligned, size-2 text
//  y=101..113  hint line 1        (12px)
//  y=113..125  hint line 2        (12px)

static void drawCalc() {
    auto& d = M5Cardputer.Display;

    // ── Status bar ──────────────────────────────────────────────────────────
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    const char* title = "Calculator";
    d.setCursor((SCREEN_W - (int)strlen(title) * FONT_W) / 2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG);

    // ── Expression area ─────────────────────────────────────────────────────
    constexpr int EX_Y = STATUS_H, EX_H = 46;
    constexpr uint32_t EX_BG = 0x0A0A18;
    d.fillRect(0, EX_Y, SCREEN_W, EX_H, EX_BG);

    d.setTextSize(2);
    // Show up to 19 chars of expression, from the right
    constexpr int MAX_CHARS = (SCREEN_W - 8) / (FONT_W * 2);
    String disp = calcExpr.length() ? calcExpr : "";
    if ((int)disp.length() > MAX_CHARS)
        disp = disp.substring(disp.length() - MAX_CHARS);
    if (disp.length() == 0) {
        d.setTextColor(0x333355, EX_BG);
        d.setCursor(SCREEN_W - FONT_W * 2 - 4, EX_Y + (EX_H - 16) / 2);
        d.print("0");
    } else {
        d.setTextColor(0xFFFFFF, EX_BG);
        int tw = (int)disp.length() * FONT_W * 2;
        d.setCursor(SCREEN_W - tw - 4, EX_Y + (EX_H - 16) / 2);
        d.print(disp.c_str());
    }

    // ── Result area ─────────────────────────────────────────────────────────
    constexpr int RE_Y = EX_Y + EX_H, RE_H = 42;
    constexpr uint32_t RE_BG = 0x000000;
    d.fillRect(0, RE_Y, SCREEN_W, RE_H, RE_BG);
    d.drawFastHLine(0, RE_Y, SCREEN_W, 0x222233);

    if (calcShownError) {
        d.setTextColor(C_ERROR, RE_BG);
        d.setTextSize(1);
        const char* em = "Error";
        d.setCursor(SCREEN_W - (int)strlen(em) * FONT_W - 4,
                    RE_Y + (RE_H - FONT_H) / 2);
        d.print(em);
    } else if (calcResult.length() && calcResult != "Error") {
        // Show result right-aligned in amber — matches calculator aesthetics
        d.setTextSize(2);
        d.setTextColor(0xFFAA00, RE_BG);
        String rs = "= " + calcResult;
        int tw = (int)rs.length() * FONT_W * 2;
        if (tw > SCREEN_W - 8) {
            // Result too wide — fall back to size 1
            d.setTextSize(1);
            tw = (int)rs.length() * FONT_W;
        }
        d.setCursor(SCREEN_W - tw - 4,
                    RE_Y + (RE_H - (d.getTextSizeX() == 2 ? 16 : 8)) / 2);
        d.print(rs.c_str());
    }

    // ── Hint area ───────────────────────────────────────────────────────────
    constexpr int HN_Y = RE_Y + RE_H + 2;
    d.fillRect(0, HN_Y, SCREEN_W, SCREEN_H - HN_Y, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, HN_Y);
    d.print("x=mul /=div %=mod ()=group");
    d.setCursor(2, HN_Y + 10);
    d.print("sqrt( sq( abs( pow(x,y) log( ln(");
    d.setCursor(2, HN_Y + 20);
    d.print("Ent=eval fn+C=clear bksp=back");
}

// ── Public API ─────────────────────────────────────────────────────────────

void appCalcEnter() {
    calcExpr      = "";
    calcResult    = "";
    calcShownError = false;
    drawCalc();
}

void appCalcLoop() {
    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) { goHome(); return; }

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if (c == 'c' || c == 'C') {
                calcExpr = ""; calcResult = ""; calcShownError = false;
                drawCalc(); return;
            }
            if (c == 'q' || c == 'Q') { goHome(); return; }
        }
        return;
    }

    if (ev.del) {
        if (calcExpr.length() > 0) {
            calcExpr.remove(calcExpr.length() - 1);
        }
        calcShownError = false;
        // Live result only when expression is complete (no trailing operator)
        if (!isExprIncomplete(calcExpr)) {
            calcResult = evaluate(calcExpr);
            if (calcResult == "Error") calcResult = "";  // no error during typing
        } else {
            calcResult = "";
        }
        drawCalc();
        return;
    }

    if (ev.enter) {
        if (calcExpr.length()) {
            String res = evaluate(calcExpr);
            if (res == "Error") {
                calcShownError = true;
                calcResult = "";
            } else {
                calcShownError = false;
                calcResult = res;
                calcExpr   = res;  // chain: result becomes next operand
            }
        }
        drawCalc();
        return;
    }

    // ── Character input ─────────────────────────────────────────────────────
    for (char c : ev.chars) {
        char lo = (char)tolower((unsigned char)c);
        // x → multiplication; keeps expression readable
        if (lo == 'x') c = '*';
        // Store all other letters lowercase (for function names like sqrt, pow…)
        else if (isalpha((unsigned char)c)) c = lo;

        bool valid = isdigit((unsigned char)c)
                  || isalpha((unsigned char)c)
                  || c == '+' || c == '-' || c == '*' || c == '/'
                  || c == '%' || c == '(' || c == ')' || c == '.' || c == ',';
        if (!valid || (int)calcExpr.length() >= 38) continue;

        // Prevent leading zeros: "00" → "0"
        if (c == '0' && calcExpr == "0") continue;
        // Replace lone "0" with digit (unless it's a decimal or operator follows)
        if (calcExpr == "0" && isdigit((unsigned char)c)) calcExpr = "";

        calcShownError = false;
        calcExpr += c;

        // Update live preview only for complete expressions
        if (!isExprIncomplete(calcExpr)) {
            calcResult = evaluate(calcExpr);
            if (calcResult == "Error") calcResult = "";  // never show mid-type errors
        } else {
            calcResult = "";
        }
    }
    drawCalc();
}
