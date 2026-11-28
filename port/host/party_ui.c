/* The party finder: see party_ui.h. Counted in vblanks (about 60 a second). */
#include "party_ui.h"

#include "charswap.h"
#include "lobby.h"

#include <psprecomp/net.h>
#include <psprecomp/net_proto.h>

#include <stdio.h>
#include <string.h>

#define P_DOWN   0x0040u
#define P_CIRCLE 0x2000u

#define SEC(s) ((int)((s) * 60))

/* ST_MENU: the F2 menu, two columns (party and warps; characters). ST_LIST: the party finder. */
enum { ST_HIDDEN, ST_MENU, ST_LIST };

/* The lobby menu: warps to the Multi Mode lobby's rooms (destination 2010000,
 * area index 0, zone 2000, by entry point; docs/area_research.md), then the
 * options. */
static const struct { const char *name; int entry; } ROOMS[] = {
    { "Central Plaza", 4 }, { "Shop Plaza", 3 }, { "Bridge (mission terminal)", 0 }, { "Counter Room", 5 },
    { "Cafe", 2 }, { "Observation Deck", 1 }, { "Battle Shop", 9 }, { "Style Shop", 11 }, { "Style Shop: Salon", 15 },
    { "Little Wing", 17 },
};
#define NROOMS ((int)(sizeof ROOMS / sizeof ROOMS[0]))
#define LOBBY_DEST 2010000
#define LOBBY_ZONE 2000
enum { A_NONE, A_FORM, A_JOIN };
/* automation steps */
enum { S_SETTLE, S_PRESS_JOIN, S_WAIT_MATCHING, S_WAIT_TARGET, S_PICK, S_CONFIRM, S_WAIT_DONE };

#define MAX_ROWS 10
#define CHAR_ROWS 15             /* the character column: rows shown at once */
/* the left column: PARTY FINDER, the rooms, weapons in the lobby, CLOSE */
#define L_FINDER  0
#define L_ROOM0   1
#define L_WEAPONS (L_ROOM0 + NROOMS)
#define L_CLOSE   (L_WEAPONS + 1)
#define L_ROWS    (L_CLOSE + 1)

static struct {
    int auto_open, state, sel, top;
    int from_menu;               /* the finder was opened from the F2 menu: Cross goes back to it */
    int col, lsel, rsel, rtop;   /* the F2 menu: the column in use (0 left, 1 right), each one's row */
    int nchars, cur_char;        /* characters: records found, the one worn (-1: your own) */
    int hold;                    /* vblanks Up/Down has been held, for auto-repeat */
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
} U;

static int in_lobby(void) { return lobby_here(); }
/* The leader's lobby options, sent with every warp so the party matches. */
static uint16_t lobby_flags(void) {
    return PSP_MMO_WARP_OPTIONS | (lobby_weapons() ? PSP_MMO_WARP_WEAPONS : 0);
}

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

/* Why the party finder can't open now, or NULL if it can. */
static const char *finder_closed(const psp_net_mmo_status *st, int role) {
    if (!st->adhocctl) return "The party finder opens in Multi Mode (Matching Counter).";
    if (role != PSP_MMO_PARTY_NONE) return "You are already in a party.";
    return NULL;
}

static int char_rows(void) { return U.nchars + 1; }    /* MY OWN CHARACTER, then the partners */

static void open_panel(void);

/* The F2 menu. The column that fits where you are is the one in use: the
 * rooms in the lobby, the finder at the Matching Counter, else the characters. */
static void open_menu(const psp_net_mmo_status *st, int role) {
    U.state = ST_MENU;
    U.nchars = charswap_count();
    U.cur_char = U.nchars ? charswap_current() : -1;
    U.rsel = U.cur_char + 1;
    U.rtop = U.rsel - CHAR_ROWS / 2;
    if (in_lobby()) { U.col = 0; U.lsel = L_ROOM0; }
    else if (!finder_closed(st, role)) { U.col = 0; U.lsel = L_FINDER; }
    else { U.col = 1; U.lsel = L_FINDER; }
}

