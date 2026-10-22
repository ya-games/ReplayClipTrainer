#include "pch.h"
#include "bakkesmod/wrappers/GuiManagerWrapper.h"
#include "ReplayClipTrainer.h"
#include "ClipFile.h"
#include "compatibility.h"
#include "UiStrings.h"
#include <Windows.h>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <string>

// The settings window and the on-screen overlays. Render() runs on the render thread: game-touching calls are deferred
// with SetTimeout (like the rest of the plugin). Everything is drawn with ImDrawList (own theme) in a Latin font and a
// Japanese font loaded through GuiManagerWrapper::LoadFont - AddFontFromFileTTF directly crashes BakkesMod.

namespace {

inline ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// ---- theme (graphite + amber / cyan) ----
const ImU32 kBg      = C(15, 18, 23, 248);
const ImU32 kPanel   = C(24, 29, 36);
const ImU32 kPanelHi = C(34, 41, 51);
const ImU32 kLine    = C(54, 64, 78);
const ImU32 kText    = C(232, 236, 242);
const ImU32 kDim     = C(138, 150, 164);
const ImU32 kAmber   = C(255, 176, 46);
const ImU32 kCyan    = C(70, 200, 232);
const ImU32 kRed     = C(240, 86, 86);
const ImU32 kGreen   = C(96, 214, 140);
const ImU32 kTrack   = C(48, 56, 66);
const ImU32 kBlue    = C(12, 136, 252);
const ImU32 kOrange  = C(252, 124, 12);

// baked pixel sizes of both fonts; text picks the nearest one (ReplayClipTrainer::kFontCount of them)
const int kFontPx[3] = { 18, 24, 32 };

using IconFn = void (*)(ImDrawList*, ImVec2, float, ImU32);

// ---- icons: simple vector shapes centred on c, s = nominal size ----
void IconPlay(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
	d->AddTriangleFilled(ImVec2(c.x - s * 0.32f, c.y - s * 0.5f), ImVec2(c.x - s * 0.32f, c.y + s * 0.5f), ImVec2(c.x + s * 0.5f, c.y), col);
}
void IconSave(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
	// arrow pointing down into a tray: "save to a file"
	d->AddLine(ImVec2(c.x, c.y - s * 0.5f), ImVec2(c.x, c.y + s * 0.05f), col, 2.6f);
	d->AddTriangleFilled(ImVec2(c.x - s * 0.3f, c.y - s * 0.1f), ImVec2(c.x + s * 0.3f, c.y - s * 0.1f), ImVec2(c.x, c.y + s * 0.26f), col);
	d->AddLine(ImVec2(c.x - s * 0.5f, c.y + s * 0.15f), ImVec2(c.x - s * 0.5f, c.y + s * 0.5f), col, 2.4f);
	d->AddLine(ImVec2(c.x - s * 0.5f, c.y + s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), col, 2.4f);
	d->AddLine(ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.15f), col, 2.4f);
}
void IconFolder(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
	d->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.38f), ImVec2(c.x - s * 0.05f, c.y - s * 0.2f), col, 2.f);
	d->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.24f), ImVec2(c.x + s * 0.5f, c.y + s * 0.4f), col, 2.f);
}
void IconLoop(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
	d->PathArcTo(c, s * 0.4f, 0.6f, 5.5f, 24);
	d->PathStroke(col, false, 2.2f);
	ImVec2 tip(c.x + std::cos(5.5f) * s * 0.4f, c.y + std::sin(5.5f) * s * 0.4f);
	d->AddTriangleFilled(ImVec2(tip.x - s * 0.18f, tip.y - s * 0.32f), ImVec2(tip.x + s * 0.3f, tip.y - s * 0.05f), ImVec2(tip.x - s * 0.22f, tip.y + s * 0.08f), col);
}

// ---- state that outlives a frame ----
int gTab = 0;
char gNameBuf[128] = "";
std::string gPendingDelete;   // clip whose "Confirm delete" is showing
bool gShowNoRetry = false;
int gSeekFrame = 0;           // replay position bar: the frame being dragged to
bool gSeeking = false;
bool gProgressShown = false; // conversion progress overlay: Cancel only reacts a moment after it appears
double gProgressStart = 0.0;
int gProgressFrame = -1;     // last replay frame seen while converting, and when it last changed
double gProgressFrameTime = 0.0;

// clip / folder dialog (replaces the tab body while open)
enum class Dlg { None, NewFolder, Clip, Folder };
Dlg gDlg = Dlg::None;
std::string gDlgTarget;               // clip path (UTF-8) or folder path relative to the clips root
std::string gDlgCurFolder;            // folder the clip is in now (relative), to mark it in the move list
std::string gDlgErr;                  // error key of the last failed action
std::vector<std::string> gDlgFolders; // every folder under the clips root, "" = root
char gDlgBuf[128] = "";
bool gDlgConfirmDelete = false;

} // namespace

// Friend of the plugin so it can read/write its state; all widgets and screens live here.
struct UiPainter
{
	ReplayClipTrainer* p;
	ImDrawList* dl = nullptr;
	float u = 1.f;    // UI scale (1 at 1080p with the window at 100%)
	bool ja = false;  // window language

	void Setup()
	{
		const ImVec2 disp = ImGui::GetIO().DisplaySize;
		u = std::clamp(disp.y / 1080.f, 0.75f, 1.6f) * std::clamp(p->uiScale_, 0.5f, 1.0f);
		ja = p->uiJa_ && p->jpReady_.load();
	}
	// overlays follow the screen size only, not the window-size slider
	void SetupOverlay()
	{
		u = ImGui::GetIO().DisplaySize.y / 1080.f;
		ja = p->uiJa_ && p->jpReady_.load();
	}

	const char* Tr(const ui::S& s) const { return ja ? s.ja : s.en; }

	// ---------- text ----------
	const ImFont* FontFor(const char* s, float px) const
	{
		bool nonAscii = false;
		for (const unsigned char* c = (const unsigned char*)s; *c; ++c) if (*c >= 0x80) { nonAscii = true; break; }
		ImFont* const* set = nonAscii ? p->fontsJa_ : p->fonts_;
		int best = 0;
		for (int i = 1; i < ReplayClipTrainer::kFontCount; ++i) if (std::fabs(kFontPx[i] - px) < std::fabs(kFontPx[best] - px)) best = i;
		return set[best];
	}
	ImVec2 TextSize(const char* s, float px, float wrap = 0.f) const
	{
		const ImFont* f = FontFor(s, px);
		return f ? f->CalcTextSizeA(px, FLT_MAX, wrap, s) : ImGui::CalcTextSize(s, nullptr, false, wrap);
	}
	void TextOn(ImDrawList* d, ImVec2 pos, const char* s, float px, ImU32 col, float wrap = 0.f) const
	{
		const ImFont* f = FontFor(s, px);
		if (f) d->AddText(f, px, pos, col, s, nullptr, wrap);
		else d->AddText(pos, col, s);
	}
	void Text(ImVec2 pos, const char* s, float px, ImU32 col, float wrap = 0.f) const { TextOn(dl, pos, s, px, col, wrap); }
	// wrapped paragraph; returns its height
	float Para(ImVec2 pos, const char* s, float px, ImU32 col, float wrap) const
	{
		Text(pos, s, px, col, wrap);
		return TextSize(s, px, wrap).y;
	}

	// ---------- widgets ----------
	// allowOverlap: let buttons submitted afterwards on top of this one take the hover and the click
	bool Hit(const char* id, ImVec2 a, ImVec2 sz, bool* hov = nullptr, bool* act = nullptr, bool allowOverlap = false) const
	{
		ImGui::SetCursorScreenPos(a);
		bool clicked = ImGui::InvisibleButton(id, sz);
		if (allowOverlap) ImGui::SetItemAllowOverlap();
		if (hov) *hov = ImGui::IsItemHovered();
		if (act) *act = ImGui::IsItemActive();
		return clicked;
	}

	// big action card: title + description + icon. enabled = false greys it out and makes it inert
	bool Card(const char* id, ImVec2 a, ImVec2 sz, const ui::S& title, const ui::S& desc, IconFn icon, bool accent, bool enabled)
	{
		bool hov = false, act = false, clicked = false;
		if (enabled) clicked = Hit(id, a, sz, &hov, &act);
		ImVec2 b(a.x + sz.x, a.y + sz.y);
		const ImU32 ac = enabled ? (accent ? kAmber : kCyan) : kTrack;
		dl->AddRectFilled(a, b, act ? kPanelHi : (hov ? C(30, 37, 46) : kPanel), 4.f * u);
		dl->AddRect(a, b, hov ? ac : kLine, 4.f * u, ImDrawCornerFlags_All, hov ? 2.f : 1.f);
		dl->AddRectFilled(a, ImVec2(a.x + 4.f * u, b.y), ac, 4.f * u, ImDrawCornerFlags_Left);
		Text(ImVec2(a.x + 18.f * u, a.y + 11.f * u), Tr(title), 19.f * u, enabled ? kText : kDim);
		Text(ImVec2(a.x + 18.f * u, a.y + 38.f * u), Tr(desc), 13.f * u, kDim, sz.x - 78.f * u);
		icon(dl, ImVec2(b.x - 34.f * u, a.y + sz.y * 0.5f), 24.f * u, ac);
		return clicked;
	}

	// style: 0 outline, 1 filled amber, 2 outline red
	bool Button(const char* id, ImVec2 a, ImVec2 sz, const char* label, int style = 0, bool enabled = true, float px = 16.f)
	{
		bool hov = false, act = false, clicked = false;
		if (enabled) clicked = Hit(id, a, sz, &hov, &act);
		ImVec2 b(a.x + sz.x, a.y + sz.y);
		const ImU32 edge = style == 2 ? kRed : (style == 1 ? kAmber : kLine);
		if (!enabled) {
			dl->AddRectFilled(a, b, kPanel, 4.f * u);
			dl->AddRect(a, b, kLine, 4.f * u, ImDrawCornerFlags_All, 1.f);
		} else {
			dl->AddRectFilled(a, b, style == 1 ? (hov ? C(255, 196, 90) : kAmber) : (act ? kPanelHi : (hov ? C(30, 37, 46) : kPanel)), 4.f * u);
			if (style != 1) dl->AddRect(a, b, hov ? (style == 2 ? kRed : kCyan) : edge, 4.f * u, ImDrawCornerFlags_All, 1.5f);
		}
		ImVec2 ts = TextSize(label, px * u);
		ImU32 col = !enabled ? kTrack : (style == 1 ? C(20, 22, 26) : (style == 2 ? kRed : kText));
		if (!enabled) col = C(96, 106, 118);
		Text(ImVec2(a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f), label, px * u, col);
		return clicked;
	}
	float ButtonW(const char* label, float px = 16.f, float padding = 36.f) const { return TextSize(label, px * u).x + padding * u; }

