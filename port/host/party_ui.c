/* The party finder: see party_ui.h. Counted in vblanks (about 60 a second). */
#include "party_ui.h"

#include "lobby.h"

#include <psprecomp/net.h>
#include <psprecomp/net_proto.h>

#include <stdio.h>
#include <string.h>

#define P_DOWN   0x0040u
#define P_CIRCLE 0x2000u

#define SEC(s) ((int)((s) * 60))

enum { ST_HIDDEN, ST_LIST, ST_LOBBY };

/* The lobby menu: warps to the Multi Mode lobby's rooms (destination 2010000,
 * area index 0, zone 2000, by entry point; docs/area_research.md), then the
 * options. */
static const struct { const char *name; int entry; } ROOMS[] = {
    { "Central Plaza", 4 }, { "Shop Plaza", 3 }, { "Bridge (mission terminal)", 0 }, { "Counter Room", 5 },
    { "Cafe", 2 }, { "Observation Deck", 1 },
};
#define NROOMS ((int)(sizeof ROOMS / sizeof ROOMS[0]))
#define LOBBY_DEST 2010000
#define LOBBY_ZONE 2000
enum { A_NONE, A_FORM, A_JOIN };
/* automation steps */
enum { S_SETTLE, S_PRESS_JOIN, S_WAIT_MATCHING, S_WAIT_TARGET, S_PICK, S_CONFIRM, S_WAIT_DONE };

#define MAX_ROWS 10

static struct {
    int auto_open, state, sel, top;
    psp_net_mmo_party parties[PSP_MMO_PARTY_LIST_MAX];
    int nparties, players;
    unsigned gen;
    int since_request;
    uint32_t prev_held;
    int was_adhocctl;
    int since_init;              /* vblanks since the game entered Multi Mode */
    /* automation */
    int act, step, wait, timeout, presses;
    psp_net_mmo_party target;
    /* the button being pressed: held for a few vblanks, then released */
    uint32_t press_btn;
    int press_left, gap_left;
    /* a notice at the top of the picture */
    char notice[96];
    int notice_left;
    int weapons;                 /* the lobby menu's "weapons in the lobby" option */
} U;

static int in_lobby(void) { return lobby_here(); }
int party_ui_weapons(void) { return U.weapons; }
void party_ui_set_weapons(int on) { U.weapons = on ? 1 : 0; }

void party_ui_init(int auto_open) {
    memset(&U, 0, sizeof U);
    U.auto_open = auto_open;
}

int party_ui_open(void) { return U.state != ST_HIDDEN; }

static void notice(const char *s, int secs) {
    snprintf(U.notice, sizeof U.notice, "%s", s);
    U.notice_left = SEC(secs);
    psp_net_log("party finder: %s", s);
}

static void press(uint32_t btn) {
    U.press_btn = btn;
    U.press_left = 6;          /* 100 ms: a 30 fps game sees it too */
    U.gap_left = 12;           /* then 200 ms released before the next press */
}
static int pressing(void) { return U.press_left > 0 || U.gap_left > 0; }

/* rows: the parties, then START A PARTY and CHOOSE MYSELF */
static int nrows(void) { return U.nparties + 2; }
static int row_start(void) { return U.nparties; }
static int row_close(void) { return U.nparties + 1; }

static int lobby_rows(void) { return NROOMS + 2; }

static void open_lobby(void) {
    U.state = ST_LOBBY;
    U.sel = 0;
}

static void choose_lobby(int row, int role) {
    if (row < NROOMS) {
        if (role != PSP_MMO_PARTY_LEADER) { notice("Only the party leader can warp the party.", 4); return; }
        const psp_net_mmo_warp w = { LOBBY_DEST, 0, LOBBY_ZONE, (int16_t)ROOMS[row].entry };
        psp_net_mmo_party_warp(&w);
        char m[96];
        snprintf(m, sizeof m, "Warping the party to the %s...", ROOMS[row].name);
        notice(m, 4);
        U.state = ST_HIDDEN;
        return;
    }
    if (row == NROOMS) {
        U.weapons = !U.weapons;
        notice(U.weapons ? "Weapons in the lobby: on (the palette appears from the next lobby entry)" : "Weapons in the lobby: off", 4);
        return;
    }
    U.state = ST_HIDDEN;
}

