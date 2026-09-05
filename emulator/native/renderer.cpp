#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "epaper_ui/dashboard_page.h"
#include "epaper_ui/details_page.h"
#include "epaper_ui/follow_up_page.h"
#include "epaper_ui/keyboard.h"
#include "epaper_ui/notes_page.h"
#include "epaper_ui/onboarding_page.h"
#include "epaper_ui/settings_page.h"
#include "epaper_ui/summarize_page.h"
#include "epaper_ui/todos_page.h"
#include "epaper_ui/toast.h"
#include "epaper_ui/vibe_check_page.h"
#include "epaper_ui/wifi_page.h"
#include "project_assets.h"

namespace {
constexpr int kWidth = 480;
constexpr int kHeight = 800;

bool ParseInt(const std::string& value, int* result)
{
    if (value.empty() || result == nullptr) return false;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto parsed = std::from_chars(first, last, *result);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

int SelectionCount(const std::string& screen)
{
    if (screen == "home" || screen == "dashboard") return 5;
    if (screen == "onboarding") return 6;
    if (screen == "settings") return 6;
    if (screen == "wifi") return 2;
    if (screen == "notes" || screen == "todos" || screen == "to-dos" ||
        screen == "followup" || screen == "follow-up") return 2;
    if (screen == "summarize" || screen == "vibe-check" || screen == "vibe" ||
        screen == "details") return 1;
    return 0;
}

epaper_ui::StatusBarState Status()
{
    return {{82, false}, epaper_ui::WifiStatus::kConnected, "09:41", true, false, false};
}

epaper_ui::GlobalFooterState Footer()
{
    using namespace epaper_ui;
    const auto icon = [](EmbeddedIconId id) { return project_assets::GetIcon(id); };
    GlobalFooterState footer = {};
    footer.visible = true;
    footer.home = {true, false, icon(EmbeddedIconId::kHome)};
    footer.settings = {true, false, icon(EmbeddedIconId::kSettings)};
    footer.wifi = {true, false, icon(EmbeddedIconId::kWifiConfig)};
    footer.time = {true, false, icon(EmbeddedIconId::kTime)};
    footer.folder = {true, false, icon(EmbeddedIconId::kFolder)};
    footer.sticky = {true, false, icon(EmbeddedIconId::kSticky)};
    footer.mic = {true, false, false, icon(EmbeddedIconId::kMicOff), icon(EmbeddedIconId::kMicOn)};
    return footer;
}

epaper_ui::TimelineListState Timeline(const std::string& label)
{
    using namespace epaper_ui;
    TimelineListState timeline = {};
    timeline.item_label_plural = label;
    timeline.groups.push_back({"TODAY", {{
        {.time_text = "09:12", .minute_seconds_text = "03:18", .tag_text = "Work"},
        "Plan the next product review", false, {ListItemAccessoryKind::kIcon, false,
        project_assets::GetIcon(EmbeddedIconId::kIdea)}, {}
    }, {
        {.time_text = "08:40", .minute_seconds_text = "01:54", .tag_text = "Personal"},
        "Call back the design team"
    }}});
    timeline.visible_group_index = 0;
    timeline.selected_item_index = 0;
    return timeline;
}

void Draw(const std::string& screen, int selection, const std::string& text,
          const std::string& overlay, uint8_t* fb)
{
    using namespace epaper_ui;
    const StatusBarState status = Status();
    GlobalFooterState footer = Footer();
    footer.home.selected = screen == "home" || screen == "dashboard";
    footer.settings.selected = screen == "settings";
    footer.wifi.selected = screen == "wifi";

    if (screen == "home" || screen == "dashboard") {
        DashboardPageState state = {};
        state.menu.selected_index = selection;
        state.welcome_message.current_date = {"Monday", "Jun 24, 2026"};
        state.welcome_message.title_text = "Good morning, Alex";
        state.current_progress = {"Today's progress", "3 of 7", 43};
        state.menu.shows_follow_up_badge = true;
        state.menu.follow_up_badge_text = "2";
        DrawDashboardPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "onboarding") {
        OnboardingPageState state = {};
        const int slide = std::clamp(selection, 0, 5);
        constexpr const char* titles[] = {"Welcome to Folloup", "Capture in a tap", "Navigate with keys", "Sleep & power", "Summaries with OpenRouter", "Notes, Todos & Follow-ups"};
        constexpr const char* bodies[] = {"Your pocket voice notebook. Capture thoughts out loud and let Folloup keep them organized.", "Press the mic to record a note, an idea, or a task. Everything is saved straight to the SD card.", "Key 1 selects, key 2 navigates up, and key 3 navigates down.", "The device sleeps when inactive. Hold keys 1 and 2 to shut it down.", "Connect OpenRouter and let Folloup transcribe your recordings and summarize your day.", "Recordings are grouped by day. Browse them as Notes, mark tasks as Todos, and pin anything as a follow-up."};
        state.carousel = {6, slide, true};
        state.slide_title = titles[slide];
        state.slide_body = bodies[slide];
        state.slide_image = project_assets::GetImage(static_cast<EmbeddedImageId>(
            slide));
        DrawOnboardingPage(fb, kHeight, kWidth, kWidth, kHeight, state, status);
    } else if (screen == "follow-up" || screen == "followup") {
        FollowUpPageState state = {};
        state.navigation_focus_index = selection;
        state.timeline = Timeline("Follow-ups");
        state.timeline.active_group_index = 0;
        state.timeline.selected_item_index = std::clamp(selection, 0, 1);
        state.timeline.groups[0].items[static_cast<size_t>(state.timeline.selected_item_index)].selected = true;
        if (!text.empty()) state.timeline.groups[0].items[0].body_text = text;
        DrawFollowUpPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "notes") {
        NotesPageState state = {};
        state.navigation_focus_index = selection;
        state.timeline = Timeline("Notes");
        state.timeline.active_group_index = 0;
        state.timeline.selected_item_index = std::clamp(selection, 0, 1);
        state.timeline.groups[0].items[static_cast<size_t>(state.timeline.selected_item_index)].selected = true;
        if (!text.empty()) state.timeline.groups[0].items[0].body_text = text;
        DrawNotesPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "todos" || screen == "to-dos") {
        TodosPageState state = {};
        state.navigation_focus_index = selection;
        state.timeline = Timeline("Todos");
        state.timeline.active_group_index = 0;
        state.timeline.selected_item_index = std::clamp(selection, 0, 1);
        state.timeline.groups[0].items[static_cast<size_t>(state.timeline.selected_item_index)].selected = true;
        state.timeline.groups[0].items[0].accessory = {ListItemAccessoryKind::kCheckbox, true};
        DrawTodosPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "wifi") {
        WifiPageState state = {};
        state.network_list.status = NetworkListStatus::kNetworksFound;
        state.network_list.networks = {{"Followup Lab", false, true, false, NetworkSignalStrength::kStrong},
                                       {"Home office", false, false, true, NetworkSignalStrength::kMedium}};
        state.password_input.value_text = "correct-horse";
        state.scan_button = {"Scan", false};
        state.connect_button = {"Connect", selection == 1};
        DrawWifiPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "settings") {
        SettingsPageState state = {};
        state.navigation_focus_index = selection;
        state.wifi_toggle = {"WiFi", selection == 1 ? ToggleVisualState::kOff : ToggleVisualState::kOn};
        state.access_point_toggle = {"Access point", selection == 2 ? ToggleVisualState::kOn : ToggleVisualState::kOff};
        state.storage_status = {true, "Ready", 12};
        state.enable_otg_button = {"Enable USB", selection == 3};
        state.format_sd_button = {"Format SD", selection == 4};
        state.manual_onboarding_button = {"Show onboarding", selection == 5};
        DrawSettingsPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "summarize") {
        SummarizePageState state = {};
        state.get_summary_button = {"Get summary", selection == 0};
        DrawSummarizePage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "vibe-check" || screen == "vibe") {
        VibeCheckPageState state = {};
        state.message_text = "Your focus is steady";
        DrawVibeCheckPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else if (screen == "details") {
        DetailsPageState state = {};
        state.recording_header.time_text = "09:12";
        state.recording_header.minute_seconds_text = "03:18";
        state.recording_header.tag_text = "Work";
        state.scroll_container.content_text = text.empty() ? "Plan the next product review and assign owners." : text;
        state.back_button = {"Back", selection == 0};
        DrawDetailsPage(fb, kHeight, kWidth, kWidth, kHeight, state, status, footer);
    } else {
        throw std::invalid_argument("unknown screen: " + screen);
    }
    if (overlay == "toast") {
        DrawToast(fb, kHeight, kWidth, kWidth, kHeight,
                  {true, text.empty() ? "Saved" : text,
                   project_assets::GetIcon(EmbeddedIconId::kCheck), true, selection == 0});
    } else if (overlay == "keyboard") {
        KeyboardState keyboard = {};
        keyboard.visible = true;
        keyboard.title_text = "Edit note";
        keyboard.input = {"Note", "Type here", text, true, true, false, true, -1, 64,
                          KeyboardInputSubmitStyle::kSave};
        DrawKeyboard(fb, kHeight, kWidth, kWidth, kHeight, keyboard, {});
    } else if (overlay != "none" && !overlay.empty()) {
        throw std::invalid_argument("unknown overlay: " + overlay);
    }
}

void WritePbm(const std::string& path, const std::vector<uint8_t>& raw)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open output: " + path);
    out << "P4\n" << kWidth << ' ' << kHeight << "\n";
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; x += 8) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8 && x + bit < kWidth; ++bit) {
                const int raw_x = y;
                const int raw_y = kWidth - 1 - (x + bit);
                const uint8_t mask = static_cast<uint8_t>(0x80U >> (raw_x & 7));
                const bool black = (raw[static_cast<size_t>(raw_y) * (kHeight / 8) + raw_x / 8] & mask) == 0;
                if (black) byte |= static_cast<uint8_t>(0x80U >> bit);
            }
            out.put(static_cast<char>(byte));
        }
    }
}
}  // namespace

