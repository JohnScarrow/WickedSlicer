#include "AIAssistantPanel.hpp"

#include <wx/sizer.h>
#include <wx/font.h>
#include <wx/msgdlg.h>

#include <fstream>
#include <sstream>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "nlohmann/json.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

using nlohmann::json;

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// Custom event definitions
// ---------------------------------------------------------------------------
wxDEFINE_EVENT(wxEVT_AI_TOKEN, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_AI_DONE,  wxCommandEvent);
wxDEFINE_EVENT(wxEVT_AI_ERROR, wxCommandEvent);

// ---------------------------------------------------------------------------
// Event table
// ---------------------------------------------------------------------------
wxBEGIN_EVENT_TABLE(AIAssistantPanel, wxPanel)
    EVT_COMMAND(wxID_ANY, wxEVT_AI_TOKEN, AIAssistantPanel::on_ai_token)
    EVT_COMMAND(wxID_ANY, wxEVT_AI_DONE,  AIAssistantPanel::on_ai_done)
    EVT_COMMAND(wxID_ANY, wxEVT_AI_ERROR, AIAssistantPanel::on_ai_error)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

static const std::string HISTORY_SECTION = "ai_chat_history";
static const std::string HISTORY_KEY     = "default"; // single key — always loads on restart

std::string cfg_str(const DynamicPrintConfig& cfg, const std::string& key)
{
    const ConfigOption* opt = cfg.option(key);
    if (!opt) return {};
    try { return opt->serialize(); } catch (...) { return {}; }
}

// Try to read a JSON file from disk; returns empty json object on failure.
json read_json_file(const std::string& path, std::string& err_out)
{
    if (path.empty()) { err_out = "no file path"; return {}; }
    std::ifstream f(path);
    if (!f.is_open()) { err_out = "cannot open: " + path; return {}; }
    try {
        json j; f >> j; return j;
    } catch (const std::exception& e) {
        err_out = std::string("parse error: ") + e.what();
        return {};
    }
}

// Convert a JSON value to the string format DynamicConfig::set_deserialize expects.
std::string json_to_serialize_str(const json& val)
{
    if (val.is_boolean())  return val.get<bool>() ? "1" : "0";
    if (val.is_string())   return val.get<std::string>();
    if (val.is_number())   return val.dump(); // "0.2", "3", etc.
    if (val.is_array()) {
        std::string s;
        for (size_t i = 0; i < val.size(); ++i) {
            if (i > 0) s += ',';
            s += json_to_serialize_str(val[i]);
        }
        return s;
    }
    return val.dump();
}

// Determine which preset type owns a given file path.
Preset::Type preset_type_from_path(const std::string& path)
{
    if (path.find("/process/")  != std::string::npos) return Preset::TYPE_PRINT;
    if (path.find("/filament/") != std::string::npos) return Preset::TYPE_FILAMENT;
    return Preset::TYPE_PRINTER;
}

// Apply a single WICKED_CHANGE to the in-memory preset config (no disk write).
// The user can review and save via the normal preset Save button.
// Returns true on success, sets err_out on failure.
bool apply_preset_change_in_memory(const std::string& file_path,
                                   const std::string& key,
                                   const json&        value,
                                   std::string&       err_out)
{
    // Verify the key exists in the on-disk JSON (catches Claude inventing key names).
    std::string read_err;
    json preset = read_json_file(file_path, read_err);
    if (preset.empty()) {
        err_out = "Could not read preset file: " + (read_err.empty() ? file_path : read_err);
        return false;
    }
    if (!preset.contains(key)) {
        std::ostringstream known;
        int n = 0;
        for (auto it = preset.begin(); it != preset.end() && n < 30; ++it, ++n)
            known << " " << it.key();
        err_out = "Key '" + key + "' does not exist in the preset.\n"
                  "Available keys (first 30):" + known.str();
        return false;
    }

    // Find the Tab that owns this preset type.
    Preset::Type type = preset_type_from_path(file_path);
    Tab* tab = wxGetApp().get_tab(type);
    if (!tab) {
        err_out = "Settings tab not available for this preset type.";
        return false;
    }

    DynamicPrintConfig* cfg = tab->get_config();
    if (!cfg) {
        err_out = "Tab has no active config.";
        return false;
    }

    // Apply value to the live in-memory config.
    std::string str_val = json_to_serialize_str(value);
    try {
        cfg->set_deserialize_strict(key, str_val);
    } catch (const std::exception& e) {
        err_out = std::string("Could not apply value '") + str_val + "': " + e.what();
        return false;
    }

    // Show dirty indicator on the preset dropdown and refresh UI widgets.
    tab->update_dirty();
    tab->reload_config();

    return true;
}