static void open_panel(void) {
    U.state = ST_LIST;
    U.sel = 0;
    U.top = 0;
    U.since_request = 1 << 20;   /* ask now */
    U.gen = 0;
}

static void stop_auto(const char *why) {
    if (U.act == A_JOIN) psp_net_mmo_set_join_target(NULL);
    U.act = A_NONE;
    if (why) notice(why, 6);
}

static void choose(int row) {
    if (row == row_close()) { U.state = ST_HIDDEN; return; }
    if (row == row_start()) {
        U.state = ST_HIDDEN;
        U.act = A_FORM; U.step = S_SETTLE; U.wait = SEC(0.5); U.timeout = 0;
        notice("Starting a party...", 4);
        return;
    }
    if (row < 0 || row >= U.nparties || !U.parties[row].open) return;   /* full: not selectable */
    U.target = U.parties[row];
    U.state = ST_HIDDEN;
    psp_net_mmo_set_join_target(U.target.leader_mac);
    U.act = A_JOIN; U.step = S_SETTLE; U.wait = SEC(0.5); U.timeout = 0; U.presses = 0;
    char m[96];
    snprintf(m, sizeof m, "Joining %s's party...", U.target.leader_name);
    notice(m, 6);
}

/* One step of the automation, once the previous button press is over. */
static void auto_step(const psp_net_mmo_status *st) {
    if (U.act == A_NONE || pressing()) return;
    if (U.wait > 0) { U.wait--; return; }
    /* the Matching Counter's menu takes a few seconds to take input after
     * the game enters Multi Mode (a press before that is lost) */
    if (U.step == S_SETTLE && U.since_init < SEC(5)) return;
    if (!st->adhocctl) { stop_auto("Party finder stopped: left Multi Mode."); return; }
    if (U.timeout > 0 && --U.timeout == 0) {
        stop_auto(U.act == A_FORM ? "Couldn't start the party: please use Form Party."
                                  : "Couldn't join that party: please use Join Party.");
        return;
    }
    if (U.act == A_FORM) {
        switch (U.step) {
        case S_SETTLE:                                   /* the cursor starts on Form Party */
            press(P_CIRCLE);
            U.step = S_WAIT_DONE; U.timeout = SEC(10);
            break;
        case S_WAIT_DONE:
            if (st->matching_mode == 1 && st->matching_running) {
                U.act = A_NONE;
                notice("Party started: others can join it now.", 5);
            }
            break;
        }
        return;
    }
    switch (U.step) {                                    /* A_JOIN */
    case S_SETTLE:
        press(P_DOWN);                                   /* Form Party -> Join Party */
        U.step = S_PRESS_JOIN;
        U.wait = SEC(1.0);                               /* the cursor's move: a confirm during it is lost */
        break;
    case S_PRESS_JOIN:
        press(P_CIRCLE);
        U.step = S_WAIT_MATCHING; U.timeout = SEC(10);
        break;
    case S_WAIT_MATCHING:
        if (st->matching_mode == 2 && st->matching_running) { U.step = S_WAIT_TARGET; U.timeout = SEC(15); }
        break;
    case S_WAIT_TARGET:                                   /* only this leader's party is in the list */
        if (st->target_seen) { U.step = S_PICK; U.wait = SEC(0.75); }
        break;
    case S_PICK:
        press(P_CIRCLE);
        U.step = S_CONFIRM; U.wait = SEC(1.0); U.timeout = SEC(20); U.presses = 0;
        break;
    case S_CONFIRM:                                       /* "join this party?" until the leader accepts */
        if (st->established > 0) {
            U.act = A_NONE;
            char m[96];
            snprintf(m, sizeof m, "Joined %s's party.", U.target.leader_name);
            notice(m, 5);
            break;
        }
        if (U.presses < 2) { press(P_CIRCLE); U.presses++; U.wait = SEC(1.5); }
        break;
    }
}

