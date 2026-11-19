/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

// the lights in the field of work of the events (experimental). The lights of a model, its lights, lightcolors and notransition,
// changed in the Inspector and written into its definition on save. And the switches of the lights: an event lights which gives
// the lights of models other states, with the event launchers which set it off, all of the plain kinds any build reads, written
// on each save to an include of the editor's own, <scenery>_swiatla.scm. A line of a comment before each keeps what the editor
// needs to read it back

#include "stdafx.h"
#include "application/editormode.h"
#include "application/editormodeevents.h"
#include "application/editormodeshared.h"
#include "application/editorprojection.h"
#include "application/editoruilayer.h"
#include "editor/editorFormat.hpp"
#include "model/AnimModel.h"
#include "rendering/renderer.h"
#include "scene/scenelayers.h"
#include "simulation/simulation.h"
#include "utilities/Globals.h"
#include "utilities/translation.h"

#include "imgui/imgui.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

using editor_events::fields_of;
using editor_events::hhmm;
using editor_events::name_of_typed;
using editor_events::word_of;

char const *const kLightsHeader{"// made by the scenery editor on each save: the switches of the lights of the scenery (Events, Lights)"};
// the line of each switch the editor reads back
std::string_view constexpr kLightsMark{"//$l "};
std::size_t constexpr kLightsFields{18};
// m, how far from the camera the models with lights are marked
double constexpr kLightsMarked{400.0};
std::size_t constexpr kLightsCount{8};

// the press of the left button landed in the view, not on a window: a pick of a press meant for the UI is dropped
bool press_in_view()
{
	return false == ImGui::GetIO().MouseDownOwned[0];
}

std::string lights_file()
{
	return editor_events::editor_file("_swiatla.scm");
}

// the kinds of states a light takes, as the editor shows them: each is a part of the value a model reads
enum class light_kind
{
	unchanged, // -1, for the switches only
	off,
	on,
	blinking, // the period in s: under 1 s mode 0 with it as the fraction, 1 s mode 2 alone, over 1 s mode 2 with 1/period
	pulsing, // 1 s, lit for a part of it: mode 1 with it as the fraction
	dark, // lit in the dark: mode 3, the fraction the level of the light it comes on at (0: the default one)
	home, // as dark, off late at night: mode 4
};

struct light_value
{
	light_kind kind{light_kind::off};
	float amount{0.0f}; // the period, the part lit or the level, by the kind
	bool opposite{false}; // blinking or pulsing in the opposite phase: the value below 0
};

light_value value_of(float const Value)
{
	light_value result;
	if (Value == -1.0f)
	{
		result.kind = light_kind::unchanged;
		return result;
	}
	result.opposite = Value < 0.0f;
	auto const magnitude{std::abs(Value)};
	auto const mode{static_cast<int>(magnitude)};
	auto const fraction{magnitude - static_cast<float>(mode)};
	auto const whole{fraction < 0.01f};
	switch (mode)
	{
	case 0:
		result.kind = whole ? light_kind::off : light_kind::blinking;
		result.amount = fraction;
		break;
	case 1:
		result.kind = whole ? light_kind::on : light_kind::pulsing;
		result.amount = fraction;
		break;
	case 2:
		result.kind = light_kind::blinking;
		result.amount = whole ? 1.0f : 1.0f / fraction;
		break;
	case 3:
		result.kind = light_kind::dark;
		result.amount = fraction;
		break;
	default:
		result.kind = light_kind::home;
		result.amount = fraction;
		break;
	}
	return result;
}

float blinking_value(float const Period)
{
	// under a second the fraction of the mode 0, over it the frequency as the fraction of the mode 2
	if (Period < 0.995f)
		return std::clamp(Period, 0.02f, 0.99f);
	if (Period <= 1.005f)
		return 2.0f;
	return 2.0f + std::clamp(1.0f / Period, 0.01f, 0.99f);
}

float value_from(light_value const &Light)
{
	float value{0.0f};
	switch (Light.kind)
	{
	case light_kind::unchanged:
		return -1.0f;
	case light_kind::on:
		value = 1.0f;
		break;
	case light_kind::blinking:
		value = blinking_value(Light.amount);
		break;
	case light_kind::pulsing:
		value = 1.0f + std::clamp(Light.amount, 0.01f, 0.99f);
		break;
	case light_kind::dark:
		value = 3.0f + std::clamp(Light.amount, 0.0f, 0.99f);
		break;
	case light_kind::home:
		value = 4.0f + std::clamp(Light.amount, 0.0f, 0.99f);
		break;
	default:
		break;
	}
	// -1 would be read as no change: a light in the opposite phase is never off
	auto const phased{Light.opposite && (Light.kind == light_kind::blinking || Light.kind == light_kind::pulsing)};
	return phased ? -value : value;
}

