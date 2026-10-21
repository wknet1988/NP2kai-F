/* === Desktop translation window for wx port === */

#ifndef NP2_WX_TRWND_H
#define NP2_WX_TRWND_H

#include <wx/wx.h>
#include <wx/timer.h>

class TrWindow : public wxFrame
{
public:
	TrWindow(wxWindow *parent);
	virtual ~TrWindow();

	void UpdateContent(void);

private:
	wxTextCtrl	*m_text;
	wxTimer		*m_timer;

	void OnTimer(wxTimerEvent &evt);
	void OnClose(wxCloseEvent &evt);

	wxDECLARE_EVENT_TABLE();
};

#endif /* NP2_WX_TRWND_H */
