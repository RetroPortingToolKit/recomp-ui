// The dashboard's tab strips (launcher_player_tabs.h) in real ImGui frames
// with no window, as launcher_pad_nav_test.cpp runs the pad lock.
//
// What a player relies on:
//   - there is one tab per player, and a click opens it;
//   - players 1 to 4 are the first row and players 5 to 8 the second, and a
//     label is never shortened: a strip too narrow for four takes two to a
//     row, then one;
//   - the focus ring can cross the strip without changing the player: only
//     activate (Space, Cross / A) opens the focused tab;
//   - R1 / L1 open the next / previous player and put the focus ring there;
//   - a shoulder button that is held when pad navigation starts does nothing
//     (the lock of launcher_pad_nav.h), and nothing happens over an open list,
//     in a text field, or while the window-switcher button is down;
//   - the memory-card strip under the player's card opens a card by a click or
//     by activate, and R1 / L1 leave it alone.
#include "imgui.h"
#include "imgui_internal.h"
#include "launcher_pad_nav.h"
#include "launcher_player_tabs.h"

#include <cstdio>

static int fails;

static void expect(bool cond, const char* what) {
    if (cond) { std::printf("ok: %s\n", what); return; }
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++fails;
}

static const RuiPlayerTabsLook kLook = {
    IM_COL32(30, 30, 40, 255), IM_COL32(40, 40, 50, 255), IM_COL32(20, 20, 30, 255),
    IM_COL32(60, 60, 80, 255), IM_COL32(140, 140, 160, 255), IM_COL32(230, 230, 240, 255),
    IM_COL32(46, 125, 255, 255), IM_COL32(70, 227, 155, 255), IM_COL32(140, 140, 160, 255),
    34.0f, 4.0f, 6.0f,
};

struct Page {
    int     count = 4;
    int     open = 0;          // the player strip's answer, fed back every frame
    int     card = 0;          // the card strip's answer
    int     cards = 0;         // 2: a memory-card strip under the player's card
    float   width = 640.0f;
    bool    list_open = false; // a popup over the page
    bool    field = false;     // a text field that holds the keyboard
    int     per_row = 0;       // tabs in one row of the player strip, as drawn
    ImVec2  origin;            // of the player strip
    ImVec2  card_origin;       // of the card strip
    ImGuiID tab_id[8] = {};
    ImGuiID card_tab_id[2] = {};
    ImGuiID card_id = 0;       // the button under the player strip
    char    name[32] = "pad";
};

static void new_context() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
}