static void choose_char(int row) {
    char m[96];
    int ok;
    if (row == 0) ok = charswap_restore(m, sizeof m) == 0;
    else if (!U.nchars) { snprintf(m, sizeof m, "The partner characters aren't loaded here: try in the lobby."); ok = 0; }
    else ok = charswap_apply(row - 1, m, sizeof m) == 0;
    /* The look shows when the area loads again: in a lobby, enter it again now. Elsewhere (a
     * mission would start over) the notice says when it will show. */
    if (ok && lobby_reenter() == 0)
        snprintf(m, sizeof m, "%s%s: entering the area again to show it.", row ? "Now " : "Your own look",
                 row ? charswap_name(row - 1) : "");
    notice(m, 6);
    U.state = ST_HIDDEN;
}

static void choose_left(int row, const psp_net_mmo_status *st, int role) {
    if (row == L_FINDER) {
        const char *why = finder_closed(st, role);
        if (why) { notice(why, 4); return; }
        open_panel();
        U.from_menu = 1;
        return;
    }
    if (row == L_CLOSE) { U.state = ST_HIDDEN; return; }
    if (row == L_WEAPONS && lobby_internet_lobby()) {
        /* Internet Multi Mode: each player's own setting (no party warp there); applied by
         * entering the area again, without the character data going out again */
        lobby_set_weapons(!lobby_weapons());
        char m[96];
        snprintf(m, sizeof m, "Weapons in the lobby: %s%s", lobby_weapons() ? "on" : "off",
                 lobby_reenter_quiet() == 0 ? " (entering the area again)" : " (from the next area)");
        notice(m, 4);
        U.state = ST_HIDDEN;
        return;
    }
    if (!in_lobby()) { notice("Warps and lobby options work in the Multi Mode lobby.", 4); return; }
    if (row < L_WEAPONS) {
        if (role != PSP_MMO_PARTY_LEADER) { notice("Only the party leader can warp the party.", 4); return; }
        const psp_net_mmo_warp w = { LOBBY_DEST, 0, LOBBY_ZONE, (int16_t)ROOMS[row - L_ROOM0].entry, lobby_flags() };
        psp_net_mmo_party_warp(&w);
        char m[96];
        snprintf(m, sizeof m, "Warping the party to the %s...", ROOMS[row - L_ROOM0].name);
        notice(m, 4);
        U.state = ST_HIDDEN;
        return;
    }
    /* weapons in the lobby: the leader's choice for the party; the room is re-entered so it shows at once */
    if (role != PSP_MMO_PARTY_LEADER) { notice("Only the party leader can change this.", 4); return; }
    lobby_set_weapons(!lobby_weapons());
    const psp_net_mmo_warp w = { LOBBY_DEST, 0, LOBBY_ZONE, (int16_t)lobby_entry(), lobby_flags() };
    psp_net_mmo_party_warp(&w);
    notice(lobby_weapons() ? "Weapons in the lobby: on" : "Weapons in the lobby: off", 4);
    U.state = ST_HIDDEN;
}