	bool Toggle(const char* id, ImVec2 a, float w, const ui::S& label, bool* v)
	{
		bool hov;
		bool clicked = Hit(id, a, ImVec2(w, 28.f * u), &hov);
		if (clicked) *v = !*v;
		const float tw = 44.f * u, th = 20.f * u;
		ImVec2 t0(a.x, a.y + 4.f * u), t1(a.x + tw, a.y + 4.f * u + th);
		dl->AddRectFilled(t0, t1, *v ? kAmber : kTrack, th * 0.5f);
		dl->AddCircleFilled(ImVec2(*v ? t1.x - th * 0.5f : t0.x + th * 0.5f, t0.y + th * 0.5f), th * 0.38f, *v ? C(30, 24, 10) : kDim);
		Text(ImVec2(a.x + tw + 12.f * u, a.y + 4.f * u), Tr(label), 16.f * u, hov ? kText : C(206, 212, 220));
		return clicked;
	}

	// returns true when the value changed; value is snapped to `step`
	bool Slider(const char* id, ImVec2 a, float w, const ui::S& label, float* v, float mn, float mx, float step, const char* fmt)
	{
		char vt[32];
		snprintf(vt, sizeof vt, fmt, *v);
		Text(ImVec2(a.x, a.y), Tr(label), 15.f * u, kDim, w - 60.f * u);
		ImVec2 vs = TextSize(vt, 15.f * u);
		Text(ImVec2(a.x + w - vs.x, a.y), vt, 15.f * u, kAmber);

		bool hov, act;
		Hit(id, ImVec2(a.x, a.y + 22.f * u), ImVec2(w, 26.f * u), &hov, &act);
		bool changed = false;
		if (act) {
			float t = std::clamp((ImGui::GetIO().MousePos.x - a.x - 10.f * u) / (w - 20.f * u), 0.f, 1.f);
			float nv = mn + t * (mx - mn);
			if (step > 0.f) nv = mn + std::round((nv - mn) / step) * step;
			nv = std::clamp(nv, mn, mx);
			if (std::fabs(nv - *v) > 1e-6f) { *v = nv; changed = true; }
		}
		const float ty = a.y + 35.f * u, th = 6.f * u;
		const float t = (*v - mn) / (mx - mn);
		dl->AddRectFilled(ImVec2(a.x + 10.f * u, ty - th * 0.5f), ImVec2(a.x + w - 10.f * u, ty + th * 0.5f), kTrack, th * 0.5f);
		dl->AddRectFilled(ImVec2(a.x + 10.f * u, ty - th * 0.5f), ImVec2(a.x + 10.f * u + (w - 20.f * u) * t, ty + th * 0.5f), kAmber, th * 0.5f);
		dl->AddCircleFilled(ImVec2(a.x + 10.f * u + (w - 20.f * u) * t, ty), (act || hov) ? 9.f * u : 7.5f * u, act ? C(255, 214, 130) : kText);
		return changed;
	}

	// slider backed by a float cvar
	void CvarSlider(const char* id, ImVec2 a, float w, const ui::S& label, const char* cvarName, float mn, float mx, float step, const char* fmt)
	{
		CVarWrapper cv = p->cvarManager->getCvar(cvarName);
		float v = cv.getFloatValue();
		if (Slider(id, a, w, label, &v, mn, mx, step, fmt)) { cv.setValue(v); p->SaveConfigSoon(); }
	}

	void CvarToggle(const char* id, ImVec2 a, float w, const ui::S& label, const char* cvarName)
	{
		CVarWrapper cv = p->cvarManager->getCvar(cvarName);
		bool v = cv.getBoolValue();
		if (Toggle(id, a, w, label, &v)) { cv.setValue(v); p->SaveConfigSoon(); }
	}

	void Section(ImVec2 a, float w, const ui::S& label)
	{
		const char* s = Tr(label);
		Text(a, s, 13.f * u, kCyan);
		ImVec2 ts = TextSize(s, 13.f * u);
		dl->AddLine(ImVec2(a.x + ts.x + 10.f * u, a.y + ts.y * 0.55f), ImVec2(a.x + w, a.y + ts.y * 0.55f), kLine, 1.f);
	}

	// replay position bar: shows the live position, drag to scrub; on release the game jumps there (deferred)
	void SeekBar(const char* id, ImVec2 a, float w, int cur, int total, float fps, const std::vector<ReplayMarker>& marks)
	{
		const int maxFrame = total > 0 ? total - 1 : 0;
		if (!gSeeking) gSeekFrame = cur;
		gSeekFrame = std::clamp(gSeekFrame, 0, maxFrame);

		bool hov, act;
		Hit(id, ImVec2(a.x, a.y + 22.f * u), ImVec2(w, 26.f * u), &hov, &act);
		const bool released = ImGui::IsItemDeactivated();
		if (act && maxFrame > 0) {
			const float t = std::clamp((ImGui::GetIO().MousePos.x - a.x - 10.f * u) / (w - 20.f * u), 0.f, 1.f);
			gSeekFrame = (int)std::lround(t * maxFrame);
			gSeeking = true;
		}
		if (released) {
			gSeeking = false;
			if (maxFrame > 0) {
				const int target = gSeekFrame;
				ReplayClipTrainer* pl = p;
				p->gameWrapper->SetTimeout([pl, target](GameWrapper*) {
					if (pl->gameWrapper->IsInReplay()) pl->gameWrapper->GetGameEventAsReplay().SkipToFrame(target);
				}, 0.f);
			}
		}

		const int curSec = fps > 0.f ? (int)(gSeekFrame / fps) : 0, totSec = fps > 0.f ? (int)(total / fps) : 0;
		char tt[48];
		snprintf(tt, sizeof tt, "%d:%02d / %d:%02d", curSec / 60, curSec % 60, totSec / 60, totSec % 60);
		ImVec2 ts = TextSize(tt, 15.f * u);
		Text(ImVec2(a.x + w - ts.x, a.y), tt, 15.f * u, kAmber);

		const float ty = a.y + 35.f * u, th = 6.f * u;
		const float t = maxFrame > 0 ? (float)gSeekFrame / (float)maxFrame : 0.f;
		dl->AddRectFilled(ImVec2(a.x + 10.f * u, ty - th * 0.5f), ImVec2(a.x + w - 10.f * u, ty + th * 0.5f), kTrack, th * 0.5f);
		dl->AddRectFilled(ImVec2(a.x + 10.f * u, ty - th * 0.5f), ImVec2(a.x + 10.f * u + (w - 20.f * u) * t, ty + th * 0.5f), kCyan, th * 0.5f);
		dl->AddCircleFilled(ImVec2(a.x + 10.f * u + (w - 20.f * u) * t, ty), (act || hov) ? 9.f * u : 7.5f * u, act ? C(150, 226, 244) : kText);

		// goals (team colour) and highlights on the bar; hover one for details
		if (maxFrame > 0) {
			const ReplayMarker* nearest = nullptr;
			float nearDx = 8.f * u;
			const float mx = ImGui::GetIO().MousePos.x;
			for (int pass = 0; pass < 2; ++pass) { // highlights first so goals sit on top
				for (const ReplayMarker& m : marks) {
					if ((m.kind == 2) != (pass == 0)) continue;
					const float x = a.x + 10.f * u + (w - 20.f * u) * ((float)m.frame / (float)maxFrame);
					if (m.kind == 2) {
						dl->AddQuadFilled(ImVec2(x, ty - 6.f * u), ImVec2(x + 4.f * u, ty), ImVec2(x, ty + 6.f * u), ImVec2(x - 4.f * u, ty), C(190, 200, 212, 200));
					} else {
						dl->AddCircleFilled(ImVec2(x, ty), 6.f * u, m.kind == 0 ? kBlue : kOrange);
						dl->AddCircle(ImVec2(x, ty), 6.f * u, kText, 16, 1.5f);
					}
					if (hov && !act && std::fabs(mx - x) < nearDx) { nearest = &m; nearDx = std::fabs(mx - x); }
				}
			}
			if (nearest && fps > 0.f) {
				const int sec = (int)(nearest->frame / fps);
				char tip[200];
				if (nearest->kind == 2) snprintf(tip, sizeof tip, Tr(ui::tipHighlight), sec / 60, sec % 60);
				else snprintf(tip, sizeof tip, Tr(ui::tipGoal), nearest->name.c_str(), sec / 60, sec % 60);
				const ImVec2 ts = TextSize(tip, 14.f * u);
				const float x = a.x + 10.f * u + (w - 20.f * u) * ((float)nearest->frame / (float)maxFrame);
				const ImVec2 p0((std::max)(a.x, (std::min)(x - ts.x * 0.5f - 8.f * u, a.x + w - ts.x - 16.f * u)), a.y - 12.f * u - ts.y);
				ImDrawList* fg = ImGui::GetForegroundDrawList();
				fg->AddRectFilled(p0, ImVec2(p0.x + ts.x + 16.f * u, p0.y + ts.y + 10.f * u), C(15, 18, 23, 240), 4.f * u);
				fg->AddRect(p0, ImVec2(p0.x + ts.x + 16.f * u, p0.y + ts.y + 10.f * u), nearest->kind == 1 ? kOrange : (nearest->kind == 0 ? kBlue : kLine), 4.f * u, ImDrawCornerFlags_All, 1.2f);
				TextOn(fg, ImVec2(p0.x + 8.f * u, p0.y + 5.f * u), tip, 14.f * u, kText);
			}
		}
	}