// One dashboard-like frame: a button above the strip, the strip, a button
// under it where the player's card is, and under that the card strip.
static void frame(Page& pg) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1280, 720));
    ImGui::Begin("##page", nullptr, ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::Button("Settings");
    pg.origin = ImGui::GetCursorScreenPos();
    bool assigned[8] = { true };
    pg.per_row = rui_tabs_per_row(pg.count, kRuiPlayerTabsRow, pg.width, kLook.gap,
                                  rui_tab_label_width("Player 8"));
    pg.open = rui_player_tabs(pg.count, pg.open, assigned, pg.width, kLook, "Player");
    ImGui::PushID("player_tabs");
    for (int p = 0; p < pg.count; ++p) {
        ImGui::PushID(p);
        pg.tab_id[p] = ImGui::GetID("##tab");
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::Button("Configure");
    pg.card_id = ImGui::GetItemID();
    if (pg.cards) {
        static const char* const kCards[] = { "Card 1 \xC2\xB7 3/15", "Card 2 \xC2\xB7 0/15" };
        const bool enabled[2] = { true, false };
        pg.card_origin = ImGui::GetCursorScreenPos();
        pg.card = rui_tab_rows("card_tabs", pg.cards, pg.card, kCards, enabled, pg.width,
                               kLook, pg.cards, 0);
        ImGui::PushID("card_tabs");
        for (int c = 0; c < pg.cards; ++c) {
            ImGui::PushID(c);
            pg.card_tab_id[c] = ImGui::GetID("##tab");
            ImGui::PopID();
        }
        ImGui::PopID();
        ImGui::Button("Browse");
    }
    if (pg.field) {
        ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##name", pg.name, sizeof(pg.name));
    }
    if (pg.list_open) {
        ImGui::OpenPopup("##list");
        if (ImGui::BeginPopup("##list")) {
            ImGui::Selectable("Keyboard");
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    ImGui::EndFrame();
}

static void frames(Page& pg, int n) { for (int i = 0; i < n; ++i) frame(pg); }

static void tap(Page& pg, ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frames(pg, 2);
    ImGui::GetIO().AddKeyEvent(key, false);
    frames(pg, 2);
}

static void click(Page& pg, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frames(pg, 2);
    io.AddMouseButtonEvent(0, true);
    frames(pg, 2);
    io.AddMouseButtonEvent(0, false);
    frames(pg, 2);
}

// The middle of player p's tab: its column and row in the strip as drawn.
static ImVec2 tab_centre(const Page& pg, int p) {
    const int n = pg.per_row;
    const float tab_w = (pg.width - kLook.gap * (float)(n - 1)) / (float)n;
    return ImVec2(pg.origin.x + (tab_w + kLook.gap) * (float)(p % n) + tab_w * 0.5f,
                  pg.origin.y + (kLook.height + kLook.gap) * (float)(p / n) +
                      kLook.height * 0.5f);
}

static void click_tab(Page& pg, int p) {
    const ImVec2 at = tab_centre(pg, p);
    click(pg, at.x, at.y);
}

static void click_card_tab(Page& pg, int c) {
    const float tab_w = (pg.width - kLook.gap * (float)(pg.cards - 1)) / (float)pg.cards;
    click(pg, pg.card_origin.x + (tab_w + kLook.gap) * (float)c + tab_w * 0.5f,
          pg.card_origin.y + kLook.height * 0.5f);
}

static ImGuiID focus() { return GImGui->NavId; }

static void test_one_tab_per_player() {
    static const int kCounts[] = { 2, 3, 4, 5, 8 };
    for (int count : kCounts) {
        new_context();
        Page pg;
        pg.count = count;
        frames(pg, 2);
        bool each = true;
        for (int p = count - 1; p >= 0; --p) {
            click_tab(pg, p);
            if (pg.open != p) each = false;
        }
        char what[64];
        std::snprintf(what, sizeof(what), "%d players: a click opens each of %d tabs", count, count);
        expect(each, what);
        click_tab(pg, count - 1);
        click(pg, pg.origin.x + pg.width + 40.0f, pg.origin.y + kLook.height * 0.5f);
        std::snprintf(what, sizeof(what), "%d players: there is no tab past the row's end", count);
        expect(pg.open == count - 1, what);
        ImGui::DestroyContext();
    }

    // A tab that is not there cannot be the open one.
    new_context();
    Page pg;
    pg.count = 2;
    pg.open = 5;
    frames(pg, 2);
    expect(pg.open == 1, "an open tab past the strip becomes the last tab");
    ImGui::DestroyContext();
}

// Players 1 to 4 in the first row, players 5 to 8 in the second.
static void test_two_rows() {
    new_context();
    Page pg;
    pg.count = 8;
    frames(pg, 2);
    expect(pg.per_row == 4, "eight players: four tabs to a row");

    // Under Player 1's tab is Player 5's, one tab height and one gap down.
    const ImVec2 first = tab_centre(pg, 0);
    click(pg, first.x, first.y + kLook.height + kLook.gap);
    expect(pg.open == 4, "the tab under Player 1's is Player 5's");
    click(pg, tab_centre(pg, 3).x, first.y + kLook.height + kLook.gap);
    expect(pg.open == 7, "the tab under Player 4's is Player 8's");
    click(pg, first.x, first.y + (kLook.height + kLook.gap) * 2.0f);
    expect(pg.open == 7, "there is no third row");

    // The focus ring moves between the rows, and opens nothing on its way.
    click_tab(pg, 1);
    expect(pg.open == 1 && focus() == pg.tab_id[1], "a click opens Player 2 and focuses its tab");
    tap(pg, ImGuiKey_DownArrow);
    expect(focus() == pg.tab_id[5], "Down moves the focus ring to Player 6's tab");
    expect(pg.open == 1, "and Player 2 stays open");
    tap(pg, ImGuiKey_GamepadFaceDown);
    expect(pg.open == 5, "Cross / A opens Player 6");
    tap(pg, ImGuiKey_DownArrow);
    expect(focus() == pg.card_id, "Down from the second row reaches the card");
    tap(pg, ImGuiKey_UpArrow);
    bool second_row = false;
    for (int p = 4; p < 8; ++p) second_row = second_row || focus() == pg.tab_id[p];
    expect(second_row, "Up from the card comes back to the second row");
    tap(pg, ImGuiKey_UpArrow);
    bool first_row = false;
    for (int p = 0; p < 4; ++p) first_row = first_row || focus() == pg.tab_id[p];
    expect(first_row, "Up again reaches the first row");
    expect(pg.open == 5, "crossing both rows does not change the open player");

    // L1 / R1 run through all eight, across the rows.
    click_tab(pg, 3);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 4 && focus() == pg.tab_id[4], "R1 on Player 4 opens Player 5 in the second row");
    tap(pg, ImGuiKey_GamepadL1);
    expect(pg.open == 3 && focus() == pg.tab_id[3], "L1 on Player 5 opens Player 4 in the first row");
    click_tab(pg, 7);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 0, "R1 on Player 8 goes round to Player 1");
    tap(pg, ImGuiKey_GamepadL1);
    expect(pg.open == 7, "L1 on Player 1 goes round to Player 8");
    ImGui::DestroyContext();

    // Five to seven players: a short second row stands under the first tabs.
    new_context();
    pg = Page();
    pg.count = 6;
    frames(pg, 2);
    const ImVec2 p2 = tab_centre(pg, 1);
    click(pg, p2.x, p2.y + kLook.height + kLook.gap);
    expect(pg.open == 5, "six players: Player 6's tab is under Player 2's");
    click(pg, tab_centre(pg, 2).x, p2.y + kLook.height + kLook.gap);
    expect(pg.open == 5, "six players: there is no tab under Player 3's");
    ImGui::DestroyContext();
}

// A label is never shortened: a narrow strip takes fewer tabs to a row.
static void test_narrow_strip() {
    new_context();
    Page pg;
    pg.count = 8;
    frames(pg, 1);
    const float label = rui_tab_label_width("Player 8");
    const float four = label * 4.0f + kLook.gap * 3.0f;
    const float two = label * 2.0f + kLook.gap;
    expect(rui_tabs_per_row(8, 4, four, kLook.gap, label) == 4, "four labels fit: four to a row");
    expect(rui_tabs_per_row(8, 4, four - 1.0f, kLook.gap, label) == 2, "four do not fit: two to a row");
    expect(rui_tabs_per_row(8, 4, two - 1.0f, kLook.gap, label) == 1, "two do not fit: one to a row");
    expect(rui_tabs_per_row(4, 4, four - 1.0f, kLook.gap, label) == 2, "four players, narrow: two rows of two");
    expect(rui_tabs_per_row(3, 4, four, kLook.gap, label) == 3, "three players: one row of three");
    expect(rui_tabs_per_row(3, 4, label * 3.0f + kLook.gap * 2.0f - 1.0f, kLook.gap, label) == 2,
           "three players, narrow: two to a row");
    expect(rui_tabs_per_row(2, 4, two, kLook.gap, label) == 2, "two players: one row of two");

    // Drawn that way: at a width for two tabs, Player 3 is under Player 1.
    pg.width = two + 20.0f;
    frames(pg, 2);
    expect(pg.per_row == 2, "the narrow strip is drawn two to a row");
    const ImVec2 first = tab_centre(pg, 0);
    click(pg, first.x, first.y + kLook.height + kLook.gap);
    expect(pg.open == 2, "the tab under Player 1's is then Player 3's");
    click(pg, first.x, first.y + (kLook.height + kLook.gap) * 3.0f);
    expect(pg.open == 6, "and the fourth row starts with Player 7");
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 7 && focus() == pg.tab_id[7], "R1 still steps to the next player");
    ImGui::DestroyContext();
}