// the amount of a kind, as the field which changes it shows it
bool render_light_amount(light_value &Light)
{
	switch (Light.kind)
	{
	case light_kind::blinking:
		if (Light.amount <= 0.0f)
			Light.amount = 1.0f;
		return ImGui::DragFloat("##amount", &Light.amount, 0.01f, 0.02f, 100.0f, STR_C("every %.2f s"));
	case light_kind::pulsing:
		return ImGui::SliderFloat("##amount", &Light.amount, 0.01f, 0.99f, STR_C("lit %.2f of a second"));
	case light_kind::dark:
	case light_kind::home:
		return ImGui::SliderFloat("##amount", &Light.amount, 0.0f, 0.99f, STR_C("comes on at %.2f (0: as usual)"));
	default:
		return false;
	}
}

// a state of a light: its kind, its amount, its phase; true if changed
bool render_light_value(char const *Id, float &Value, bool const Unchanged)
{
	ImGui::PushID(Id);
	auto light{value_of(Value)};
	std::array const kinds{STR_C("No change"), STR_C("Unlit"), STR_C("Lit"), STR_C("Blinking"), STR_C("Pulsing"), STR_C("In the dark"), STR_C("In the dark, off late at night")};
	// the switches leave a light as it is, the model itself always has a state
	auto const first{Unchanged ? 0 : 1};
	auto kind{static_cast<int>(light.kind) - first};
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
	auto changed{ImGui::Combo("##kind", &kind, kinds.data() + first, static_cast<int>(kinds.size()) - first)};
	if (changed)
	{
		light.kind = static_cast<light_kind>(kind + first);
		light.amount = light.kind == light_kind::blinking ? 1.0f : 0.0f;
	}
	if (light.kind == light_kind::blinking || light.kind == light_kind::pulsing || light.kind == light_kind::dark || light.kind == light_kind::home)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (light.kind == light_kind::blinking || light.kind == light_kind::pulsing ? 70.0f : 0.0f));
		changed |= render_light_amount(light);
	}
	if (light.kind == light_kind::blinking || light.kind == light_kind::pulsing)
	{
		ImGui::SameLine();
		changed |= ImGui::Checkbox(STR_C("Opposite##phase"), &light.opposite);
		item_tooltip("Blinks when the light set the same way doesn't: the two lights of a level crossing");
	}
	if (changed)
		Value = value_from(light);
	ImGui::PopID();
	return changed;
}

std::string number_of(float const Value)
{
	std::ostringstream text;
	text.imbue(std::locale::classic());
	text << Value;
	return text.str();
}

std::string join(std::vector<std::string> const &Items, char const Separator)
{
	std::string text;
	for (auto const &item : Items)
	{
		if (false == text.empty())
			text += Separator;
		text += item;
	}
	return text;
}

// the states of the lights the event gives: up to the last one which changes, -1 for the ones before it left as they are
std::string states_text(std::array<float, 8> const &States)
{
	std::size_t count{0};
	for (std::size_t index = 0; index < States.size(); ++index)
		if (States[index] != -1.0f)
			count = index + 1;
	std::string text;
	for (std::size_t index = 0; index < std::max<std::size_t>(count, 1); ++index)
		text += ' ' + number_of(States[index]);
	return text;
}

std::string event_text(editor_mode_light_switch const &Switch, std::string_view const Suffix, std::array<float, 8> const &States)
{
	std::string text{"event " + Switch.name + std::string{Suffix} + " lights " + format("%.1f", std::max(0.0f, Switch.delay)) + ' ' + join(Switch.models, '|') + states_text(States)};
	if (Switch.random > 0.0f)
		text += format(" randomdelay %.1f", Switch.random);
	return text + " endevent\r\n";
}