	// text box (ImGui::InputText, restyled to the theme). Returns true when the content changed.
	bool NameInput(const char* id, ImVec2 a, ImVec2 sz, const ui::S& hint, char* buf = gNameBuf, size_t cap = sizeof gNameBuf)
	{
		ImVec2 b(a.x + sz.x, a.y + sz.y);
		dl->AddRectFilled(a, b, kPanel, 4.f * u);
		const float fpx = 17.f * u;
		ImFont* f = p->fontsJa_[0] ? p->fontsJa_[0] : p->fonts_[0];
		if (f) { ImGui::PushFont(f); ImGui::SetWindowFontScale(fpx / f->FontSize); }
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f * u, (std::max)(2.f, (sz.y - fpx) * 0.5f)));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, C(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, C(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, C(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, kText);
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, kDim);
		ImGui::SetCursorScreenPos(a);
		ImGui::SetNextItemWidth(sz.x);
		const bool changed = ImGui::InputTextWithHint(id, Tr(hint), buf, cap);
		const bool active = ImGui::IsItemActive();
		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar(2);
		if (f) { ImGui::PopFont(); ImGui::SetWindowFontScale(1.f); }
		dl->AddRect(a, b, active ? kCyan : kLine, 4.f * u, ImDrawCornerFlags_All, active ? 1.8f : 1.f);
		return changed;
	}

	// Scale the alpha of every vertex this window's draw list got since v0 (window opacity). Raw IM_COL32 colours
	// ignore ImGuiStyleVar_Alpha, so the faded result is produced here, after drawing.
	void FadeFrom(int v0) const
	{
		const float a = p->uiOpacity_;
		if (a >= 0.999f) return;
		for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
			const ImU32 c = dl->VtxBuffer[i].col;
			const unsigned al = (unsigned)(((c >> 24) & 0xFF) * a);
			dl->VtxBuffer[i].col = (c & 0x00FFFFFF) | (al << 24);
		}
	}

	// scrolling area inside a tab: widgets are placed at base + (x, y); finish with EndScroll(y)
	struct Scroll { ImDrawList* saved; int v0; ImVec2 base; };
	Scroll BeginScroll(const char* id, ImVec2 o, float w, float h)
	{
		ImGui::SetCursorScreenPos(o);
		ImGui::BeginChild(id, ImVec2(w, h), false, ImGuiWindowFlags_NoBackground);
		Scroll s{ dl, 0, ImGui::GetCursorScreenPos() };
		dl = ImGui::GetWindowDrawList();
		s.v0 = dl->VtxBuffer.Size;
		return s;
	}
	void EndScroll(const Scroll& s, float contentH)
	{
		FadeFrom(s.v0);
		dl = s.saved;
		ImGui::SetCursorScreenPos(ImVec2(s.base.x, s.base.y + contentH));
		ImGui::Dummy(ImVec2(1.f, 1.f));
		ImGui::EndChild();
	}

	// ---------- one frame of the window ----------
	void Frame(bool* open);
	void TabConvert(ImVec2 o, float w, float h);
	void TabClips(ImVec2 o, float w, float h);
	void TabSettings(ImVec2 o, float w, float h);
	void TabDialog(ImVec2 o, float w, float h);
	void OpenDlg(Dlg kind, const std::string& target, const std::string& curFolder, const std::string& preset);
	bool MoreButton(const char* id, ImVec2 a, float w, float h);

	// ---------- overlays ----------
	void Hud();
	void Preparing();
	void Progress();
	void Compat();
};

// =====================================================================================================================
// window frame
// =====================================================================================================================

void UiPainter::Frame(bool* open)
{
	const float W = p->uiW_ * u, H = p->uiH_ * u;
	ImVec2 o = ImGui::GetWindowPos();
	dl = ImGui::GetWindowDrawList();
	const int vtx0 = dl->VtxBuffer.Size;

	// frame
	dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), kBg, 8.f * u);
	dl->AddRect(o, ImVec2(o.x + W, o.y + H), kLine, 8.f * u, ImDrawCornerFlags_All, 1.5f);
	dl->AddRectFilled(o, ImVec2(o.x + W, o.y + 3.f * u), kAmber, 8.f * u, ImDrawCornerFlags_Top);

	const float pad = 26.f * u;

	// header
	Text(ImVec2(o.x + pad, o.y + 16.f * u), Tr(ui::caption), 13.f * u, kCyan);
	Text(ImVec2(o.x + pad, o.y + 32.f * u), Tr(ui::title), 30.f * u, kText);

	// status pill
	{
		const ui::S* st = &ui::stIdle;
		ImU32 stc = kDim;
		if (p->cvarManager->getCvar("reclip_replaySave").getBoolValue()) { st = &ui::stConverting; stc = kAmber; }
		else if (p->isPreparingSession) { st = &ui::stPreparing; stc = kAmber; }
		else if (p->cvarManager->getCvar("reclip_controllGame").getBoolValue()) { st = &ui::stTraining; stc = kGreen; }
		else if (p->gameWrapper->IsInReplay()) { st = &ui::stReplay; stc = kCyan; }
		const char* s = Tr(*st);
		ImVec2 ts = TextSize(s, 14.f * u);
		const float pw = ts.x + 40.f * u, ph = 28.f * u;
		ImVec2 a(o.x + W - pad - 44.f * u - pw, o.y + 22.f * u);
		dl->AddRectFilled(a, ImVec2(a.x + pw, a.y + ph), C(0, 0, 0, 70), ph * 0.5f);
		dl->AddRect(a, ImVec2(a.x + pw, a.y + ph), stc, ph * 0.5f, ImDrawCornerFlags_All, 1.2f);
		dl->AddCircleFilled(ImVec2(a.x + 16.f * u, a.y + ph * 0.5f), 4.5f * u, stc);
		Text(ImVec2(a.x + 28.f * u, a.y + (ph - ts.y) * 0.5f), s, 14.f * u, stc);
	}
	// close (hides the panel; the ImGui window itself stays open for the overlays)
	{
		ImVec2 a(o.x + W - pad - 30.f * u, o.y + 20.f * u);
		bool hov;
		if (Hit("##close", a, ImVec2(30.f * u, 30.f * u), &hov)) *open = false;
		const ImU32 col = hov ? kRed : kDim;
		ImVec2 c(a.x + 15.f * u, a.y + 15.f * u);
		dl->AddLine(ImVec2(c.x - 7.f * u, c.y - 7.f * u), ImVec2(c.x + 7.f * u, c.y + 7.f * u), col, 2.f);
		dl->AddLine(ImVec2(c.x - 7.f * u, c.y + 7.f * u), ImVec2(c.x + 7.f * u, c.y - 7.f * u), col, 2.f);
	}

	// tabs
	const float tabY = o.y + 84.f * u, tabH = 40.f * u, tabW = (W - 2.f * pad) / 3.f;
	const ui::S* tabs[3] = { &ui::tabConvert, &ui::tabClips, &ui::tabSettings };
	dl->AddLine(ImVec2(o.x + pad, tabY + tabH), ImVec2(o.x + W - pad, tabY + tabH), kLine, 1.f);
	for (int i = 0; i < 3; ++i) {
		ImVec2 a(o.x + pad + tabW * i, tabY);
		bool hov;
		char id[16]; snprintf(id, sizeof id, "##tab%d", i);
		if (Hit(id, a, ImVec2(tabW, tabH), &hov)) { gTab = i; gDlg = Dlg::None; }
		const char* s = Tr(*tabs[i]);
		ImVec2 ts = TextSize(s, 18.f * u);
		const bool sel = gTab == i;
		if (sel) dl->AddRectFilled(a, ImVec2(a.x + tabW, a.y + tabH), C(255, 176, 46, 22));
		else if (hov) dl->AddRectFilled(a, ImVec2(a.x + tabW, a.y + tabH), C(255, 255, 255, 10));
		Text(ImVec2(a.x + (tabW - ts.x) * 0.5f, a.y + (tabH - ts.y) * 0.5f), s, 18.f * u, sel ? kAmber : (hov ? kText : kDim));
		if (sel) dl->AddRectFilled(ImVec2(a.x, a.y + tabH - 3.f * u), ImVec2(a.x + tabW, a.y + tabH), kAmber);
	}

	// body
	const ImVec2 bo(o.x + pad, tabY + tabH + 18.f * u);
	const float bw = W - 2.f * pad;
	const float footerH = 40.f * u;
	const float bh = (o.y + H - footerH) - bo.y - 8.f * u;
	if (gDlg != Dlg::None) {
		TabDialog(bo, bw, bh);
	} else {
		switch (gTab) {
		case 0: TabConvert(bo, bw, bh); break;
		case 1: TabClips(bo, bw, bh); break;
		default: TabSettings(bo, bw, bh); break;
		}
	}

	// footer
	const float fy = o.y + H - footerH;
	dl->AddLine(ImVec2(o.x + pad, fy), ImVec2(o.x + W - pad, fy), kLine, 1.f);
	Text(ImVec2(o.x + pad, fy + 12.f * u), Tr(ui::hintDrag), 13.f * u, kDim);
	{
		char buf[64];
		snprintf(buf, sizeof buf, "v%s", plugin_version);
		ImVec2 ts = TextSize(buf, 13.f * u);
		Text(ImVec2(o.x + W - pad - 22.f * u - ts.x, fy + 12.f * u), buf, 13.f * u, kDim);
	}

	// resize grip (bottom-right): drag to change the window size; saved when released
	{
		const float g = 22.f * u;
		const ImVec2 a(o.x + W - g - 4.f * u, o.y + H - g - 4.f * u);
		bool hov, act;
		Hit("##grip", a, ImVec2(g, g), &hov, &act);
		if (act) {
			const ImVec2 d = ImGui::GetIO().MouseDelta;
			const ImVec2 disp = ImGui::GetIO().DisplaySize;
			p->uiW_ = std::clamp(p->uiW_ + d.x / u, 400.f, (std::max)(400.f, disp.x / u - 20.f));
			p->uiH_ = std::clamp(p->uiH_ + d.y / u, 360.f, (std::max)(360.f, disp.y / u - 20.f));
		}
		if (ImGui::IsItemDeactivated()) {
			p->cvarManager->getCvar("reclip_ui_w").setValue(p->uiW_);
			p->cvarManager->getCvar("reclip_ui_h").setValue(p->uiH_);
			p->SaveConfigSoon();
		}
		const ImU32 col = (act || hov) ? kCyan : kDim;
		for (int i = 1; i <= 3; ++i) {
			const float k = i * 6.f * u;
			dl->AddLine(ImVec2(a.x + g - 3.f * u, a.y + g - 3.f * u - k), ImVec2(a.x + g - 3.f * u - k, a.y + g - 3.f * u), col, 1.5f);
		}
	}

	FadeFrom(vtx0);
}