static void test_focus_does_not_open() {
    new_context();
    Page pg;
    frames(pg, 2);
    click_tab(pg, 0);
    expect(pg.open == 0 && focus() == pg.tab_id[0], "a click opens Player 1 and focuses its tab");

    tap(pg, ImGuiKey_RightArrow);
    expect(focus() == pg.tab_id[1], "Right moves the focus ring to Player 2's tab");
    expect(pg.open == 0, "and Player 1 stays open");
    tap(pg, ImGuiKey_Space);
    expect(pg.open == 1, "Space opens the focused tab");

    tap(pg, ImGuiKey_RightArrow);
    tap(pg, ImGuiKey_RightArrow);
    expect(focus() == pg.tab_id[3], "Right twice more reaches Player 4's tab");
    tap(pg, ImGuiKey_GamepadFaceDown);
    expect(pg.open == 3, "Cross / A opens the focused tab");

    // Down to the card and back up: the ring lands on a tab, the player stays.
    tap(pg, ImGuiKey_DownArrow);
    expect(focus() == pg.card_id, "Down leaves the strip for the card");
    tap(pg, ImGuiKey_UpArrow);
    bool on_a_tab = false;
    for (int p = 0; p < pg.count; ++p) on_a_tab = on_a_tab || focus() == pg.tab_id[p];
    expect(on_a_tab, "Up comes back to the strip");
    expect(pg.open == 3, "crossing the strip does not change the open player");
    ImGui::DestroyContext();
}

