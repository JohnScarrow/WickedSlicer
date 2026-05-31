#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/stattext.h>

#include <boost/thread.hpp>

wxDECLARE_EVENT(wxEVT_AI_TOKEN, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_AI_DONE,  wxCommandEvent);
wxDECLARE_EVENT(wxEVT_AI_ERROR, wxCommandEvent);

namespace Slic3r {
namespace GUI {

struct ChatMessage {
    std::string role;    // "user" or "assistant"
    std::string content;
};

class AIAssistantPanel : public wxPanel
{
public:
    explicit AIAssistantPanel(wxWindow* parent);
    ~AIAssistantPanel();

    // Call when the user opens/switches projects so history updates
    void on_project_changed();

private:
    wxTextCtrl*   m_conversation{nullptr};
    wxTextCtrl*   m_input{nullptr};
    wxButton*     m_send_btn{nullptr};
    wxButton*     m_clear_btn{nullptr};

    boost::thread    m_thread;
    std::atomic_bool m_cancel{false};
    bool             m_streaming{false};
    std::string      m_current_response;

    std::vector<ChatMessage> m_history;
    static constexpr size_t  MAX_HISTORY = 40;

    // Preset context
    std::string build_context_block() const;

    // Project / persistence
    std::string get_project_key() const;
    void        load_history();
    void        save_history() const;
    void        render_history();

    void append(const wxString& text);
    void set_streaming(bool s);
    void send_to_claude(const std::string& user_text);

    void on_send(wxCommandEvent&);
    void on_clear(wxCommandEvent&);
    void on_key_down(wxKeyEvent&);

    void on_ai_token(wxCommandEvent&);
    void on_ai_done(wxCommandEvent&);
    void on_ai_error(wxCommandEvent&);

    wxDECLARE_EVENT_TABLE();
};

} // namespace GUI
} // namespace Slic3r