// =====================================================================================================================
// CONVERT tab
// =====================================================================================================================

void UiPainter::TabConvert(ImVec2 o, float w, float h)
{
	ReplayClipTrainer* pl = p;
	auto gw = p->gameWrapper;
	const bool converting = p->cvarManager->getCvar("reclip_replaySave").getBoolValue();
	const bool inReplay = gw->IsInReplay();
	const bool canConvert = inReplay && !converting;

	Scroll sc = BeginScroll("##convert", o, w, h);
	const float cw = w - ImGui::GetStyle().ScrollbarSize - 8.f * u; // room for the scrollbar
	auto at = [&](float x, float y) { return ImVec2(sc.base.x + x, sc.base.y + y); };
	float y = 0.f;

	// ---- intro ----
	y += Para(at(0, y), Tr(ui::howTo), 14.f * u, kDim, cw) + 6.f * u;
	if (converting) y += Para(at(0, y), Tr(ui::warnConverting), 14.f * u, kAmber, cw);
	else y += Para(at(0, y), Tr(ui::warnChange), 13.f * u, kDim, cw);
	y += 14.f * u;

	// ---- replay position ----
	Section(at(0, y), cw, ui::lblPosition);
	y += 24.f * u;
	if (inReplay && !converting) {
		ReplayServerWrapper replayServer = gw->GetGameEventAsReplay();
		const int total = replayServer.GetReplay().GetNumFrames();
		const int cur = replayServer.GetCurrentReplayFrame();
		const float fps = replayServer.GetReplayFPS();
		if (p->markersKey_ != total && !p->markersPending_) { // new replay: read its goals / highlights on the game thread
			p->markersKey_ = total;
			p->markersPending_ = true;
			gw->SetTimeout([pl](GameWrapper*) { pl->RefreshReplayMarkers(); }, 0.f);
		}
		std::vector<ReplayMarker> marks;
		{ std::lock_guard<std::mutex> l(p->markersMutex_); marks = p->replayMarkers_; }
		SeekBar("##seek", at(0, y), cw, cur, total, fps, marks);
	} else if (!inReplay) {
		p->markersKey_ = -1;
		Para(at(0, y + 4.f * u), Tr(ui::notInReplay), 14.f * u, kDim, cw);
	}
	y += 54.f * u;

	// ---- clip range ----
	Section(at(0, y), cw, ui::lblRange);
	y += 28.f * u;
	const bool narrow = cw < 520.f * u; // sliders stack in one column when the window is narrow
	const float colW = narrow ? cw : (cw - 30.f * u) / 2.f;
	CvarSlider("##slBefore", at(0, y), colW, ui::lblBefore, "reclip_clipSecondsBefore", 0.f, 20.f, 0.5f, "%.1f s");
	if (narrow) y += 58.f * u;
	CvarSlider("##slAfter", at(narrow ? 0.f : colW + 30.f * u, y), colW, ui::lblAfter, "reclip_clipSecondsAfter", 0.f, 20.f, 0.5f, "%.1f s");
	y += 58.f * u;
	CvarSlider("##slRes", at(0, y), colW, ui::lblRes, "reclip_resolution", 0.1f, 2.0f, 0.1f, "%.1f");
	if (narrow) {
		y += 54.f * u;
		y += Para(at(0, y), Tr(ui::resHint), 13.f * u, kDim, cw) + 10.f * u;
	} else {
		Para(at(colW + 30.f * u, y + 2.f * u), Tr(ui::resHint), 13.f * u, kDim, colW);
		y += 62.f * u;
	}

	// ---- save: the main use ----
	Section(at(0, y), cw, ui::lblSave);
	y += 28.f * u;
	NameInput("##clipName", at(0, y), ImVec2(cw, 38.f * u), ui::phName);
	y += 48.f * u;
	{
		const float saveH = (std::max)(64.f * u, 38.f * u + TextSize(Tr(ui::btnSaveD), 13.f * u, cw - 78.f * u).y + 14.f * u);
		if (Card("##save", at(0, y), ImVec2(cw, saveH), ui::btnSave, ui::btnSaveD, IconSave, true, canConvert)) {
			std::string name(gNameBuf);
			gw->SetTimeout([pl, name](GameWrapper*) { pl->ConvertClipToFile(name); }, 0.f);
			gNameBuf[0] = '\0';
		}
		y += saveH + 10.f * u;
	}
	CvarToggle("##tgAnon", at(0, y), cw, ui::tgAnon, "reclip_anonymize");
	y += 32.f * u;
	y += Para(at(56.f * u, y), Tr(ui::anonD), 13.f * u, kDim, cw - 56.f * u) + 18.f * u;

	// ---- train without saving: a side feature, kept small ----
	Section(at(0, y), cw, ui::lblQuick);
	y += 28.f * u;
	{
		const char* s = Tr(ui::btnTrainNow);
		const float bwid = ButtonW(s, 15.f, 36.f);
		if (Button("##train", at(0, y), ImVec2(bwid, 32.f * u), s, 0, canConvert, 15.f))
			gw->SetTimeout([pl](GameWrapper*) { pl->cvarManager->executeCommand("reclip_convertClip 1"); }, 0.f);
		const float dh = Para(at(bwid + 12.f * u, y + 2.f * u), Tr(ui::btnTrainNowD), 13.f * u, kDim, cw - bwid - 12.f * u);
		y += (std::max)(32.f * u, dh + 4.f * u) + 10.f * u;
	}

	// ---- results ----
	if (!p->lastClipRangeError.empty()) {
		char msg[200];
		snprintf(msg, sizeof msg, Tr(ui::errShort), (int)ReplayClipTrainer::kMinClipTotalSeconds);
		y += Para(at(0, y), msg, 14.f * u, kRed, cw) + 6.f * u;
	}
	if (!p->lastSaveClipMessage.empty()) {
		char msg[400];
		snprintf(msg, sizeof msg, Tr(p->lastSaveClipSucceeded ? ui::savedOk : ui::savedFail), p->lastSaveClipMessage.c_str());
		y += Para(at(0, y), msg, 14.f * u, p->lastSaveClipSucceeded ? kGreen : kRed, cw) + 6.f * u;
	}

	EndScroll(sc, y + 8.f * u);
}

// =====================================================================================================================
// CLIPS tab
// =====================================================================================================================

