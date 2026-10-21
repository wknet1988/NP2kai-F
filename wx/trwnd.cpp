/* === Desktop translation window for wx port === */

#include "trwnd.h"
#include <translate/translate.h>

wxBEGIN_EVENT_TABLE(TrWindow, wxFrame)
	EVT_TIMER(wxID_ANY, TrWindow::OnTimer)
	EVT_CLOSE(TrWindow::OnClose)
wxEND_EVENT_TABLE()

TrWindow::TrWindow(wxWindow *parent)
	: wxFrame(parent, wxID_ANY, wxT("翻译窗 - Translation"), wxDefaultPosition, wxSize(440, 640))
{
	m_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
	                       wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
	m_text->SetFont(wxFont(wxFontInfo(11).Family(wxFONTFAMILY_DEFAULT)));
	m_timer = new wxTimer(this);
	m_timer->Start(300);
	UpdateContent();
}

TrWindow::~TrWindow()
{
	if (m_timer) {
		delete m_timer;
		m_timer = nullptr;
	}
}

void TrWindow::OnTimer(wxTimerEvent &WXUNUSED(evt))
{
	UpdateContent();
}

void TrWindow::OnClose(wxCloseEvent &WXUNUSED(evt))
{
	/* Hide instead of destroying, so the menu can re-show it.  The parent
	 * (main frame) destroys child windows on exit automatically. */
	Show(false);
}

void TrWindow::UpdateContent(void)
{
	int n;
	int start;
	int i;
	char orig[TR_SRC_MAX];
	char tran[TR_TRANS_MAX];

	if (!m_text) return;

	n = translate_get_window_count();
	if (n <= 0) {
		m_text->SetValue(wxT("(无文本 / no text captured)"));
		return;
	}

	start = 0;
	if (n > 300) start = n - 300;

	wxString s;
	for (i = start; i < n; i++) {
		translate_get_window_line(i, orig, tran, TR_SRC_MAX);
		wxString o = wxString::FromUTF8(orig);
		wxString t = wxString::FromUTF8(tran);
		if (t.IsEmpty())
			s += o + wxT("\n");
		else
			s += o + wxT("  →  ") + t + wxT("\n");
	}
	m_text->SetValue(s);
	m_text->ShowPosition(m_text->GetLastPosition());
}