// Scan response text for WICKED_CHANGE:{...} markers.
struct ChangeRequest { std::string file, key; json value; };
std::vector<ChangeRequest> parse_change_requests(const std::string& text)
{
    std::vector<ChangeRequest> out;
    const std::string marker = "WICKED_CHANGE:";
    size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        pos += marker.size();
        size_t brace = text.find('{', pos);
        if (brace == std::string::npos) break;
        int depth = 0;
        size_t end = brace;
        for (; end < text.size(); ++end) {
            if      (text[end] == '{') ++depth;
            else if (text[end] == '}') { if (--depth == 0) break; }
        }
        if (depth != 0) break;
        try {
            json obj = json::parse(text.substr(brace, end - brace + 1));
            if (obj.contains("file") && obj.contains("key") && obj.contains("value")) {
                ChangeRequest req;
                req.file  = obj["file"].get<std::string>();
                req.key   = obj["key"].get<std::string>();
                req.value = obj["value"];
                out.push_back(std::move(req));
            }
        } catch (...) {}
        pos = end + 1;
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
AIAssistantPanel::AIAssistantPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(wxColour(20, 20, 22));

    // ---- Conversation display -----------------------------------------------
    m_conversation = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxBORDER_NONE);
    m_conversation->SetBackgroundColour(wxColour(20, 20, 22));
    m_conversation->SetForegroundColour(wxColour(220, 220, 220));
    {
        wxFont f = m_conversation->GetFont();
        f.SetPointSize(10);
        m_conversation->SetFont(f);
    }

    // ---- Input area ---------------------------------------------------------
    m_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 80),
        wxTE_MULTILINE | wxBORDER_SIMPLE);
    m_input->SetBackgroundColour(wxColour(35, 35, 38));
    m_input->SetForegroundColour(wxColour(220, 220, 220));
    m_input->SetHint("Ask a 3D printing question... (Enter to send, Shift+Enter for newline)");
    m_input->Bind(wxEVT_KEY_DOWN, &AIAssistantPanel::on_key_down, this);

    // ---- Buttons ------------------------------------------------------------
    m_send_btn  = new wxButton(this, wxID_ANY, "Send");
    m_clear_btn = new wxButton(this, wxID_ANY, "Clear");

    // ---- Layout -------------------------------------------------------------
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer(1);
    btn_sizer->Add(m_clear_btn, 0, wxRIGHT, 4);
    btn_sizer->Add(m_send_btn,  0);

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_conversation, 1, wxEXPAND | wxALL, 2);
    root->Add(m_input,        0, wxEXPAND | wxLEFT | wxRIGHT, 2);
    root->Add(btn_sizer,      0, wxEXPAND | wxALL, 2);
    SetSizer(root);

    // ---- Button events ------------------------------------------------------
    m_send_btn ->Bind(wxEVT_BUTTON, &AIAssistantPanel::on_send,  this);
    m_clear_btn->Bind(wxEVT_BUTTON, &AIAssistantPanel::on_clear, this);

    // ---- Load persisted history.
    // History is always stored under a single "default" key so it loads
    // reliably regardless of whether a project file is open at startup.
    load_history();
    if (m_history.empty())
        append("AI Assistant ready.\nType a message and press Enter (or Send).\n\n");
    else
        render_history();
}

// ---------------------------------------------------------------------------
AIAssistantPanel::~AIAssistantPanel()
{
    m_cancel.store(true);
    if (m_thread.joinable())
        m_thread.join();
}

// ---------------------------------------------------------------------------
void AIAssistantPanel::on_project_changed()
{
    // Project context updates automatically via build_context_block() on
    // every send — no storage key change needed.
    (void)this;
}