void UiPainter::TabClips(ImVec2 o, float w, float h)
{
	ReplayClipTrainer* pl = p;
	auto gw = p->gameWrapper;
	const bool converting = p->cvarManager->getCvar("reclip_replaySave").getBoolValue();
	float y = o.y;

	// ---- toolbar ----
	{
		float x = o.x;
		auto tb = [&](const char* id, const ui::S& label, bool enabled) {
			const char* s = Tr(label);
			const float bwid = ButtonW(s, 15.f, 30.f);
			if (x > o.x && x + bwid > o.x + w) { x = o.x; y += 42.f * u; } // wrap to the next row
			const bool c = Button(id, ImVec2(x, y), ImVec2(bwid, 34.f * u), s, 0, enabled, 15.f);
			x += bwid + 8.f * u;
			return c;
		};
		if (tb("##refresh", ui::btnRefresh, true)) p->RefreshActiveClipList();
		if (tb("##openfolder", ui::btnOpenFolder, true)) gw->SetTimeout([pl](GameWrapper*) { pl->OpenClipsFolder(); }, 0.f);
		if (!p->showRecentClips && tb("##newfolder", ui::btnNewFolder, true)) OpenDlg(Dlg::NewFolder, "", "", "");
		if (tb("##retry", ui::btnRetry, !converting)) {
			if (p->hasReplaySaved) {
				gShowNoRetry = false;
				gw->SetTimeout([pl](GameWrapper*) { pl->cvarManager->executeCommand("reclip_openReplay 1"); }, 0.f);
			} else {
				gShowNoRetry = true;
			}
		}
	}
	y += 44.f * u;

	// ---- recent filter ----
	{
		const float tw = 44.f * u + 12.f * u + TextSize(Tr(ui::tgRecent), 16.f * u).x + 20.f * u;
		if (Toggle("##recent", ImVec2(o.x, y), tw, ui::tgRecent, &p->showRecentClips)) p->RefreshActiveClipList();
		if (p->showRecentClips) {
			float x = o.x + tw + 12.f * u;
			if (x + 6.f * 52.f * u > o.x + w) { x = o.x; y += 38.f * u; } // not enough room beside the toggle
			for (int hours : { 1, 3, 6, 12, 24, 48 }) {
				char label[24]; snprintf(label, sizeof label, "%d%s", hours, Tr(ui::txtHours));
				char id[16]; snprintf(id, sizeof id, "##rh%d", hours);
				const float bwid = ButtonW(label, 14.f, 22.f);
				const bool sel = p->recentClipHours == hours;
				if (Button(id, ImVec2(x, y - 2.f * u), ImVec2(bwid, 30.f * u), label, sel ? 1 : 0, true, 14.f)) {
					p->recentClipHours = hours;
					p->RefreshRecentClipsList();
				}
				x += bwid + 6.f * u;
			}
		}
	}
	y += 38.f * u;

	if (gShowNoRetry && !p->hasReplaySaved) { Text(ImVec2(o.x, y), Tr(ui::noRetry), 14.f * u, kRed, w); y += 22.f * u; }
	if (!p->lastLoadClipError.empty()) {
		std::string msg = std::string(Tr(ui::loadFail)) + ": " + p->lastLoadClipError;
		Text(ImVec2(o.x, y), msg.c_str(), 14.f * u, kRed, w);
		y += 22.f * u;
	}

	// ---- list ----
	std::string clipToDelete, folderToEnter;
	bool goBack = false;
	const bool recent = p->showRecentClips;
	const std::vector<SavedClipInfo>& clips = recent ? p->recentClips : p->savedClips;

	Scroll sc = BeginScroll("##clips", ImVec2(o.x, y), w, (std::max)(60.f * u, o.y + h - y));
	const float rw = w - ImGui::GetStyle().ScrollbarSize - 8.f * u, rh = 56.f * u;
	float ly = 0.f;
	auto at = [&](float x, float yy) { return ImVec2(sc.base.x + x, sc.base.y + yy); };
	auto rowBg = [&](ImVec2 a, bool hot) {
		dl->AddRectFilled(a, ImVec2(a.x + rw, a.y + rh), hot ? C(30, 37, 46) : kPanel, 4.f * u);
		dl->AddRect(a, ImVec2(a.x + rw, a.y + rh), hot ? kCyan : kLine, 4.f * u, ImDrawCornerFlags_All, 1.f);
	};

	if (clips.empty() && (recent || p->savedClipFolders.empty())) {
		Text(at(0, ly + 4.f * u), Tr(recent ? ui::noRecent : ui::noClips), 15.f * u, kDim, rw);
		ly += 30.f * u;
	}

	if (!recent && !p->currentClipFolder.empty()) {
		const float bwid = ButtonW(Tr(ui::btnBack), 15.f, 30.f);
		if (Button("##back", at(0, ly), ImVec2(bwid, 34.f * u), Tr(ui::btnBack), 0, true, 15.f)) goBack = true;
		std::string path = "/" + p->currentClipFolder;
		Text(at(bwid + 12.f * u, ly + 8.f * u), path.c_str(), 15.f * u, kDim, rw - bwid - 12.f * u);
		ly += 44.f * u;
	}
	if (!recent) {
		int fi = 0;
		for (const std::string& name : p->savedClipFolders) {
			ImVec2 a = at(0, ly);
			char id[24]; snprintf(id, sizeof id, "##fold%d", fi++);
			bool hov;
			if (Hit(id, a, ImVec2(rw, 44.f * u), &hov, nullptr, true)) folderToEnter = name;
			dl->AddRectFilled(a, ImVec2(a.x + rw, a.y + 44.f * u), hov ? C(30, 37, 46) : kPanel, 4.f * u);
			dl->AddRect(a, ImVec2(a.x + rw, a.y + 44.f * u), hov ? kCyan : kLine, 4.f * u, ImDrawCornerFlags_All, 1.f);
			IconFolder(dl, ImVec2(a.x + 26.f * u, a.y + 22.f * u), 22.f * u, kAmber);
			dl->PushClipRect(a, ImVec2(a.x + rw - 52.f * u, a.y + 44.f * u), true);
			Text(ImVec2(a.x + 48.f * u, a.y + 12.f * u), name.c_str(), 16.f * u, kText);
			dl->PopClipRect();
			{
				char idm[24]; snprintf(idm, sizeof idm, "##fm%d", fi);
				if (MoreButton(idm, ImVec2(a.x + rw - 40.f * u, a.y + 6.f * u), 32.f * u, 32.f * u))
					OpenDlg(Dlg::Folder, p->currentClipFolder.empty() ? name : p->currentClipFolder + "/" + name, "", name);
			}
			ly += 52.f * u;
		}
	}

	int ci = 0;
	for (const SavedClipInfo& clip : clips) {
		ImVec2 a = at(0, ly);
		const bool hot = ImGui::IsMouseHoveringRect(a, ImVec2(a.x + rw, a.y + rh)) && ImGui::IsWindowHovered();
		rowBg(a, hot);

		const bool pending = gPendingDelete == clip.path;
		const char* lLoad = Tr(ui::btnLoad);
		const char* lDel = Tr(pending ? ui::btnConfirm : ui::btnDelete);
		const char* lCancel = Tr(ui::btnCancel);
		const float bhgt = 32.f * u, by = a.y + (rh - bhgt) * 0.5f;
		float bx = a.x + rw - 12.f * u;
		char id[24];

		{
			const float ms = 32.f * u;
			bx -= ms; snprintf(id, sizeof id, "##mo%d", ci);
			if (MoreButton(id, ImVec2(bx, by), ms, bhgt))
				OpenDlg(Dlg::Clip, clip.path, recent ? clip.folder : p->currentClipFolder, std::filesystem::u8path(clip.path).stem().u8string());
			bx -= 8.f * u;
		}
		if (pending) {
			const float wc = ButtonW(lCancel, 14.f, 24.f);
			bx -= wc; snprintf(id, sizeof id, "##cn%d", ci);
			if (Button(id, ImVec2(bx, by), ImVec2(wc, bhgt), lCancel, 0, true, 14.f)) gPendingDelete.clear();
			bx -= 8.f * u;
		}
		{
			const float wd = ButtonW(lDel, 14.f, 24.f);
			bx -= wd; snprintf(id, sizeof id, "##dl%d", ci);
			if (Button(id, ImVec2(bx, by), ImVec2(wd, bhgt), lDel, 2, true, 14.f)) {
				if (pending) { clipToDelete = clip.path; gPendingDelete.clear(); }
				else gPendingDelete = clip.path;
			}
			bx -= 8.f * u;
		}
		{
			const float wl = ButtonW(lLoad, 14.f, 28.f);
			bx -= wl; snprintf(id, sizeof id, "##ld%d", ci);
			if (Button(id, ImVec2(bx, by), ImVec2(wl, bhgt), lLoad, 1, clip.isCompatible && !converting, 14.f)) {
				const std::string clipPath = clip.path;
				gw->SetTimeout([pl, clipPath](GameWrapper*) { pl->LoadClipAndOpen(clipPath); }, 0.f);
			}
			bx -= 14.f * u;
		}

		std::string name = std::filesystem::u8path(clip.path).stem().u8string();
		std::string sub;
		if (clip.isCompatible) {
			if (!clip.saveTimeStr.empty()) sub += clip.saveTimeStr;
			if (clip.playerCount > 0) { if (!sub.empty()) sub += "  -  "; sub += std::to_string(clip.playerCount) + "p"; }
		} else {
			sub = Tr(ui::txtIncompat);
		}
		if (recent) {
			if (!sub.empty()) sub += "  -  ";
			sub += std::string(Tr(ui::txtIn)) + " " + (clip.folder.empty() ? std::string(Tr(ui::txtRoot)) : clip.folder);
		}
		dl->PushClipRect(a, ImVec2(bx, a.y + rh), true);
		Text(ImVec2(a.x + 14.f * u, a.y + 9.f * u), name.c_str(), 16.f * u, clip.isCompatible ? kText : kDim);
		Text(ImVec2(a.x + 14.f * u, a.y + 32.f * u), sub.c_str(), 13.f * u, clip.isCompatible ? kDim : C(110, 120, 132));
		dl->PopClipRect();
		ly += rh + 6.f * u;
		++ci;
	}
	EndScroll(sc, ly);

	// navigation/deletion are applied here rather than inline above: RefreshSavedClipsList() mutates the vectors that
	// were being iterated
	if (goBack) {
		gPendingDelete.clear();
		size_t lastSlash = p->currentClipFolder.find_last_of('/');
		p->currentClipFolder = (lastSlash == std::string::npos) ? "" : p->currentClipFolder.substr(0, lastSlash);
		p->RefreshSavedClipsList();
	} else if (!folderToEnter.empty()) {
		gPendingDelete.clear();
		p->currentClipFolder = p->currentClipFolder.empty() ? folderToEnter : p->currentClipFolder + "/" + folderToEnter;
		p->RefreshSavedClipsList();
	}
	if (!clipToDelete.empty()) {
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(clipToDelete), ec);
		p->RefreshActiveClipList();
	}
}

// =====================================================================================================================
// SETTINGS tab
// =====================================================================================================================

void UiPainter::TabSettings(ImVec2 o, float w, float h)
{
	ReplayClipTrainer* pl = p;
	Scroll sc = BeginScroll("##settings", o, w, h);
	const float cw = w - ImGui::GetStyle().ScrollbarSize - 8.f * u;
	auto at = [&](float x, float yy) { return ImVec2(sc.base.x + x, sc.base.y + yy); };
	float y = 0.f;

	Section(at(0, y), cw, ui::lblTraining);
	y += 28.f * u;
	CvarToggle("##tgBoost", at(0, y), cw, ui::tgLimitedBoost, "reclip_limitedBoost");        y += 34.f * u;
	CvarToggle("##tgUnpause", at(0, y), cw, ui::tgInputUnpause, "reclip_inputToUnpause");    y += 34.f * u;
	CvarToggle("##tgHud", at(0, y), cw, ui::tgShowHud, "reclip_showHud");                    y += 34.f * u;
	CvarToggle("##tgGuide", at(0, y), cw, ui::tgKeyGuide, "reclip_showKeyGuide");            y += 34.f * u;
	CvarToggle("##tgGoal", at(0, y), cw, ui::tgDisableGoal, "reclip_disableGoal");           y += 34.f * u;
	CvarToggle("##tgFree", at(0, y), cw, ui::tgBallFree, "reclip_ballFreePhysics");          y += 32.f * u;
	y += Para(at(56.f * u, y), Tr(ui::ballFreeD), 13.f * u, kDim, cw - 56.f * u) + 18.f * u;

	Section(at(0, y), cw, ui::lblBindings);
	y += 28.f * u;
	{
		const char* s = Tr(ui::btnApplyBind);
		const float bwid = ButtonW(s, 16.f, 44.f);
		if (Button("##bind", at(0, y), ImVec2(bwid, 36.f * u), s, 0))
			p->gameWrapper->SetTimeout([pl](GameWrapper*) { pl->cvarManager->getCvar("reclip_bindings").setValue(true); }, 0.f);
		y += 46.f * u;
	}
	y += Para(at(0, y), Tr(ui::bindDesc), 13.f * u, kDim, cw) + 18.f * u;

	Section(at(0, y), cw, ui::lblLang);
	y += 30.f * u;
	{
		const float segW = 150.f * u, segH = 38.f * u;
		for (int i = 0; i < 2; ++i) {
			const bool jaSeg = i == 1;
			const char* label = jaSeg ? u8"日本語" : "English";
			const ImVec2 a(sc.base.x + i * (segW + 8.f * u), sc.base.y + y);
			bool hov;
			char id[16]; snprintf(id, sizeof id, "##lang%d", i);
			if (Hit(id, a, ImVec2(segW, segH), &hov)) p->SetLanguage(jaSeg);
			const bool sel = p->uiJa_ == jaSeg;
			dl->AddRectFilled(a, ImVec2(a.x + segW, a.y + segH), sel ? C(255, 176, 46, 40) : (hov ? C(30, 37, 46) : kPanel), 4.f * u);
			dl->AddRect(a, ImVec2(a.x + segW, a.y + segH), sel ? kAmber : kLine, 4.f * u, ImDrawCornerFlags_All, sel ? 1.8f : 1.f);
			ImVec2 ts = TextSize(label, 17.f * u);
			Text(ImVec2(a.x + (segW - ts.x) * 0.5f, a.y + (segH - ts.y) * 0.5f), label, 17.f * u, sel ? kAmber : kText);
		}
	}
	y += 50.f * u;
	Text(at(0, y), Tr(p->jpReady_ ? ui::fontReady : ui::fontLoading), 13.f * u, p->jpReady_ ? kGreen : kRed);
	y += 36.f * u;

	Section(at(0, y), cw, ui::lblWindow);
	y += 28.f * u;
	{
		const bool narrow = cw < 520.f * u;
		const float colW = narrow ? cw : (cw - 30.f * u) / 2.f;
		float size = p->uiScale_ * 100.f;
		if (Slider("##slSize", at(0, y), colW, ui::lblSize, &size, 50.f, 100.f, 5.f, "%.0f%%")) {
			p->uiScale_ = size / 100.f;
			p->cvarManager->getCvar("reclip_ui_scale").setValue(p->uiScale_);
			p->SaveConfigSoon();
		}
		float op = p->uiOpacity_ * 100.f;
		if (narrow) y += 58.f * u;
		if (Slider("##slOpacity", at(narrow ? 0.f : colW + 30.f * u, y), colW, ui::lblOpacity, &op, 20.f, 100.f, 5.f, "%.0f%%")) {
			p->uiOpacity_ = op / 100.f;
			p->cvarManager->getCvar("reclip_ui_opacity").setValue(p->uiOpacity_);
			p->SaveConfigSoon();
		}
	}
	y += 64.f * u;

	Section(at(0, y), cw, ui::lblAbout);
	y += 28.f * u;
	y += Para(at(0, y), Tr(ui::aboutBy), 14.f * u, kText, cw) + 4.f * u;
	y += Para(at(0, y), Tr(ui::aboutBase), 13.f * u, kDim, cw) + 4.f * u;
	{
		char buf[64];
		snprintf(buf, sizeof buf, "%s %s", Tr(ui::txtVersion), plugin_version);
		y += Para(at(0, y), buf, 13.f * u, kDim, cw);
	}

	EndScroll(sc, y + 8.f * u);
}

