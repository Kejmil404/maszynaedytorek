/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

// the autosave of the editor: the scenery saved by itself, the way Ctrl+S saves it, every so many minutes while it has changes not
// saved. Off unless turned on in the settings of the editor; the status bar tells how long until it saves

#include "stdafx.h"
#include "application/editormode.h"
#include "application/editoruilayer.h"
#include "editor/editorFormat.hpp"
#include "editor/editorSettings.hpp"
#include "scene/scenelayers.h"
#include "utilities/Logs.h"
#include "utilities/translation.h"

#include "imgui/imgui.h"
#include "imgui/ImGuizmo.h"
#include <algorithm>
#include <chrono>
#include <string>

bool editor_mode::unsaved_changes() const
{
	auto const step{m_history.empty() ? std::uint64_t{0} : m_history.back().serial};
	return step != m_saved_serial || m_streamer.modified() || scene::Layers.pending();
}

void editor_mode::saved_now()
{
	m_saved_serial = m_history.empty() ? std::uint64_t{0} : m_history.back().serial;
	m_autosave_due.reset();
}

bool editor_mode::autosave_idle() const
{
	// nothing held or being typed: a save in the middle of a drag would write half of it, and stop the editor for a moment
	auto const &io{ImGui::GetIO()};
	if (ImGui::IsAnyItemActive() || io.WantTextInput || ImGuizmo::IsUsing())
		return false;
	if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
		return false;
	if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		return false;
	return false == m_dem_import.loading.valid();
}

void editor_mode::autosave_update()
{
	using clock = std::chrono::steady_clock;
	if (false == EditorSettings.autosave() || scene::Layers.empty())
	{
		m_autosave_due.reset();
		ui()->set_autosave({});
		return;
	}
	auto const now{clock::now()};
	auto const interval{std::chrono::minutes{std::clamp(EditorSettings.autosave_minutes(), 1, 60)}};
	if (false == unsaved_changes())
	{
		// the time runs only while there's something to save
		m_autosave_due.reset();
		if (m_autosaved.has_value())
		{
			auto const ago{std::chrono::duration_cast<std::chrono::minutes>(now - *m_autosaved).count()};
			ui()->set_autosave(ago < 1 ? std::string{STR_C("Autosaved a moment ago")} : format(STR_C("Autosaved %d min ago"), static_cast<int>(ago)));
		}
		else
			ui()->set_autosave(STR_C("Autosave: no changes"));
		return;
	}
	if (false == m_autosave_due.has_value() || *m_autosave_due > now + interval)
		m_autosave_due = now + interval;
	if (now < *m_autosave_due)
	{
		auto const left{std::chrono::duration_cast<std::chrono::seconds>(*m_autosave_due - now).count()};
		ui()->set_autosave(left < 60 ? format(STR_C("Autosave in %d s"), static_cast<int>(left)) : format(STR_C("Autosave in %d min"), static_cast<int>((left + 59) / 60)));
		return;
	}
	if (false == autosave_idle())
	{
		ui()->set_autosave(STR_C("Autosave: as soon as the tool is let go"));
		return;
	}
	WriteLog("Editor: autosave");
	if (save())
		m_autosaved = now;
	else
		// a save which failed is tried again after the next interval, not in each frame
		m_autosave_due = now + interval;
}