int party_ui_update(uint32_t held, uint32_t latched, uint32_t *inject) {
    *inject = 0;
    if (U.notice_left > 0) U.notice_left--;
    psp_net_mmo_status st;
    psp_net_mmo_get_status(&st);
    const int role = psp_net_mmo_party_role();

    /* opens by itself at the Matching Counter, when the player has no party */
    if (st.adhocctl && !U.was_adhocctl && U.auto_open && role == PSP_MMO_PARTY_NONE && U.act == A_NONE)
        open_panel();
    U.was_adhocctl = st.adhocctl;
    if (!st.adhocctl) U.since_init = 0;
    else if (U.since_init < SEC(60)) U.since_init++;

    const uint32_t pressed = (held & ~U.prev_held) | latched;
    U.prev_held = held;
    if (pressed & MI_TOGGLE) {
        if (U.state != ST_HIDDEN) U.state = ST_HIDDEN;
        else if (U.act != A_NONE) stop_auto("Party finder stopped.");
        else if (in_lobby()) open_lobby();
        else if (!st.adhocctl) notice("The party finder opens in Multi Mode (Matching Counter).", 4);
        else if (role != PSP_MMO_PARTY_NONE) notice("You are already in a party: F2 / L3 in the lobby for its menu.", 4);
        else open_panel();
    }

    if (U.state == ST_LOBBY) {
        if (!in_lobby()) U.state = ST_HIDDEN;
        const int n = lobby_rows();
        if (pressed & MI_UP) U.sel = (U.sel + n - 1) % n;
        if (pressed & MI_DOWN) U.sel = (U.sel + 1) % n;
        if (pressed & MI_BACK) U.state = ST_HIDDEN;
        else if (pressed & MI_CONFIRM) choose_lobby(U.sel, role);
    }

    if (U.state == ST_LIST) {
        if (++U.since_request >= SEC(2)) { U.since_request = 0; psp_net_mmo_request_parties(); }
        unsigned g;
        int players;
        const int n = psp_net_mmo_parties(U.parties, PSP_MMO_PARTY_LIST_MAX, &players, &g);
        if (g != U.gen) {
            U.gen = g;
            if (n != U.nparties) {
                char m[64];
                snprintf(m, sizeof m, "party finder: %d %s listed", n, n == 1 ? "party" : "parties");
                psp_net_log("%s", m);
            }
            U.nparties = n;
            U.players = players;
            if (U.sel >= nrows()) U.sel = nrows() - 1;
        }
        if (pressed & MI_UP) U.sel = (U.sel + nrows() - 1) % nrows();
        if (pressed & MI_DOWN) U.sel = (U.sel + 1) % nrows();
        if (pressed & MI_BACK) U.state = ST_HIDDEN;
        else if (pressed & MI_CONFIRM) choose(U.sel);
        if (U.sel < U.top) U.top = U.sel;
        if (U.sel >= U.top + MAX_ROWS) U.top = U.sel - MAX_ROWS + 1;
    }

    auto_step(&st);
    if (U.press_left > 0) { *inject = U.press_btn; U.press_left--; }
    else if (U.gap_left > 0) U.gap_left--;
    return U.state != ST_HIDDEN;
}

/* ---- drawing ---------------------------------------------------------------------- */

#define W_   480
#define H_   272
#define WHITE  0xFFFFFF
#define GREY   0x8890A0
#define YELLOW 0xFFD040
#define CYAN   0x60D8FF

static void fill(menu_image *img, int x, int y, int w, int h, uint32_t argb) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < img->w && j < img->h) img->px[j * img->w + i] = argb;
}

static void frame_box(menu_image *img, int x, int y, int w, int h) {
    fill(img, x, y, w, h, 0xFF101828);
    fill(img, x, y, w, 1, 0xFF6080C0); fill(img, x, y + h - 1, w, 1, 0xFF6080C0);
    fill(img, x, y, 1, h, 0xFF6080C0); fill(img, x + w - 1, y, 1, h, 0xFF6080C0);
}