// =====================================================================================================================
// overlays (not tied to the settings panel being open)
// =====================================================================================================================

// ---------- clip / folder dialog ----------

void UiPainter::OpenDlg(Dlg kind, const std::string& target, const std::string& curFolder, const std::string& preset)
{
	gDlg = kind;
	gDlgTarget = target;
	gDlgCurFolder = curFolder;
	gDlgErr.clear();
	gDlgConfirmDelete = false;
	snprintf(gDlgBuf, sizeof gDlgBuf, "%s", preset.c_str());
	gDlgFolders.clear();
	if (kind == Dlg::Clip) gDlgFolders = p->ListClipFolders();
}

// three-dots button
bool UiPainter::MoreButton(const char* id, ImVec2 a, float w, float h)
{
	bool hov, act;
	const bool clicked = Hit(id, a, ImVec2(w, h), &hov, &act);
	dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), act ? kPanelHi : (hov ? C(30, 37, 46) : kPanel), 4.f * u);
	dl->AddRect(a, ImVec2(a.x + w, a.y + h), hov ? kCyan : kLine, 4.f * u, ImDrawCornerFlags_All, 1.f);
	for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(ImVec2(a.x + w * 0.5f + i * 7.f * u, a.y + h * 0.5f), 2.2f * u, hov ? kText : kDim);
	return clicked;
}

void UiPainter::TabDialog(ImVec2 o, float w, float h)
{
	Scroll sc = BeginScroll("##dialog", o, w, h);
	const float cw = w - ImGui::GetStyle().ScrollbarSize - 8.f * u;
	auto at = [&](float x, float yy) { return ImVec2(sc.base.x + x, sc.base.y + yy); };
	float y = 0.f;
	auto errText = [&](const std::string& key) -> const ui::S& {
		if (key == "exists") return ui::errExists;
		if (key == "invalid") return ui::errInvalid;
		if (key == "notEmpty") return ui::errNotEmpty;
		return ui::errFailed;
	};
	auto close = [&]() { gDlg = Dlg::None; gDlgErr.clear(); gDlgConfirmDelete = false; };

	const ui::S& title = gDlg == Dlg::NewFolder ? ui::dlgNewFolder : (gDlg == Dlg::Clip ? ui::dlgClip : ui::dlgFolder);
	Section(at(0, y), cw, title);
	y += 30.f * u;

	// name box
	NameInput("##dlgName", at(0, y), ImVec2(cw, 38.f * u), gDlg == Dlg::Clip ? ui::phClipName : ui::phFolderName, gDlgBuf, sizeof gDlgBuf);
	y += 50.f * u;

	// buttons
	{
		const ui::S& okLabel = gDlg == Dlg::NewFolder ? ui::btnCreate : ui::btnRename;
		const char* ok = Tr(okLabel);
		const char* back = Tr(ui::btnBack);
		const float w1 = ButtonW(ok, 16.f, 40.f), w2 = ButtonW(back, 16.f, 40.f);
		if (Button("##dlgOk", at(0, y), ImVec2(w1, 36.f * u), ok, 1)) {
			std::string err;
			if (gDlg == Dlg::NewFolder) err = p->CreateClipFolder(gDlgBuf);
			else if (gDlg == Dlg::Clip) err = p->RenameClip(gDlgTarget, gDlgBuf);
			else err = p->RenameClipFolder(gDlgTarget, gDlgBuf);
			if (err.empty()) { close(); p->RefreshActiveClipList(); } else gDlgErr = err;
		}
		if (Button("##dlgBack", at(w1 + 8.f * u, y), ImVec2(w2, 36.f * u), back, 0)) close();
		if (gDlg == Dlg::Folder) {
			const char* del = Tr(gDlgConfirmDelete ? ui::btnConfirmFolder : ui::btnDeleteFolder);
			const float w3 = ButtonW(del, 16.f, 40.f);
			if (Button("##dlgDel", at(cw - w3, y), ImVec2(w3, 36.f * u), del, 2)) {
				if (!gDlgConfirmDelete) {
					gDlgConfirmDelete = true;
				} else {
					const std::string err = p->DeleteClipFolder(gDlgTarget);
					if (err.empty()) { close(); p->RefreshActiveClipList(); } else { gDlgErr = err; gDlgConfirmDelete = false; }
				}
			}
		}
		y += 48.f * u;
	}
	if (!gDlgErr.empty()) y += Para(at(0, y), Tr(errText(gDlgErr)), 14.f * u, kRed, cw) + 8.f * u;

	// move list (clips only)
	if (gDlg == Dlg::Clip) {
		y += 8.f * u;
		Section(at(0, y), cw, ui::lblMoveTo);
		y += 30.f * u;
		int fi = 0;
		for (const std::string& folder : gDlgFolders) {
			const bool here = folder == gDlgCurFolder;
			const ImVec2 a = at(0, y);
			char id[24]; snprintf(id, sizeof id, "##mv%d", fi++);
			bool hov = false;
			bool clicked = false;
			if (!here) clicked = Hit(id, a, ImVec2(cw, 36.f * u), &hov);
			dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + 36.f * u), hov ? C(30, 37, 46) : kPanel, 4.f * u);
			dl->AddRect(a, ImVec2(a.x + cw, a.y + 36.f * u), hov ? kCyan : kLine, 4.f * u, ImDrawCornerFlags_All, 1.f);
			IconFolder(dl, ImVec2(a.x + 22.f * u, a.y + 18.f * u), 18.f * u, here ? kDim : kAmber);
			std::string label = folder.empty() ? std::string(Tr(ui::txtRoot)) : "/" + folder;
			if (here) label += "  " + std::string(Tr(ui::txtCurrent));
			dl->PushClipRect(a, ImVec2(a.x + cw - 8.f * u, a.y + 36.f * u), true);
			Text(ImVec2(a.x + 42.f * u, a.y + 8.f * u), label.c_str(), 15.f * u, here ? kDim : kText);
			dl->PopClipRect();
			if (clicked) {
				const std::string err = p->MoveClip(gDlgTarget, folder);
				if (err.empty()) { close(); p->RefreshActiveClipList(); } else gDlgErr = err;
			}
			y += 42.f * u;
		}
	}

	EndScroll(sc, y + 8.f * u);
}

// HUD text of the training session (controlled player, time, take-control state): the canvas font is coarse and has no
// Japanese glyphs, so once the fonts are loaded the game thread publishes these (DrawPlayer / DrawTime / DrawJumpIn)
// and they are drawn here, anti-aliased, with the same anchors as the canvas HUD
void UiPainter::Hud()
{
	std::string name, timeText;
	int team = 0;
	bool nameOk, timeOk, jumpOk, jumped;
	{
		std::lock_guard<std::mutex> lock(p->hudMutex_);
		const auto age = std::chrono::steady_clock::now() - p->hudStamp_;
		if (age > std::chrono::milliseconds(300)) return; // the game thread stopped publishing (left the match)
		nameOk = p->hudNameValid_; name = p->hudName_; team = p->hudTeam_;
		timeOk = p->hudTimeValid_; timeText = p->hudTime_;
		jumpOk = p->hudJumpValid_; jumped = p->hudJumpedIn_;
	}
	SetupOverlay();
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const float px = 30.f * u;
	auto anchor = [&](float x, float y) { return ImVec2((x / 1920.f) * disp.x, (y / 1080.f) * disp.y); };
	ImDrawList* d = ImGui::GetForegroundDrawList();
	auto draw = [&](ImVec2 pos, const char* s, float size, ImU32 col) {
		TextOn(d, ImVec2(pos.x + 1.5f, pos.y + 1.5f), s, size, C(0, 0, 0, 200));
		TextOn(d, pos, s, size, col);
	};
	if (nameOk) draw(anchor(650.f, 920.f), name.c_str(), px, team == 0 ? kBlue : kOrange);
	if (timeOk) draw(anchor(1361.f, 1000.f), timeText.c_str(), 26.f * u, C(236, 236, 236));
	if (jumpOk) { // status (what is happening now) in colour, the action a key press would trigger below it in grey
		const ImVec2 pos = anchor(1240.f, 905.f);
		const ImU32 col = jumped ? C(255, 60, 60) : C(114, 225, 78);
		d->AddCircleFilled(ImVec2(pos.x + 8.f * u, pos.y + px * 0.55f), 7.f * u, col);
		draw(ImVec2(pos.x + 24.f * u, pos.y), Tr(jumped ? ui::stDriving : ui::stAuto), px, col);
		char hint[120];
		snprintf(hint, sizeof hint, Tr(ui::hudPress), Tr(jumped ? ui::ctlJumpOut : ui::ctlJumpIn));
		draw(ImVec2(pos.x + 24.f * u, pos.y + px + 4.f * u), hint, 17.f * u, C(190, 198, 208));
	}
	if (p->showKeyGuide_ && (nameOk || timeOk || jumpOk)) { // small key guide, top-left
		const char* g = Tr(ui::guideText);
		const float gpx = 15.f * u;
		const ImVec2 gs = TextSize(g, gpx);
		const ImVec2 pos = anchor(24.f, 24.f);
		const float pad = 8.f * u;
		d->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad), ImVec2(pos.x + gs.x + pad, pos.y + gs.y + pad), C(8, 12, 20, 140), 6.f * u);
		TextOn(d, pos, g, gpx, C(214, 220, 228));
	}
}