static void test_shoulder_buttons() {
    new_context();
    Page pg;
    frames(pg, 2);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 1, "R1 opens the next player");
    expect(focus() == pg.tab_id[1], "and puts the focus ring on that tab");
    tap(pg, ImGuiKey_GamepadR1);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 3, "R1 twice more opens Player 4");
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 0, "R1 on the last player goes round to Player 1");
    tap(pg, ImGuiKey_GamepadL1);
    expect(pg.open == 3, "L1 on Player 1 goes round to the last player");
    tap(pg, ImGuiKey_GamepadL1);
    expect(pg.open == 2 && focus() == pg.tab_id[2], "L1 opens the previous player");

    // Held for a second: one step, not one a frame.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadR1, true);
    frames(pg, 60);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadR1, false);
    frames(pg, 2);
    expect(pg.open == 3, "a held R1 steps once");
    ImGui::DestroyContext();
}

static void test_shoulder_buttons_stay_out_of_the_way() {
    // Held when pad navigation starts: nothing until it is released.
    new_context();
    Page pg;
    frames(pg, 2);
    launcher_pad_nav_lock_held_keys();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadR1, true);
    frames(pg, 30);
    expect(pg.open == 0, "an R1 held when pad navigation starts opens nothing");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadR1, false);
    frames(pg, 3);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 1, "after a release, a real press works");
    ImGui::DestroyContext();

    // The window-switcher button gives L1 / R1 another meaning.
    new_context();
    pg = Page();
    frames(pg, 2);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    frames(pg, 3);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 0, "R1 with face-left held opens nothing");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
    frames(pg, 3);
    ImGui::DestroyContext();

    // An open list owns the pad.
    new_context();
    pg = Page();
    pg.list_open = true;
    frames(pg, 3);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 0, "R1 over an open list opens nothing");
    ImGui::DestroyContext();

    // So does a text field.
    new_context();
    pg = Page();
    pg.field = true;
    frames(pg, 3);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.open == 0, "R1 while a field is edited opens nothing");
    ImGui::DestroyContext();
}

// The memory-card strip: Card 1 and Card 2, one shown.
static void test_card_tabs() {
    new_context();
    Page pg;
    pg.cards = 2;
    frames(pg, 2);
    expect(pg.card == 0, "Card 1 is open at the start");
    click_card_tab(pg, 1);
    expect(pg.card == 1 && focus() == pg.card_tab_id[1], "a click opens Card 2 and focuses its tab");
    expect(pg.open == 0, "and leaves the player strip alone");
    click_card_tab(pg, 0);
    expect(pg.card == 0, "a click opens Card 1 again");

    tap(pg, ImGuiKey_RightArrow);
    expect(focus() == pg.card_tab_id[1], "Right moves the focus ring to Card 2's tab");
    expect(pg.card == 0, "and Card 1 stays open");
    tap(pg, ImGuiKey_GamepadFaceDown);
    expect(pg.card == 1, "Cross / A opens the focused card tab");
    tap(pg, ImGuiKey_LeftArrow);
    tap(pg, ImGuiKey_Space);
    expect(pg.card == 0, "Left and Space open Card 1");

    // The ring reaches the card strip from the controls above it and leaves it
    // downwards, and opens nothing on its way.
    click_tab(pg, 0);
    tap(pg, ImGuiKey_DownArrow);
    expect(focus() == pg.card_id, "Down from the player strip reaches the card's control");
    tap(pg, ImGuiKey_DownArrow);
    expect(focus() == pg.card_tab_id[0] || focus() == pg.card_tab_id[1],
           "Down again reaches the card strip");
    tap(pg, ImGuiKey_DownArrow);
    expect(focus() != pg.card_tab_id[0] && focus() != pg.card_tab_id[1],
           "Down again leaves the card strip for the card under it");
    expect(pg.card == 0 && pg.open == 0, "crossing the card strip opens nothing");

    // R1 / L1 belong to the players.
    click_card_tab(pg, 1);
    tap(pg, ImGuiKey_GamepadR1);
    expect(pg.card == 1, "R1 leaves the open card alone");
    expect(pg.open == 1, "and opens the next player");
    tap(pg, ImGuiKey_GamepadL1);
    expect(pg.card == 1 && pg.open == 0, "L1 the same");

    // A card that is not there cannot be the open one.
    pg.card = 5;
    frames(pg, 2);
    expect(pg.card == 1, "an open card past the strip becomes the last card");
    ImGui::DestroyContext();
}

int main() {
    IMGUI_CHECKVERSION();
    test_one_tab_per_player();
    test_two_rows();
    test_narrow_strip();
    test_focus_does_not_open();
    test_shoulder_buttons();
    test_shoulder_buttons_stay_out_of_the_way();
    test_card_tabs();
    if (fails) { std::fprintf(stderr, "launcher_player_tabs_strip_test: %d failure(s)\n", fails); return 1; }
    std::printf("launcher_player_tabs_strip_test: all checks passed\n");
    return 0;
}