static void open_panel(void) {
    U.state = ST_LIST;
    U.from_menu = 0;
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
    if (row == row_close()) { U.state = U.from_menu ? ST_MENU : ST_HIDDEN; return; }
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
        else open_menu(&st, role);
    }

    /* Up / Down held: repeated after a third of a second, then 10 a second */
    uint32_t nav = pressed;
    if (held & (MI_UP | MI_DOWN)) {
        if (++U.hold > 20 && (U.hold - 20) % 6 == 0) nav |= held & (MI_UP | MI_DOWN);
    } else U.hold = 0;

    if (U.state == ST_MENU) {
        if (nav & (MI_LEFT | MI_RIGHT)) U.col = !U.col;
        if (U.col == 0) {
            if (nav & MI_UP) U.lsel = (U.lsel + L_ROWS - 1) % L_ROWS;
            if (nav & MI_DOWN) U.lsel = (U.lsel + 1) % L_ROWS;
        } else {
            if (nav & MI_UP) U.rsel = (U.rsel + char_rows() - 1) % char_rows();
            if (nav & MI_DOWN) U.rsel = (U.rsel + 1) % char_rows();
        }
        /* the character column's scroll: the chosen row in view, the list's end at the bottom */
        if (U.rsel < U.rtop) U.rtop = U.rsel;
        if (U.rsel >= U.rtop + CHAR_ROWS) U.rtop = U.rsel - CHAR_ROWS + 1;
        if (U.rtop > char_rows() - CHAR_ROWS) U.rtop = char_rows() - CHAR_ROWS;
        if (U.rtop < 0) U.rtop = 0;
        if (pressed & MI_BACK) U.state = ST_HIDDEN;
        else if (pressed & MI_CONFIRM) {
            if (U.col == 0) choose_left(U.lsel, &st, role);
            else choose_char(U.rsel);
        }
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
        if (nav & MI_UP) U.sel = (U.sel + nrows() - 1) % nrows();
        if (nav & MI_DOWN) U.sel = (U.sel + 1) % nrows();
        if (pressed & MI_BACK) U.state = U.from_menu ? ST_MENU : ST_HIDDEN;
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

/* The panels' layout in the 480x272 picture, shared by drawing and taps. */
#define MENU_W  464
#define MENU_LW 204                 /* the left column's width */
static void menu_geom(int *x, int *y, int *w, int *h) {
    *w = MENU_W; *h = 26 + CHAR_ROWS * 12 + 20; *x = (W_ - *w) / 2; *y = (H_ - *h) / 2;
}
static int left_row_y(int y, int i) {   /* the left column's rows, in groups */
    return y + 26 + i * 12 + (i >= L_ROOM0 ? 4 : 0) + (i >= L_WEAPONS ? 4 : 0) + (i >= L_CLOSE ? 4 : 0);
}
static int right_row_y(int y, int r) { return y + 26 + r * 12; }
static int list_rows(void) { return nrows() < MAX_ROWS ? nrows() : MAX_ROWS; }
static void list_geom(int *x, int *y, int *w, int *h) {
    *w = 300; *h = 44 + list_rows() * 12 + 16; *x = (W_ - *w) / 2; *y = (H_ - *h) / 2;
}
static int list_row_y(int y, int r) { return y + 38 + r * 12; }
static int on_row(int ty, int ly) { return ty >= ly - 2 && ty < ly + 10; }   /* a row's highlight, and a little below */

int party_ui_touch(int tx, int ty) {
    int x, y, w, h;
    if (U.state == ST_MENU) {
        menu_geom(&x, &y, &w, &h);
        if (tx < x || ty < y || tx >= x + w || ty >= y + h) return 0;
        psp_net_mmo_status st;
        psp_net_mmo_get_status(&st);
        const int role = psp_net_mmo_party_role();
        if (tx < x + MENU_LW) {
            for (int i = 0; i < L_ROWS; i++) {
                if (!on_row(ty, left_row_y(y, i))) continue;
                if (U.col == 0 && U.lsel == i) choose_left(i, &st, role);
                else { U.col = 0; U.lsel = i; }
                break;
            }
        } else {
            for (int r = 0; r < CHAR_ROWS && U.rtop + r < char_rows(); r++) {
                const int i = U.rtop + r;
                if (!on_row(ty, right_row_y(y, r))) continue;
                if (U.col == 1 && U.rsel == i) choose_char(i);
                else { U.col = 1; U.rsel = i; }
                break;
            }
        }
        return 1;
    }
    if (U.state == ST_LIST) {
        list_geom(&x, &y, &w, &h);
        if (tx < x || ty < y || tx >= x + w || ty >= y + h) return 0;
        for (int r = 0; r < list_rows(); r++) {
            const int i = U.top + r;
            if (!on_row(ty, list_row_y(y, r))) continue;
            if (U.sel == i) choose(i);
            else U.sel = i;
            break;
        }
        return 1;
    }
    return 0;
}

int party_ui_draw(menu_image *img, int *bx, int *by, int *bw, int *bh) {
    if (U.state == ST_MENU) {
        psp_net_mmo_status st;
        psp_net_mmo_get_status(&st);
        const int role = psp_net_mmo_party_role();
        const int finder = !finder_closed(&st, role), lobby = in_lobby(), leader = role == PSP_MMO_PARTY_LEADER;
        int x, y, w, h;
        menu_geom(&x, &y, &w, &h);
        const int lw = MENU_LW, rx = x + lw + 4, rw = w - lw - 4;
        frame_box(img, x, y, w, h);
        fill(img, x + lw, y + 6, 1, h - 24, 0xFF6080C0);
        char line[96];
        menu_text(img, x + 10, y + 8, "PARTY", U.col == 0 ? CYAN : GREY);
        menu_text(img, rx + 6, y + 8, "CHARACTER", U.col == 1 ? CYAN : GREY);
        snprintf(line, sizeof line, "%d/%d", U.rsel + 1, char_rows());
        menu_text(img, x + w - 10 - menu_text_width(line), y + 8, line, GREY);
        for (int i = 0; i < L_ROWS; i++) {   /* the left column, in groups */
            const int ly = left_row_y(y, i);
            uint32_t col = WHITE;
            if (i == L_FINDER) { snprintf(line, sizeof line, "Party finder"); if (!finder) col = GREY; }
            else if (i < L_WEAPONS) { snprintf(line, sizeof line, "%s", ROOMS[i - L_ROOM0].name); if (!lobby || !leader) col = GREY; }
            else if (i == L_WEAPONS) {
                snprintf(line, sizeof line, "Lobby weapons: %s", lobby_weapons() ? "ON" : "OFF");
                if ((!lobby || !leader) && !lobby_internet_lobby()) col = GREY;
            }
            else snprintf(line, sizeof line, "Close");
            if (i == U.lsel) {
                fill(img, x + 6, ly - 2, lw - 12, 11, U.col == 0 ? 0xFF304878 : 0xFF1C2A44);
                if (U.col == 0 && col == WHITE) col = YELLOW;
            }
            menu_text(img, x + 12, ly, line, col);
        }
        const int n = char_rows();
        for (int r = 0; r < CHAR_ROWS && U.rtop + r < n; r++) {   /* the right column */
            const int i = U.rtop + r, ly = right_row_y(y, r);
            const int worn = i == U.cur_char + 1;
            uint32_t col = WHITE;
            if (i == 0) snprintf(line, sizeof line, "%sMy own character", worn ? "* " : "  ");
            else snprintf(line, sizeof line, "%s%-24.24s", worn ? "* " : "  ", charswap_name(i - 1));
            if (i == U.rsel) {
                fill(img, rx + 2, ly - 2, rw - 8, 11, U.col == 1 ? 0xFF304878 : 0xFF1C2A44);
                if (U.col == 1) col = YELLOW;
            }
            menu_text(img, rx + 6, ly, line, col);
            if (i) menu_text(img, x + w - 10 - menu_text_width(charswap_kind(i - 1)), ly, charswap_kind(i - 1), GREY);
        }
        if (!U.nchars) {
            menu_text(img, rx + 6, y + 26 + 18, "Partner characters load in", GREY);
            menu_text(img, rx + 6, y + 26 + 30, "the lobby and in missions.", GREY);
        }
        menu_text(img, x + 10, y + h - 12, "Circle: choose   Cross: close   Left/Right: column   F2 / L3: menu", GREY);
        *bx = x; *by = y; *bw = w; *bh = h;
        return 1;
    }
    if (U.state == ST_LIST) {
        const int rows = list_rows();
        int x, y, w, h;
        list_geom(&x, &y, &w, &h);
        frame_box(img, x, y, w, h);
        menu_text(img, x + 10, y + 8, "PARTY FINDER", CYAN);
        char line[96];
        snprintf(line, sizeof line, "%d online, %d %s", U.players, U.nparties, U.nparties == 1 ? "party" : "parties");
        menu_text(img, x + w - 10 - menu_text_width(line), y + 8, line, GREY);
        if (!U.nparties) menu_text(img, x + 10, y + 22, "No parties yet: start one, or wait.", GREY);
        else menu_text(img, x + 10, y + 22, "Pick a party to join:", GREY);
        for (int r = 0; r < rows; r++) {
            const int i = U.top + r, ly = list_row_y(y, r);
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
    if (U.notice_left > 0 && U.notice[0]) {
        const int w = menu_text_width(U.notice) + 20, h = 17, x = (W_ - w) / 2, y = 6;
        frame_box(img, x, y, w, h);
        menu_text(img, x + 10, y + 5, U.notice, WHITE);
        *bx = x; *by = y; *bw = w; *bh = h;
        return 1;
    }
    return 0;
}