// "preparing" message while the private match is loading / spawning bots, between triggering reclip_privateMatch and
// ControllGame() actually starting playback
void UiPainter::Preparing()
{
	if (!p->isPreparingSession) return;
	SetupOverlay();
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const char* base = Tr(ui::ovPreparing);
	const float px = 34.f * u;
	const int dots = (int)(ImGui::GetTime() / 0.5) % 4;
	const std::string full = std::string(base) + "...";
	const ImVec2 ts = TextSize(full.c_str(), px);
	const float padX = 36.f * u, padY = 22.f * u;
	const ImVec2 a(disp.x * 0.5f - ts.x * 0.5f - padX, disp.y * 0.3f - ts.y * 0.5f - padY);
	const ImVec2 b(a.x + ts.x + 2.f * padX, a.y + ts.y + 2.f * padY);
	ImDrawList* d = ImGui::GetForegroundDrawList();
	d->AddRectFilled(a, b, C(15, 18, 23, 225), 8.f * u);
	d->AddRect(a, b, kAmber, 8.f * u, ImDrawCornerFlags_All, 1.5f);
	d->AddRectFilled(a, ImVec2(b.x, a.y + 3.f * u), kAmber, 8.f * u, ImDrawCornerFlags_Top);
	const std::string shown = std::string(base) + std::string(dots, '.');
	TextOn(d, ImVec2(a.x + padX, a.y + padY), shown.c_str(), px, kAmber);
}