int party_ui_draw(menu_image *img, int *bx, int *by, int *bw, int *bh) {
    if (U.state == ST_LIST) {
        const int rows = nrows() < MAX_ROWS ? nrows() : MAX_ROWS;
        const int w = 300, h = 44 + rows * 12 + 16, x = (W_ - w) / 2, y = (H_ - h) / 2;
        frame_box(img, x, y, w, h);
        menu_text(img, x + 10, y + 8, "PARTY FINDER", CYAN);
        char line[96];
        snprintf(line, sizeof line, "%d online, %d %s", U.players, U.nparties, U.nparties == 1 ? "party" : "parties");
        menu_text(img, x + w - 10 - menu_text_width(line), y + 8, line, GREY);
        if (!U.nparties) menu_text(img, x + 10, y + 22, "No parties yet: start one, or wait.", GREY);
        else menu_text(img, x + 10, y + 22, "Pick a party to join:", GREY);
        for (int r = 0; r < rows; r++) {
            const int i = U.top + r, ly = y + 38 + r * 12;
            uint32_t col = WHITE;
            if (i < U.nparties) {
                const psp_net_mmo_party *p = &U.parties[i];
                if (p->max) snprintf(line, sizeof line, "%-24.24s %d/%d%s", p->leader_name, p->members, p->max, p->open ? "" : "  FULL");
                else snprintf(line, sizeof line, "%-24.24s %d", p->leader_name, p->members);
                if (!p->open) col = GREY;
            } else if (i == row_start()) snprintf(line, sizeof line, "START A PARTY");
            else snprintf(line, sizeof line, "CHOOSE MYSELF (close)");
            if (i == U.sel) { fill(img, x + 6, ly - 2, w - 12, 11, 0xFF304878); col = i < U.nparties && !U.parties[i].open ? GREY : YELLOW; }
            menu_text(img, x + 12, ly, line, col);
        }
        menu_text(img, x + 10, y + h - 12, "Circle: choose   Cross: close   F2 / L3: finder", GREY);
        *bx = x; *by = y; *bw = w; *bh = h;
        return 1;
    }
    if (U.state == ST_LOBBY) {
        const int rows = lobby_rows();
        const int w = 300, h = 44 + rows * 12 + 22, x = (W_ - w) / 2, y = (H_ - h) / 2;
        const int leader = psp_net_mmo_party_role() == PSP_MMO_PARTY_LEADER;
        frame_box(img, x, y, w, h);
        menu_text(img, x + 10, y + 8, "LOBBY", CYAN);
        menu_text(img, x + 10, y + 22, leader ? "Warp the party to:" : "Warp (party leader only):", GREY);
        char line[96];
        for (int i = 0; i < rows; i++) {
            const int ly = y + 38 + i * 12 + (i >= NROOMS ? 6 : 0);
            uint32_t col = i < NROOMS && !leader ? GREY : WHITE;
            if (i < NROOMS) snprintf(line, sizeof line, "%s", ROOMS[i].name);
            else if (i == NROOMS) snprintf(line, sizeof line, "Weapons in the lobby: %s", U.weapons ? "ON" : "OFF");
            else snprintf(line, sizeof line, "Close");
            if (i == U.sel) { fill(img, x + 6, ly - 2, w - 12, 11, 0xFF304878); if (col == WHITE) col = YELLOW; }
            menu_text(img, x + 12, ly, line, col);
        }
        menu_text(img, x + 10, y + h - 12, "Circle: choose   Cross: close   F2 / L3: menu", GREY);
        *bx = x; *by = y; *bw = w; *bh = h;
        return 1;
    }
    if (U.notice_left > 0 && U.notice[0]) {
        const int w = menu_text_width(U.notice) + 20, h = 17, x = (W_ - w) / 2, y = 6;
        frame_box(img, x, y, w, h);
        menu_text(img, x + 10, y + 5, U.notice, WHITE);
        *bx = x; *by = y; *bw = w; *bh = h;
        return 1;
    }
    return 0;
}