// ---------------------------------------------------------------------------
// Build the preset context block injected into every prompt
// ---------------------------------------------------------------------------
std::string AIAssistantPanel::build_context_block() const
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle) return {};

    std::string ctx = "=== Current WickedSlicer Settings ===\n";
    ctx += "Data directory : " + Slic3r::data_dir() + "\n\n";

    // ---------- Printer ------------------------------------------------------
    const Preset& printer = bundle->printers.get_edited_preset();
    ctx += "[Printer preset]\n";
    ctx += "  Name : " + printer.name + "\n";
    ctx += "  File : " + printer.file + "\n";
    const DynamicPrintConfig& pc = printer.config;
    if (auto v = cfg_str(pc, "nozzle_diameter"); !v.empty()) ctx += "  Nozzle diameter : " + v + " mm\n";
    if (auto v = cfg_str(pc, "bed_type");         !v.empty()) ctx += "  Bed type        : " + v + "\n";
    ctx += "\n";

    // ---------- Process ------------------------------------------------------
    const Preset& process = bundle->prints.get_edited_preset();
    ctx += "[Process preset]\n";
    ctx += "  Name : " + process.name + "\n";
    ctx += "  File : " + process.file + "\n";
    const DynamicPrintConfig& rc = process.config;
    struct F { const char* label; const char* key; const char* unit; };
    for (auto& f : std::initializer_list<F>{
        {"Layer height",         "layer_height",         " mm"},
        {"Initial layer height", "initial_layer_height", " mm"},
        {"Infill density",       "fill_density",         "%"},
        {"Infill pattern",       "fill_pattern",         ""},
        {"Wall loops",           "wall_loops",           ""},
        {"Top solid layers",     "top_shell_layers",     ""},
        {"Bottom solid layers",  "bottom_shell_layers",  ""},
        {"Supports enabled",     "enable_support",       ""},
        {"Support type",         "support_type",         ""},
        {"Outer wall speed",     "outer_wall_speed",     " mm/s"},
        {"Sparse infill speed",  "sparse_infill_speed",  " mm/s"},
    }) {
        if (auto v = cfg_str(rc, f.key); !v.empty())
            ctx += std::string("  ") + f.label + " : " + v + f.unit + "\n";
    }
    ctx += "\n";

    // ---------- Full process preset JSON (exact key names for WICKED_CHANGE) --
    {
        std::string err;
        json j = read_json_file(process.file, err);
        if (!j.empty()) {
            ctx += "[Process preset JSON — these are the EXACT key names to use in WICKED_CHANGE]\n";
            ctx += "File: " + process.file + "\n";
            ctx += j.dump(2) + "\n\n";
        } else if (!err.empty()) {
            ctx += "[Process preset JSON unavailable: " + err + "]\n\n";
        }
    }

    // ---------- Filament(s) --------------------------------------------------
    const std::vector<std::string>& fil_presets = bundle->filament_presets;
    for (size_t i = 0; i < fil_presets.size() && i < 4; ++i) {
        const Preset* fp = bundle->filaments.find_preset(fil_presets[i]);
        if (!fp) continue;
        std::string label = fil_presets.size() == 1 ? "Filament preset" :
                            "Filament " + std::to_string(i + 1);
        ctx += "[" + label + "]\n";
        ctx += "  Name : " + fp->name + "\n";
        ctx += "  File : " + fp->file + "\n";
        const DynamicPrintConfig& fc = fp->config;
        if (auto v = cfg_str(fc, "filament_type");      !v.empty()) ctx += "  Material   : " + v + "\n";
        if (auto v = cfg_str(fc, "nozzle_temperature"); !v.empty()) ctx += "  Print temp : " + v + " °C\n";
        if (auto v = cfg_str(fc, "bed_temperature");    !v.empty()) ctx += "  Bed temp   : " + v + " °C\n";
        ctx += "\n";
    }

    // ---------- Project name -------------------------------------------------
    if (wxGetApp().plater()) {
        wxString proj = wxGetApp().plater()->get_project_name();
        if (!proj.IsEmpty() && proj != "Untitled")
            ctx += "Active project : " + proj.ToStdString() + "\n\n";
    }

    // ---------- Change protocol ----------------------------------------------
    ctx +=
        "=== HOW TO MODIFY SETTINGS ===\n"
        "You CAN suggest setting changes. When the user asks you to change a setting:\n"
        "1. Find the EXACT key name in the preset JSON shown above.\n"
        "2. Output a WICKED_CHANGE marker — the app intercepts it, asks the user\n"
        "   for permission, and applies it to the LIVE IN-MEMORY config (no file\n"
        "   is written). The dirty indicator (pencil icon) will appear on the preset\n"
        "   dropdown. The user must click Save in the preset dropdown to keep the\n"
        "   change permanently. Format:\n\n"
        "   WICKED_CHANGE:{\"file\":\"/absolute/path.json\",\"key\":\"exact_key_name\",\"value\":new_value}\n\n"
        "IMPORTANT rules:\n"
        "- ONLY use keys that already exist in the JSON. Do NOT invent new key names.\n"
        "- The key name must match exactly (case-sensitive) what is in the JSON file.\n"
        "- value must be valid JSON (number, string, boolean, or array).\n"
        "- One WICKED_CHANGE per setting; multiple changes = multiple markers.\n"
        "- After applying, tell the user to save the preset if they want to keep it.\n"
        "=====================================\n\n";

    return ctx;
}