// progress bar + cancel while a replay is being converted
void UiPainter::Progress()
{
	if (!p->cvarManager->getCvar("reclip_replaySave").getBoolValue() || !p->gameWrapper->IsInReplay()) {
		gProgressShown = false;
		return;
	}
	if (!gProgressShown) { gProgressShown = true; gProgressStart = ImGui::GetTime(); gProgressFrame = -1; }

	const int currentFrame = p->gameWrapper->GetGameEventAsReplay().GetCurrentReplayFrame();
	const int totalFrames = p->ConvertEndFrame - p->ConvertStartFrame;
	float progress = 0.f;
	if (totalFrames > 0) progress = (float)(currentFrame - p->ConvertStartFrame) / (float)totalFrames;
	progress = std::clamp(progress, 0.f, 1.f);

	// the conversion only advances while the replay plays: if the frame stops moving, the replay is paused in the game
	const double now = ImGui::GetTime();
	if (currentFrame != gProgressFrame) { gProgressFrame = currentFrame; gProgressFrameTime = now; }
	const bool paused = now - gProgressFrameTime > 1.5 && now - gProgressStart > 1.5;

	SetupOverlay();
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const float warnH = paused ? TextSize(Tr(ui::ovPaused), 15.f * u, 500.f * u - 44.f * u).y + 10.f * u : 0.f;
	const ImVec2 size(500.f * u, 118.f * u + warnH);
	ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.07f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
	if (ImGui::Begin("##rct_progress", nullptr, flags)) {
		dl = ImGui::GetWindowDrawList();
		const ImVec2 o = ImGui::GetWindowPos();
		const ImVec2 e(o.x + size.x, o.y + size.y);
		dl->AddRectFilled(o, e, C(15, 18, 23, 235), 8.f * u);
		dl->AddRect(o, e, kLine, 8.f * u, ImDrawCornerFlags_All, 1.5f);
		dl->AddRectFilled(o, ImVec2(e.x, o.y + 3.f * u), kAmber, 8.f * u, ImDrawCornerFlags_Top);

		const float pad = 22.f * u;
		Text(ImVec2(o.x + pad, o.y + 16.f * u), Tr(ui::ovConverting), 19.f * u, kText);
		char pct[16]; snprintf(pct, sizeof pct, "%d%%", (int)(progress * 100.f));
		const ImVec2 ps = TextSize(pct, 19.f * u);
		Text(ImVec2(e.x - pad - ps.x, o.y + 16.f * u), pct, 19.f * u, kAmber);

		const float by = o.y + 52.f * u, bh = 8.f * u, bw = size.x - 2.f * pad;
		dl->AddRectFilled(ImVec2(o.x + pad, by), ImVec2(o.x + pad + bw, by + bh), kTrack, bh * 0.5f);
		if (progress > 0.f) dl->AddRectFilled(ImVec2(o.x + pad, by), ImVec2(o.x + pad + bw * progress, by + bh), kAmber, bh * 0.5f);

		if (paused) Para(ImVec2(o.x + pad, o.y + 68.f * u), Tr(ui::ovPaused), 15.f * u, kAmber, size.x - 2.f * pad);
		const char* cancel = Tr(ui::btnCancel);
		const float cwid = ButtonW(cancel, 15.f, 36.f);
		if (Button("##cancel", ImVec2(e.x - pad - cwid, o.y + 72.f * u + warnH), ImVec2(cwid, 32.f * u), cancel, 2, ImGui::GetTime() - gProgressStart > 1.0, 15.f)) {
			ReplayClipTrainer* pl = p;
			p->gameWrapper->SetTimeout([pl](GameWrapper*) { pl->CancelConversion(); }, 0.f);
		}
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}

// asks whether to disable a plugin that is known to be incompatible
void UiPainter::Compat()
{
	SetupOverlay();
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const ImVec2 size(620.f * u, 190.f * u);
	ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
	if (ImGui::Begin("##rct_compat", nullptr, flags)) {
		dl = ImGui::GetWindowDrawList();
		const ImVec2 o = ImGui::GetWindowPos();
		const ImVec2 e(o.x + size.x, o.y + size.y);
		dl->AddRectFilled(o, e, kBg, 8.f * u);
		dl->AddRect(o, e, kLine, 8.f * u, ImDrawCornerFlags_All, 1.5f);
		dl->AddRectFilled(o, ImVec2(e.x, o.y + 3.f * u), kRed, 8.f * u, ImDrawCornerFlags_Top);

		ReplayClipTrainer* pl = p;
		const std::string plugin = p->compatibility.currentNotCompatiblePlugin;
		const float pad = 24.f * u;
		char msg[300];
		snprintf(msg, sizeof msg, Tr(ui::cmpAsk), plugin.c_str());
		Para(ImVec2(o.x + pad, o.y + 22.f * u), msg, 17.f * u, kText, size.x - 2.f * pad);

		const float by = o.y + 84.f * u, bhgt = 38.f * u;
		const char* sDis = Tr(ui::cmpDisable);
		const char* sKeep = Tr(ui::cmpKeep);
		const float w1 = ButtonW(sDis, 16.f, 40.f), w2 = ButtonW(sKeep, 16.f, 40.f);
		if (Button("##cmpdis", ImVec2(o.x + pad, by), ImVec2(w1, bhgt), sDis, 1)) {
			p->compatibility.needsGUI = false;
			p->gameWrapper->SetTimeout([pl, plugin](GameWrapper*) {
				pl->cvarManager->executeCommand("plugin unload " + plugin);
				LOG("{} was disabled by ReplayClipTrainer", plugin);
			}, 0.f);
		}
		if (Button("##cmpkeep", ImVec2(o.x + pad + w1 + 10.f * u, by), ImVec2(w2, bhgt), sKeep, 2)) {
			p->compatibility.needsGUI = false;
			p->gameWrapper->SetTimeout([pl](GameWrapper*) { pl->cvarManager->executeCommand("reclip_replaySave 0"); }, 0.f);
		}
		CvarToggle("##cmpnoask", ImVec2(o.x + pad, by + 56.f * u), size.x - 2.f * pad, ui::cmpNoAsk, "reclip_doNotAskForDisableOfIncompatiblePlugins");
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}

// =====================================================================================================================
// plugin side: fonts, setup, window plumbing
// =====================================================================================================================

// Reads the goals and highlights of the replay being watched (game thread).
void ReplayClipTrainer::RefreshReplayMarkers()
{
	std::vector<ReplayMarker> marks;
	if (gameWrapper->IsInReplay()) {
		ReplayWrapper replay = gameWrapper->GetGameEventAsReplay().GetReplay();
		if (!replay.IsNull()) {
			ReplaySoccarWrapper soccar(replay.memory_address);
			const std::vector<ScoredGoal> goals = soccar.GetGoals();
			const std::vector<Highlight> highlights = soccar.GetHighlights();
			const int fps = (std::max)(1, gameWrapper->GetGameEventAsReplay().GetReplayFPS());
			for (const ScoredGoal& g : goals) marks.push_back({ g.frame, g.player_team == 0 ? 0 : 1, g.player_name });
			for (const Highlight& h : highlights) {
				bool nearGoal = false; // goals are highlights too - keep only the extra ones
				for (const ScoredGoal& g : goals) if (std::abs(g.frame - h.frame) < 5 * fps) nearGoal = true;
				if (!nearGoal) marks.push_back({ h.frame, 2, std::string() });
			}
			LOG("[markers] {} goals, {} highlights", goals.size(), highlights.size());
		}
	}
	{
		std::lock_guard<std::mutex> lock(markersMutex_);
		replayMarkers_ = std::move(marks);
	}
	markersPending_ = false;
}

void ReplayClipTrainer::SetupUi()
{
	// language / size / opacity of the window and overlays (saved by BakkesMod)
	cvarManager->registerCvar("reclip_language", "1", "Language of the window and on-screen text: 0 English, 1 Japanese", true, true, 0.f, true, 1.f)
		.addOnValueChanged([this](std::string, CVarWrapper cv) { uiJa_ = cv.getIntValue() == 1; });
	uiJa_ = cvarManager->getCvar("reclip_language").getIntValue() == 1;
	cvarManager->registerCvar("reclip_ui_scale", "0.85", "Settings window size (0.5 - 1.0)", true, true, 0.5f, true, 1.0f)
		.addOnValueChanged([this](std::string, CVarWrapper cv) { uiScale_ = cv.getFloatValue(); });
	uiScale_ = cvarManager->getCvar("reclip_ui_scale").getFloatValue();
	cvarManager->registerCvar("reclip_ui_opacity", "0.94", "Settings window opacity (0.2 - 1.0)", true, true, 0.2f, true, 1.0f)
		.addOnValueChanged([this](std::string, CVarWrapper cv) { uiOpacity_ = cv.getFloatValue(); });
	uiOpacity_ = cvarManager->getCvar("reclip_ui_opacity").getFloatValue();
	cvarManager->registerCvar("reclip_anonymize", "0", "Replace player names with Player1, Player2... when saving a clip (spectated player = 1, then teammates, then opponents)", true, true, 0.f, true, 1.f);
	cvarManager->registerCvar("reclip_showKeyGuide", "1", "Show a small key guide on the training HUD", true, true, 0.f, true, 1.f)
		.addOnValueChanged([this](std::string, CVarWrapper cv) { showKeyGuide_ = cv.getBoolValue(); });
	showKeyGuide_ = cvarManager->getCvar("reclip_showKeyGuide").getBoolValue();
	cvarManager->registerCvar("reclip_ui_w", "560", "Settings window width at 100% size (drag the bottom-right grip)", true, true, 400.f, true, 2000.f);
	cvarManager->registerCvar("reclip_ui_h", "640", "Settings window height at 100% size (drag the bottom-right grip)", true, true, 360.f, true, 2000.f);
	uiW_ = cvarManager->getCvar("reclip_ui_w").getFloatValue();
	uiH_ = cvarManager->getCvar("reclip_ui_h").getFloatValue();

	// reclip_window shows / hides the settings panel (same as `togglemenu ReplayClipTrainer`)
	cvarManager->registerNotifier("reclip_window", [this](std::vector<std::string>) {
		controlsVisible_ = !controlsVisible_;
		if (!isWindowOpen_) cvarManager->executeCommand("togglemenu " + GetMenuName());
	}, "Show / hide the ReplayClipTrainer window", PERMISSION_ALL);

	// fonts: copy a system font into BakkesMod's data/fonts and request it at several sizes
	{
		std::filesystem::path fontDir = gameWrapper->GetDataFolder() / "fonts";
		std::error_code ec;
		std::filesystem::create_directories(fontDir, ec);
		std::filesystem::path dst = fontDir / "rct_bahnschrift.ttf";
		if (!std::filesystem::exists(dst)) std::filesystem::copy_file("C:\\Windows\\Fonts\\bahnschrift.ttf", dst, ec);
		if (ec) LOG("[font] could not copy bahnschrift.ttf: {}", ec.message());
		GuiManagerWrapper gui = gameWrapper->GetGUIManager();
		for (int i = 0; i < kFontCount; ++i) {
			auto [status, font] = gui.LoadFont("rct_" + std::to_string(kFontPx[i]), "rct_bahnschrift.ttf", kFontPx[i]);
			LOG("[font] rct_{}: status {}", kFontPx[i], status);
		}

		// Japanese: BIZ UDGothic. Glyphs = ASCII + every character of the UI strings + ImGui's common Japanese set, so
		// clip / folder / player names (which the plugin cannot know in advance) show up too.
		std::filesystem::path jpDst = fontDir / "rct_jp.ttc";
		if (!std::filesystem::exists(jpDst)) std::filesystem::copy_file("C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc", jpDst, ec);
		if (ec) LOG("[font] could not copy BIZ-UDGothicR.ttc: {}", ec.message());
		ImFontGlyphRangesBuilder gb;
		for (unsigned ch = 0x20; ch <= 0x7E; ++ch) gb.AddChar((ImWchar)ch);
		gb.AddChar((ImWchar)0x00B0);
		gb.AddChar((ImWchar)0x00B7);
		{
			ImFontAtlas atlas; // only used for its static glyph-range tables
			gb.AddRanges(atlas.GetGlyphRangesJapanese());
		}
		for (const char* s : ui::kAllJa) gb.AddText(s);
		ImVector<ImWchar> ranges;
		gb.BuildRanges(&ranges);
		jaRanges_.assign(ranges.Data, ranges.Data + ranges.Size);
		for (int i = 0; i < kFontCount; ++i) {
			auto [status, font] = gui.LoadFont("rct_jp_" + std::to_string(kFontPx[i]), "rct_jp.ttc", kFontPx[i], nullptr, reinterpret_cast<const ImWchar*>(jaRanges_.data()));
			LOG("[font] rct_jp_{}: status {}", kFontPx[i], status);
		}
		LOG("[font] Japanese glyph ranges: {} entries", jaRanges_.size());
		fontsRequested_ = true;
	}

	// open the (invisible) ImGui window so Render() runs every frame; a 1 s loop keeps it open
	gameWrapper->SetTimeout([this](GameWrapper*) { UiKeepAlive(); }, 3.0f);
}

void ReplayClipTrainer::UiKeepAlive()
{
	if (unloading_) return;
	//while Esc is held (game menu) BakkesMod closes the window on every key repeat - reopening now would flicker
	if (!isWindowOpen_ && !(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) cvarManager->executeCommand("togglemenu " + GetMenuName());
	gameWrapper->SetTimeout([this](GameWrapper*) { UiKeepAlive(); }, 1.0f);
}

void ReplayClipTrainer::EnsureFonts()
{
	if (!fontsRequested_) return;
	GuiManagerWrapper gui = gameWrapper->GetGUIManager();
	bool jaOk = true;
	for (int i = 0; i < kFontCount; ++i) {
		if (!fonts_[i]) fonts_[i] = gui.GetFont("rct_" + std::to_string(kFontPx[i]));
		if (!fontsJa_[i]) fontsJa_[i] = gui.GetFont("rct_jp_" + std::to_string(kFontPx[i]));
		jaOk = jaOk && fontsJa_[i];
	}
	jpReady_ = jaOk; // text falls back to English until all Japanese sizes are loaded
}

void ReplayClipTrainer::SetLanguage(bool japanese)
{
	uiJa_ = japanese;
	cvarManager->getCvar("reclip_language").setValue(japanese ? 1 : 0);
	SaveConfigSoon();
}

void ReplayClipTrainer::PublishHudName(bool valid, const std::string& name, int team)
{
	std::lock_guard<std::mutex> lock(hudMutex_);
	hudNameValid_ = valid;
	if (valid) {
		hudName_ = name;
		hudTeam_ = team;
		hudStamp_ = std::chrono::steady_clock::now();
	}
}

void ReplayClipTrainer::PublishHudTime(const std::string& text)
{
	std::lock_guard<std::mutex> lock(hudMutex_);
	hudTimeValid_ = true;
	hudTime_ = text;
	hudStamp_ = std::chrono::steady_clock::now();
}

void ReplayClipTrainer::PublishHudJump(bool jumpedIn)
{
	std::lock_guard<std::mutex> lock(hudMutex_);
	hudJumpValid_ = true;
	hudJumpedIn_ = jumpedIn;
	hudStamp_ = std::chrono::steady_clock::now();
}

void ReplayClipTrainer::RenderControls()
{
	UiPainter ui_{ this };
	ui_.Setup();
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const ImVec2 size(uiW_ * ui_.u, uiH_ * ui_.u);
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(disp.x - size.x - 24.f, 60.f), ImGuiCond_FirstUseEver); // starts at the right edge
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
	bool open = true;
	if (ImGui::Begin("##rct_controls", nullptr, flags)) ui_.Frame(&open);
	ImGui::End();
	ImGui::PopStyleVar(2);
	if (!open) controlsVisible_ = false;
}

// Do ImGui rendering here
void ReplayClipTrainer::Render()
{
	EnsureFonts();
	UiPainter ui_{ this };
	ui_.Hud();
	ui_.Preparing();
	ui_.Progress();
	if (compatibility.needsGUI) ui_.Compat();
	if (controlsVisible_) RenderControls();
}

// Name of the menu that is used to toggle the window.
std::string ReplayClipTrainer::GetMenuName()
{
	return "ReplayClipTrainer";
}

// Title to give the menu
std::string ReplayClipTrainer::GetMenuTitle()
{
	return menuTitle_;
}

// Don't call this yourself, BM will call this function with a pointer to the current ImGui context
void ReplayClipTrainer::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

// Should events such as mouse clicks/key inputs be blocked so they won't reach the game
bool ReplayClipTrainer::ShouldBlockInput()
{
	return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

// Return true if window should be interactive
bool ReplayClipTrainer::IsActiveOverlay()
{
	return true;
}

// Called when window is opened
void ReplayClipTrainer::OnOpen()
{
	isWindowOpen_ = true;
}

// Called when window is closed. The ImGui window is meant to stay open (it hosts the overlays), so a close - e.g. from
// `togglemenu ReplayClipTrainer` - is read as "toggle the settings panel" and the window is opened again right away.
void ReplayClipTrainer::OnClose()
{
	isWindowOpen_ = false;
	if (unloading_) return;
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return; //closed by Esc / the game menu: leave the panel state alone, UiKeepAlive() reopens it after Esc is released
	controlsVisible_ = !controlsVisible_;
	gameWrapper->SetTimeout([this](GameWrapper*) {
		if (!unloading_ && !isWindowOpen_) cvarManager->executeCommand("togglemenu " + GetMenuName());
	}, 0.f);
}