std::string launcher_text(editor_mode_light_switch const &Switch, std::string_view const Suffix, std::string const &Head, std::string_view const Event)
{
	auto const &at{Switch.location};
	std::string text{"node -1 0 " + Switch.name + std::string{Suffix} + " eventlauncher " + format("%.3f %.3f %.3f ", at.x, at.y, at.z) + Head + ' ' + Switch.name + std::string{Event}};
	if (false == Switch.cell.empty())
		text += " condition " + Switch.cell + ' ' + word_of(Switch.text) + ' ' + word_of(Switch.value1) + ' ' + word_of(Switch.value2);
	return text + " end\r\n";
}

std::string states_line(std::array<float, 8> const &States)
{
	std::string text;
	for (auto const state : States)
		text += (text.empty() ? "" : ";") + number_of(state);
	return text;
}

std::string switch_line(editor_mode_light_switch const &Switch)
{
	auto const &at{Switch.location};
	return std::string{kLightsMark} + Switch.name + '|' + format("%.3f|%.3f|%.3f", at.x, at.y, at.z) + '|' + join(Switch.models, ';') + '|' + std::to_string(Switch.when) + '|' + std::to_string(Switch.from) + '|' + std::to_string(Switch.to) + '|' +
	       states_line(Switch.on) + '|' + states_line(Switch.off) + '|' + format("%.1f|%.1f|%.1f", Switch.reach, Switch.delay, Switch.random) + '|' + Switch.key + '|' + Switch.cell + '|' + word_of(Switch.text) + '|' + word_of(Switch.value1) + '|' +
	       word_of(Switch.value2) + "\r\n";
}

// the events and the launchers of a switch, as the simulator reads them
std::string switch_text(editor_mode_light_switch const &Switch)
{
	std::string text{switch_line(Switch)};
	auto const reach{format("%.1f", std::max(1.0f, Switch.reach))};
	switch (Switch.when)
	{
	case 1:
		// on at an hour and off at another, anywhere in the scenery
		text += event_text(Switch, "_on", Switch.on);
		text += event_text(Switch, "_off", Switch.off);
		text += launcher_text(Switch, "_from", "-1 none " + hhmm(Switch.from), "_on");
		text += launcher_text(Switch, "_to", "-1 none " + hhmm(Switch.to), "_off");
		break;
	case 2:
		// on a key, while the camera is near: an interval of 0 and no hour leave the key alone
		text += event_text(Switch, "_on", Switch.on);
		text += launcher_text(Switch, "_launcher", reach + ' ' + (Switch.key.empty() ? std::string{"none"} : Switch.key) + " 0", "_on");
		break;
	case 3:
		// once, as the camera first comes near: an interval of -10000 sets it off once
		text += event_text(Switch, "_on", Switch.on);
		text += launcher_text(Switch, "_launcher", reach + " none -10000", "_on");
		break;
	default:
		// at an hour, anywhere in the scenery
		text += event_text(Switch, "_on", Switch.on);
		text += launcher_text(Switch, "_from", "-1 none " + hhmm(Switch.from), "_on");
		break;
	}
	return text;
}

bool read_states(std::string const &Field, std::array<float, 8> &States)
{
	std::istringstream input{Field};
	input.imbue(std::locale::classic());
	std::string state;
	std::size_t index{0};
	while (std::getline(input, state, ';') && index < States.size())
		States[index++] = std::stof(state);
	return index == States.size();
}

bool read_switch(std::string_view Line, editor_mode_light_switch &Switch)
{
	if (false == Line.starts_with(kLightsMark))
		return false;
	Line.remove_prefix(kLightsMark.size());
	auto const fields{fields_of(Line)};
	if (fields.size() < kLightsFields)
		return false;
	try
	{
		Switch.name = fields[0];
		Switch.location = {std::stod(fields[1]), std::stod(fields[2]), std::stod(fields[3])};
		std::istringstream models{fields[4]};
		for (std::string model; std::getline(models, model, ';');)
			if (false == model.empty())
				Switch.models.push_back(model);
		Switch.when = std::clamp(std::stoi(fields[5]), 0, 3);
		Switch.from = std::stoi(fields[6]);
		Switch.to = std::stoi(fields[7]);
		if (false == read_states(fields[8], Switch.on) || false == read_states(fields[9], Switch.off))
			return false;
		Switch.reach = std::stof(fields[10]);
		Switch.delay = std::stof(fields[11]);
		Switch.random = std::stof(fields[12]);
		Switch.key = fields[13];
		Switch.cell = fields[14];
		Switch.text = fields[15];
		Switch.value1 = fields[16];
		Switch.value2 = fields[17];
	}
	catch (std::exception const &)
	{
		return false;
	}
	return false == Switch.name.empty() && false == Switch.models.empty();
}