// ---------------------------------------------------------------------------
// Persistence — always uses a single "default" key so history loads on
// every startup regardless of which project (if any) is currently open.
// ---------------------------------------------------------------------------
std::string AIAssistantPanel::get_project_key() const
{
    return HISTORY_KEY; // always "default"
}

void AIAssistantPanel::load_history()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg || !cfg->has(HISTORY_SECTION, HISTORY_KEY)) return;

    std::string raw = cfg->get(HISTORY_SECTION, HISTORY_KEY);
    if (raw.empty()) return;

    try {
        json arr = json::parse(raw);
        m_history.clear();
        for (const auto& item : arr) {
            ChatMessage msg;
            msg.role    = item.at("role").get<std::string>();
            msg.content = item.at("content").get<std::string>();
            m_history.push_back(std::move(msg));
        }
    } catch (...) {
        m_history.clear();
    }
}

void AIAssistantPanel::save_history() const
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;

    json arr = json::array();
    for (const auto& msg : m_history)
        arr.push_back({{"role", msg.role}, {"content", msg.content}});

    cfg->set(HISTORY_SECTION, HISTORY_KEY, arr.dump());
    cfg->save();
}

void AIAssistantPanel::render_history()
{
    m_conversation->Clear();
    for (const auto& msg : m_history) {
        if (msg.role == "user")
            append("You: " + msg.content + "\n\n");
        else
            append("Assistant: " + msg.content + "\n\n");
    }
}

// ---------------------------------------------------------------------------
void AIAssistantPanel::append(const wxString& text)
{
    m_conversation->AppendText(text);
}

void AIAssistantPanel::set_streaming(bool s)
{
    m_streaming = s;
    m_send_btn->Enable(!s);
    m_input->Enable(!s);
}

// ---------------------------------------------------------------------------
// Background subprocess
// ---------------------------------------------------------------------------
void AIAssistantPanel::send_to_claude(const std::string& user_text)
{
    std::string context = build_context_block();

    std::string prompt;
    prompt += "You are an expert 3D printing assistant embedded in WickedSlicer "
              "(an OrcaSlicer fork). Help the user troubleshoot print quality, "
              "slicer settings, and hardware issues. Be concise.\n\n";
    prompt += context;

    size_t start = m_history.size() > MAX_HISTORY ? m_history.size() - MAX_HISTORY : 0;
    for (size_t i = start; i < m_history.size(); ++i) {
        const auto& msg = m_history[i];
        prompt += (msg.role == "user" ? "Human: " : "Assistant: ") + msg.content + "\n\n";
    }
    prompt += "Human: " + user_text + "\n\nAssistant:";

    m_cancel.store(false);

    m_thread = boost::thread([this, prompt]() {
        std::string tmp_path = "/tmp/wicked_ai_prompt_" + std::to_string(getpid()) + ".txt";

        {
            FILE* f = fopen(tmp_path.c_str(), "w");
            if (!f) {
                auto* ev = new wxCommandEvent(wxEVT_AI_ERROR);
                ev->SetString("Failed to create temp prompt file.");
                wxQueueEvent(this, ev);
                return;
            }
            fwrite(prompt.data(), 1, prompt.size(), f);
            fclose(f);
        }

        std::string cmd = "claude --print < " + tmp_path + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");

        if (!pipe) {
            remove(tmp_path.c_str());
            auto* ev = new wxCommandEvent(wxEVT_AI_ERROR);
            ev->SetString("Could not launch 'claude'. Is it installed? (npm install -g @anthropic-ai/claude-code)");
            wxQueueEvent(this, ev);
            return;
        }

        char buf[256];
        while (!m_cancel.load() && fgets(buf, sizeof(buf), pipe)) {
            auto* ev = new wxCommandEvent(wxEVT_AI_TOKEN);
            ev->SetString(wxString::FromUTF8(buf));
            wxQueueEvent(this, ev);
        }

        int exit_code = pclose(pipe);
        remove(tmp_path.c_str());

        if (m_cancel.load()) return;

        if (exit_code != 0) {
            auto* ev = new wxCommandEvent(wxEVT_AI_ERROR);
            ev->SetString(wxString::Format("claude exited with code %d", exit_code));
            wxQueueEvent(this, ev);
        } else {
            wxQueueEvent(this, new wxCommandEvent(wxEVT_AI_DONE));
        }
    });
}