int main(int argc, char** argv)
{
    std::string screen;
    std::string output;
    std::string text;
    std::string overlay = "none";
    bool selection_set = false;
    int selection = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--screen" && i + 1 < argc) screen = argv[++i];
        else if (arg == "--selection" && i + 1 < argc) {
            selection_set = true;
            if (!ParseInt(argv[++i], &selection)) {
                std::cerr << "--selection must be an integer\n";
                return 2;
            }
        }
        else if (arg == "--output" && i + 1 < argc) output = argv[++i];
        else if (arg == "--text" && i + 1 < argc) text = argv[++i];
        else if (arg == "--overlay" && i + 1 < argc) overlay = argv[++i];
        else if (arg == "--help") {
            std::cout << "renderer --screen <name> --selection N --output <file.pbm>\n"
                      << "screens: onboarding home dashboard follow-up notes todos wifi settings summarize vibe-check details\n"
                      << "output: portrait PBM P4, 480x800; optional --text TEXT --overlay none|toast|keyboard\n";
            return 0;
        } else if (arg == "--list") {
            std::cout << R"({"screens":[{"id":"home","label":"Home","selections":5},{"id":"onboarding","label":"Onboarding","selections":6},{"id":"settings","label":"Settings","selections":6},{"id":"wifi","label":"WiFi","selections":2},{"id":"notes","label":"Notes","selections":2},{"id":"todos","label":"Todos","selections":2},{"id":"followup","label":"Follow-up","selections":2},{"id":"summarize","label":"Summarize","selections":1},{"id":"vibe-check","label":"Vibe check","selections":1},{"id":"details","label":"Details","selections":1}]})" << '\n';
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (screen.empty() || output.empty()) { std::cerr << "--screen and --output are required\n"; return 2; }
    const int selection_count = SelectionCount(screen);
    if (selection_count == 0) { std::cerr << "unknown screen: " << screen << '\n'; return 2; }
    if (selection_set && (selection < 0 || selection >= selection_count)) {
        std::cerr << "--selection must be in [0," << (selection_count - 1) << "] for " << screen << '\n';
        return 2;
    }
    try {
        std::vector<uint8_t> framebuffer(static_cast<size_t>(kHeight) * (kWidth / 8), 0xFF);
        Draw(screen, selection, text, overlay, framebuffer.data());
        WritePbm(output, framebuffer);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