editor_mode_model_lights lights_of(TAnimModel &Model)
{
	editor_mode_model_lights lights;
	lights.model = &Model;
	std::ranges::copy(Model.lsLights, lights.states.begin());
	std::ranges::copy(Model.m_lightcolors, lights.colours.begin());
	lights.transition = Model.m_transition;
	return lights;
}

void set_lights(editor_mode_model_lights const &Lights)
{
	auto &model{*Lights.model};
	std::ranges::copy(Lights.states, model.lsLights.begin());
	std::ranges::copy(Lights.colours, model.m_lightcolors.begin());
	model.m_transition = Lights.transition;
	scene::Layers.lit(&model);
}

bool lights_exist(TAnimModel const *Model)
{
	return Model != nullptr && std::ranges::find(simulation::Instances.sequence(), Model) != simulation::Instances.sequence().end();
}

// the model of a switch, by its name
TAnimModel *model_named(std::string const &Name)
{
	return simulation::Instances.find(Name);
}

// how many lights the models of a switch have, the most of them, at least one
int lights_count(editor_mode_light_switch const &Switch)
{
	auto count{1};
	for (auto const &name : Switch.models)
		if (auto const *model{model_named(name)})
			count = std::max(count, model->iNumLights);
	return std::min(count, static_cast<int>(kLightsCount));
}

} // namespace

void editor_mode::lights_load()
{
	auto &state{m_events};
	auto const file{lights_file()};
	if (file.empty())
		return;
	std::ifstream input{Global.asCurrentSceneryPath + file, std::ios_base::binary};
	std::string line;
	while (std::getline(input, line))
	{
		if (false == line.empty() && line.back() == '\r')
			line.pop_back();
		if (light_switch lights; read_switch(line, lights))
			state.switches.push_back(std::move(lights));
	}
	// read, not changed: no step of the history
	state.switches_last = state.switches;
}

bool editor_mode::light_switches(std::vector<std::string> &Rootstatements)
{
	auto const file{lights_file()};
	// a scenery the events were never opened for: whatever its file holds stays as it is
	if (file.empty() || false == m_events.loaded)
		return false;
	std::string content{std::string{kLightsHeader} + "\r\n"};
	for (auto const &lights : m_events.switches)
		content += switch_text(lights);
	return editor_events::write_include(file, content, m_events.switches.empty(), m_events.lights_included, Rootstatements);
}

void editor_mode::events_pick(bool const Insert, bool const Add)
{
	// NOTE: the pick is resolved a few frames later, by then it's known whether the press was meant for the UI
	GfxRenderer->Pick_Node_Callback([this, Insert, Add](scene::basic_node *Node) {
		if (false == press_in_view())
			return;
		auto *model{dynamic_cast<TAnimModel *>(Node)};
		events_picked(model, Insert, Add);
	});
}

void editor_mode::events_picked(TAnimModel *Model, bool const Insert, bool const Add)
{
	auto &state{m_events};
	auto const lit{Model != nullptr && Model->iNumLights > 0};
	if (lit && (Model->from_template() || false == scene::Layers.editable(Model)))
	{
		state.status = STR("The lights of this model are the ones of its template: the editor leaves them as they are");
		return;
	}
	if (Insert)
	{
		if (lit)
			lights_place(*Model);
		else
			state.status = STR("A switch goes with a model with lights: click one, they're marked in the view");
		return;
	}
	if (Add)
	{
		if (false == lit || state.switch_selected < 0 || state.switch_selected >= static_cast<int>(state.switches.size()))
			return;
		auto &models{state.switches[state.switch_selected].models};
		auto const name{lights_name_model(*Model)};
		if (std::erase(models, name) == 0)
			models.push_back(name);
		return;
	}
	events_deselect();
	if (lit)
		state.model = Model;
}

// the name a switch knows the model by: one of its own, given to it if it has none or shares it with another model
std::string editor_mode::lights_name_model(TAnimModel &Model)
{
	auto const &current{Model.name()};
	if (false == current.empty() && current != "none" && model_named(current) == &Model)
		return current;
	int number{1};
	auto name{format("lampa_%d", number)};
	while (simulation::Instances.find(name) != nullptr)
		name = format("lampa_%d", ++number);
	if (false == simulation::State.rename_model(&Model, name))
		return current;
	m_events.status = format(STR_C("The model is named %s now, for the event to find it"), name.c_str());
	return name;
}