// ---------------------------------------------------------------------------
// UI event handlers
// ---------------------------------------------------------------------------
void AIAssistantPanel::on_send(wxCommandEvent&)
{
    if (m_streaming) return;

    wxString text = m_input->GetValue().Trim(true).Trim(false);
    if (text.IsEmpty()) return;

    std::string user_text = text.ToStdString();
    m_input->Clear();

    append("You: " + user_text + "\n\n");
    append("Assistant: ");

    m_history.push_back({"user", user_text});
    m_current_response.clear();

    set_streaming(true);
    send_to_claude(user_text);
}

void AIAssistantPanel::on_clear(wxCommandEvent&)
{
    m_cancel.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_cancel.store(false);

    m_history.clear();
    m_current_response.clear();

    AppConfig* cfg = wxGetApp().app_config;
    if (cfg) {
        cfg->erase(HISTORY_SECTION, HISTORY_KEY);
        cfg->save();
    }

    m_conversation->Clear();
    set_streaming(false);
    append("Conversation cleared.\n\n");
}

void AIAssistantPanel::on_key_down(wxKeyEvent& e)
{
    if (e.GetKeyCode() == WXK_RETURN && !e.ShiftDown()) {
        wxCommandEvent dummy;
        on_send(dummy);
    } else {
        e.Skip();
    }
}

// ---------------------------------------------------------------------------
// AI event handlers
// ---------------------------------------------------------------------------
void AIAssistantPanel::on_ai_token(wxCommandEvent& e)
{
    m_current_response += e.GetString().ToStdString();
    append(e.GetString());
}

void AIAssistantPanel::on_ai_done(wxCommandEvent&)
{
    append("\n\n");

    // --- Process any WICKED_CHANGE requests in the response -----------------
    auto changes = parse_change_requests(m_current_response);
    bool any_change_applied = false;

    for (const auto& req : changes) {
        wxString msg = wxString::Format(
            "The AI Assistant wants to make the following change:\n\n"
            "File:  %s\n"
            "Key:   %s\n"
            "Value: %s\n\n"
            "Apply this change?",
            req.file, req.key,
            wxString::FromUTF8(req.value.dump()));

        int answer = wxMessageBox(msg, "AI Assistant — Proposed Change",
                                  wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
        if (answer == wxYES) {
            std::string err;
            if (apply_preset_change_in_memory(req.file, req.key, req.value, err)) {
                append("[Setting changed: " + req.key + " = " + req.value.dump() + "]\n"
                       "[Use the preset Save button to keep this change permanently.]\n\n");
                any_change_applied = true;
            } else {
                // Show the error (including list of valid key names) so Claude
                // can self-correct on the next turn.
                append("[Change failed: " + err + "]\n\n");
            }
        } else {
            append("[Change declined: " + req.key + "]\n\n");
        }
    }
    (void)any_change_applied; // changes are already live via tab->reload_config()
    // ------------------------------------------------------------------------

    m_history.push_back({"assistant", m_current_response});
    m_current_response.clear();

    // Trim history to avoid unbounded growth
    while (m_history.size() > MAX_HISTORY * 2)
        m_history.erase(m_history.begin());

    save_history();
    set_streaming(false);

    if (m_thread.joinable())
        m_thread.detach();
}

void AIAssistantPanel::on_ai_error(wxCommandEvent& e)
{
    append("\n[Error] " + e.GetString() + "\n\n");
    set_streaming(false);

    if (m_thread.joinable())
        m_thread.detach();
}

} // namespace GUI
} // namespace Slic3r