void editor_mode::lights_place(TAnimModel &Model)
{
	auto &state{m_events};
	light_switch lights;
	int number{1};
	auto const taken = [&state](std::string const &Name) {
		return std::ranges::any_of(state.switches, [&Name](light_switch const &Switch) { return Switch.name == Name; }) || simulation::Events.FindEvent(Name + "_on") != nullptr;
	};
	lights.name = format("swiatla_%d", number);
	while (taken(lights.name))
		lights.name = format("swiatla_%d", ++number);
	lights.location = Model.location() + glm::dvec3{0.0, 2.0, 0.0};
	lights.models.push_back(lights_name_model(Model));
	// what a switch is put for most often: the lights on
	for (int index = 0; index < std::min(Model.iNumLights, static_cast<int>(kLightsCount)); ++index)
		lights.on[index] = 1.0f;
	state.switches.push_back(std::move(lights));
	events_deselect();
	state.switch_selected = static_cast<int>(state.switches.size()) - 1;
	state.status = format(STR_C("%s put; it goes into the scenery with the next save"), state.switches.back().name.c_str());
}

void editor_mode::render_model_lights(TAnimModel &Model)
{
	auto &state{m_events};
	if (false == lights_exist(&Model))
	{
		state.model = nullptr;
		return;
	}
	ImGui::SeparatorText(Model.name().empty() ? STR_C("A model with lights") : Model.name().c_str());
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", STR_C("The lights the model has from the start of the scenario, written into its definition on save. A switch of the lights changes them later"));
	ImGui::PopTextWrapPos();
	auto const before{lights_of(Model)};
	auto lights{before};
	auto const count{std::min(Model.iNumLights, static_cast<int>(kLightsCount))};
	if (begin_fields("##modellights", 0.25f))
	{
		for (int index = 0; index < count; ++index)
		{
			field_row(format(STR_C("Light %d"), index + 1).c_str(), false);
			render_light_value(format("light%d", index).c_str(), lights.states[index], false);
			auto own{lights.colours[index].r >= 0.0f};
			ImGui::PushID(index);
			if (ImGui::Checkbox(STR_C("Own colour##light"), &own))
				lights.colours[index] = own ? glm::vec3{1.0f, 0.85f, 0.6f} : glm::vec3{-1.0f};
			if (own)
			{
				ImGui::SameLine();
				ImGui::ColorEdit3("##colour", &lights.colours[index].x, ImGuiColorEditFlags_NoInputs);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::Checkbox(STR_C("Smooth change of the lights"), &lights.transition);
	if (lights == before)
		return;
	// the change from where it started, a step of the history when it's done
	if (false == state.model_base.has_value())
		state.model_base = before;
	set_lights(lights);
}

void editor_mode::render_light_switch(light_switch &Switch)
{
	auto &state{m_events};
	ImGui::SeparatorText(Switch.name.c_str());
	if (state.switch_renaming != state.switch_selected)
	{
		state.switch_renaming = state.switch_selected;
		state.switch_rename.fill('\0');
		std::copy_n(Switch.name.begin(), std::min(Switch.name.size(), state.switch_rename.size() - 1), state.switch_rename.begin());
	}
	if (begin_fields("##switchfields"))
	{
		field_row("Name##switch");
		if (ImGui::InputText("##switchname", state.switch_rename.data(), state.switch_rename.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
		{
			auto const name{name_of_typed(state.switch_rename.data())};
			auto const taken{std::ranges::any_of(state.switches, [&name](light_switch const &Other) { return Other.name == name; }) || simulation::Events.FindEvent(name + "_on") != nullptr};
			if (false == name.empty() && name != Switch.name && false == taken)
				Switch.name = name;
			else if (taken && name != Switch.name)
				state.status = format(STR_C("The name %s is taken"), name.c_str());
			state.switch_renaming = -1;
		}
		item_tooltip("Its events and launchers are named after it. Letters, digits, _ and -");
		field_row("Place##switch");
		ImGui::DragScalarN("##switchplace", ImGuiDataType_Double, &Switch.location.x, 3, 0.1f, nullptr, nullptr, "%.1f");
		item_tooltip("Where its launcher stands: the camera near it sets off a switch on a key, or as it comes near");
		ImGui::EndTable();
	}
	ImGui::SeparatorText(STR_C("Models##switch"));
	for (std::size_t index = 0; index < Switch.models.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		auto const *model{model_named(Switch.models[index])};
		if (ImGui::SmallButton("x"))
		{
			Switch.models.erase(Switch.models.begin() + static_cast<std::ptrdiff_t>(index));
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		if (model == nullptr)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), STR_C("%s (not in the scenery)"), Switch.models[index].c_str());
		else
			ImGui::TextUnformatted(Switch.models[index].c_str());
		ImGui::PopID();
	}
	ImGui::TextDisabled("%s", STR_C("Shift+LMB on a model with lights adds it, or takes it out"));
	render_light_switch_when(Switch);
	render_light_switch_states(Switch, Switch.on, Switch.when == 1 ? STR_C("At the first hour##switchon") : STR_C("The lights it gives##switchon"));
	if (Switch.when == 1)
		render_light_switch_states(Switch, Switch.off, STR_C("At the second hour##switchoff"));
	ImGui::Separator();
	auto const trying{false == state.tried.empty()};
	if (ImGui::Button(trying ? STR_C("Put the lights back##switch") : STR_C("Try it out##switch")))
	{
		if (trying)
			lights_untry();
		else
			lights_try(Switch, false);
	}
	item_tooltip("Gives the models the lights of the switch in the view, until clicked again; nothing of it is saved");
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Fly to##switch")))
		Camera.Pos = Switch.location + glm::dvec3{0.0, 15.0, 25.0};
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Delete##switch")))
		events_delete();
}

void editor_mode::render_light_switch_when(light_switch &Switch)
{
	ImGui::SeparatorText(STR_C("When##switch"));
	std::array const whens{STR_C("At an hour"), STR_C("On at an hour, off at another"), STR_C("On a key, while the camera is near"), STR_C("Once, as the camera comes near")};
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::Combo("##switchwhen", &Switch.when, whens.data(), static_cast<int>(whens.size()));
	if (false == begin_fields("##switchwhenfields"))
		return;
	if (Switch.when <= 1)
	{
		field_row(Switch.when == 1 ? "From (HHMM)" : "At (HHMM)##ambient");
		ImGui::InputInt("##switchfrom", &Switch.from, 0);
		// 0000 is read by the launcher as no hour at all
		Switch.from = std::clamp(Switch.from, 1, 2359);
	}
	if (Switch.when == 1)
	{
		field_row("To (HHMM)");
		ImGui::InputInt("##switchto", &Switch.to, 0);
		Switch.to = std::clamp(Switch.to, 1, 2359);
	}
	if (Switch.when >= 2)
	{
		field_row("Radius (m)##ambient");
		ImGui::DragFloat("##switchreach", &Switch.reach, 1.0f, 1.0f, 5000.0f, "%.0f");
	}
	if (Switch.when == 2)
	{
		field_row("Key##ambient");
		std::array<char, 2> key{};
		if (false == Switch.key.empty())
			key[0] = Switch.key.front();
		if (ImGui::InputText("##switchkey", key.data(), key.size(), ImGuiInputTextFlags_CharsNoBlank))
			Switch.key = std::isalnum(static_cast<unsigned char>(key[0])) ? std::string(1, key[0]) : std::string{};
		item_tooltip("A letter or a digit; a capital letter is the key with Shift");
	}
	field_row("Delay (s)##ambient");
	ImGui::DragFloat("##switchdelay", &Switch.delay, 0.5f, 0.0f, 3600.0f, "%.1f");
	field_row("Random delay up to (s)##ambient");
	ImGui::DragFloat("##switchrandom", &Switch.random, 0.5f, 0.0f, 3600.0f, "%.1f");
	item_tooltip("A delay picked at random each time: the lamps of a street put on one switch each come on at their own moment");
	field_row("Memory cell##ambient");
	std::array<char, 64> cell{};
	std::copy_n(Switch.cell.begin(), std::min(Switch.cell.size(), cell.size() - 1), cell.begin());
	if (ImGui::InputText("##switchcell", cell.data(), cell.size(), ImGuiInputTextFlags_CharsNoBlank))
		Switch.cell = name_of_typed(cell.data());
	item_tooltip("It's set off only while this memory cell holds what's below; empty: no condition");
	ImGui::EndTable();
	if (Switch.cell.empty())
		return;
	if (false == begin_fields("##switchcondition"))
		return;
	auto const value_field = [](char const *Label, char const *Id, std::string &Value) {
		field_row(Label);
		std::array<char, 64> text{};
		std::copy_n(Value.begin(), std::min(Value.size(), text.size() - 1), text.begin());
		if (ImGui::InputText(Id, text.data(), text.size(), ImGuiInputTextFlags_CharsNoBlank))
			Value = word_of(text.data());
	};
	value_field("Text (* any)##ambient", "##switchtext", Switch.text);
	value_field("Value 1 (* any)##ambient", "##switchvalue1", Switch.value1);
	value_field("Value 2 (* any)##ambient", "##switchvalue2", Switch.value2);
	ImGui::EndTable();
}

void editor_mode::render_light_switch_states(light_switch &Switch, std::array<float, 8> &States, char const *Id)
{
	ImGui::SeparatorText(Id);
	if (false == begin_fields(Id, 0.25f))
		return;
	auto const count{lights_count(Switch)};
	for (int index = 0; index < count; ++index)
	{
		field_row(format(STR_C("Light %d"), index + 1).c_str(), false);
		render_light_value(format("state%d", index).c_str(), States[index], true);
	}
	ImGui::EndTable();
	if (Switch.when <= 1)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", STR_C("A scenario which starts after the hour gets the lights the models have from the start: give them in the lights of each model"));
		ImGui::PopTextWrapPos();
	}
}

void editor_mode::lights_try(light_switch const &Switch, bool const Off)
{
	lights_untry();
	auto &state{m_events};
	auto const &states{Off ? Switch.off : Switch.on};
	for (auto const &name : Switch.models)
	{
		auto *model{model_named(name)};
		if (model == nullptr)
			continue;
		state.tried.push_back(lights_of(*model));
		for (std::size_t index = 0; index < states.size(); ++index)
			if (states[index] != -1.0f)
				model->LightSet(static_cast<int>(index), states[index]);
	}
}

void editor_mode::lights_untry()
{
	auto &state{m_events};
	for (auto const &lights : state.tried)
	{
		if (false == lights_exist(lights.model))
			continue;
		std::ranges::copy(lights.states, lights.model->lsLights.begin());
	}
	state.tried.clear();
}

void editor_mode::restore_lights(EditorSnapshot &Snapshot)
{
	if (false == Snapshot.lights.has_value() || false == lights_exist(Snapshot.lights->model))
		return;
	// what they are now goes to the step, for the way back
	auto current{lights_of(*Snapshot.lights->model)};
	set_lights(*Snapshot.lights);
	Snapshot.lights = std::move(current);
}

void editor_mode::draw_lights_overlay()
{
	auto const &state{m_events};
	screen_projection const projection;
	auto *drawlist{ImGui::GetBackgroundDrawList(ImGui::GetMainViewport())};
	glm::dvec3 const camera{Camera.Pos};
	// the models with lights near the camera, as small rings: they're the ones a click gives the lights of
	for (auto const *model : simulation::Instances.sequence())
	{
		ImVec2 at;
		if (model == nullptr || model->iNumLights <= 0 || glm::distance(model->location(), camera) > kLightsMarked || false == projection.project(model->location(), at))
			continue;
		auto const chosen{model == state.model};
		drawlist->AddCircle(at, chosen ? 8.0f : 5.0f, chosen ? IM_COL32(255, 190, 60, 240) : IM_COL32(255, 235, 120, 170), 4, chosen ? 2.5f : 1.5f);
	}
	for (std::size_t index = 0; index < state.switches.size(); ++index)
	{
		auto const &lights{state.switches[index]};
		auto const chosen{static_cast<int>(index) == state.switch_selected};
		auto const colour{chosen ? IM_COL32(255, 190, 60, 240) : IM_COL32(255, 225, 90, 220)};
		ImVec2 at;
		if (false == projection.project(lights.location, at))
			continue;
		drawlist->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 5.0f), ImVec2(at.x + 5.0f, at.y + 5.0f), colour, 2.0f);
		drawlist->AddText(ImVec2(at.x + 9.0f, at.y - 8.0f), colour, lights.name.c_str());
		if (false == chosen)
			continue;
		// the models it switches
		for (auto const &name : lights.models)
			if (ImVec2 to; model_named(name) != nullptr && projection.project(model_named(name)->location(), to))
				drawlist->AddLine(at, to, IM_COL32(255, 190, 60, 160), 1.5f);
	}
}
