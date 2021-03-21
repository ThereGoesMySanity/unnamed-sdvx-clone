#include "stdafx.h"
#include "SettingsScreen.hpp"

#include "Application.hpp"
#include <Shared/Profiling.hpp>
#include "GameConfig.hpp"
#include "Scoring.hpp"
#include <Audio/Audio.hpp>
#include "Track.hpp"
#include "Camera.hpp"
#include "Background.hpp"
#include "Shared/Jobs.hpp"
#include "ScoreScreen.hpp"
#include "Shared/Enum.hpp"
#include "Shared/Files.hpp"
#include "Input.hpp"
#include "nanovg.h"
#include "CalibrationScreen.hpp"
#include "TransitionScreen.hpp"
#include "GuiUtils.hpp"

constexpr static int NK_PROPERTY_DEFAULT = 0;

static inline void nk_sdl_text(nk_flags event)
{
	if (event & NK_EDIT_ACTIVATED)
	{
		    SDL_StartTextInput();
	}
	if (event & NK_EDIT_DEACTIVATED)
	{
		   SDL_StopTextInput();
	}
}

static inline int nk_get_property_state(struct nk_context *ctx, const char *name)
{
    if (!ctx || !ctx->current || !ctx->current->layout) return NK_PROPERTY_DEFAULT;
	struct nk_window* win = ctx->current;
	nk_hash hash = 0;
    if (name[0] == '#') {
        hash = nk_murmur_hash(name, (int)nk_strlen(name), win->property.seq++);
        name++; /* special number hash */
    } else hash = nk_murmur_hash(name, (int)nk_strlen(name), 42);

	if (win->property.active && hash == win->property.name)
		return win->property.state;
	return NK_PROPERTY_DEFAULT;
}

static inline int nk_propertyi_sdl_text(struct nk_context *ctx, const char *name, int min, int val,
		    int max, int step, float inc_per_pixel)
{
	int oldState = nk_get_property_state(ctx, name);
	int value = nk_propertyi(ctx, name, min, val, max, step, inc_per_pixel);
	int newState = nk_get_property_state(ctx, name);

	if (oldState != newState) {
		if (newState == NK_PROPERTY_DEFAULT)
			SDL_StopTextInput();
		else
			SDL_StartTextInput();
	}

	return value;
}

static inline float nk_propertyf_sdl_text(struct nk_context *ctx, const char *name, float min,
		    float val, float max, float step, float inc_per_pixel)
{
	int oldState = nk_get_property_state(ctx, name);
	float value = nk_propertyf(ctx, name, min, val, max, step, inc_per_pixel);
	int newState = nk_get_property_state(ctx, name);

	if (oldState != newState) {
		if (newState == NK_PROPERTY_DEFAULT)
			SDL_StopTextInput();
		else
			SDL_StartTextInput();
	}

	return value;
}

static inline const char* GetKeyNameFromScancodeConfig(int scancode)
{
	return SDL_GetKeyName(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode)));
}

class SettingsPage
{
protected:
	SettingsPage(nk_context* nctx, const std::string_view& name) : m_nctx(nctx), m_name(name) {}

	virtual void Load() = 0;
	virtual void Save() = 0;

	virtual void RenderContents() = 0;

	class TextSettingData
	{
	public:
		TextSettingData(GameConfigKeys key) : m_key(key) {}

		void Load()
		{
			String str = g_gameConfig.GetString(m_key);
			m_len = static_cast<int>(str.length());

			if (m_len >= m_buffer.size())
			{
				Logf("Config key=%d cropped due to being too long (%d)", Logger::Severity::Error, static_cast<int>(m_key), m_len);
				m_len = static_cast<int>(m_buffer.size() - 1);
			}

			std::memcpy(m_buffer.data(), str.data(), m_len + 1);
		}

		void Save()
		{
			String str = String(m_buffer.data(), m_len);

			str.TrimBack('\n');
			str.TrimBack(' ');

			g_gameConfig.Set(m_key, str);
		}

		void Render(nk_context* nctx)
		{
			nk_sdl_text(nk_edit_string(nctx, NK_EDIT_FIELD, m_buffer.data(), &m_len, static_cast<int>(m_buffer.size()), nk_filter_default));
		}

		void RenderPassword(nk_context* nctx)
		{
			// Hack taken from https://github.com/vurtun/nuklear/blob/a9e5e7299c19b8a8831a07173211fa8752d0cc8c/demo/overview.c#L549
			const int old_len = m_len;
			
			std::array<char, BUFFER_SIZE> tokenBuffer;
			std::fill(tokenBuffer.begin(), tokenBuffer.begin() + m_len, '*');

			nk_sdl_text(nk_edit_string(nctx, NK_EDIT_FIELD, tokenBuffer.data(), &m_len, 1024, nk_filter_default));

			if (old_len < m_len)
			{
				std::memcpy(m_buffer.data() + old_len, tokenBuffer.data() + old_len, m_len - old_len);
			}
		}

	protected:
		constexpr static size_t BUFFER_SIZE = 1024;

		GameConfigKeys m_key;
		std::array<char, BUFFER_SIZE> m_buffer;
		int m_len = 0;
	};

	// Useful elements

	inline void LayoutRowDynamic(int num_columns)
	{
		LayoutRowDynamic(num_columns, static_cast<float>(m_buttonHeight));
	}

	inline void LayoutRowDynamic(int num_columns, float height)
	{
		nk_layout_row_dynamic(m_nctx, height, num_columns);
	}

	bool ToggleSetting(GameConfigKeys key, const std::string_view& label)
	{
		int value = g_gameConfig.GetBool(key) ? 0 : 1;
		const int prevValue = value;

		nk_checkbox_label(m_nctx, label.data(), &value);

		if (value != prevValue)
		{
			g_gameConfig.Set(key, value == 0);
			return true;
		}
		else
		{
			return false;
		}
	}

	template<typename EnumClass>
	bool EnumSetting(GameConfigKeys key, const std::string_view& label)
	{
		EnumStringMap<typename EnumClass::EnumType> nameMap = EnumClass::GetMap();
		Vector<const char*> names;

		int value = (int) g_gameConfig.GetEnum<EnumClass>(key);
		const int prevValue = value;

		for (auto it = nameMap.begin(); it != nameMap.end(); it++)
		{
			names.Add(*(*it).second);
		}

		nk_label(m_nctx, label.data(), nk_text_alignment::NK_TEXT_LEFT);
		nk_combobox(m_nctx, names.data(), names.size(), &value, m_buttonHeight, m_comboBoxSize);
		if (prevValue != value) {
			g_gameConfig.SetEnum<EnumClass>(key, nameMap.FromString(names[value]));
			return true;
		}
		return false;
	}

	bool SelectionSetting(GameConfigKeys key, const Vector<const char*>& options, const std::string_view& label)
	{
		assert(options.size() > 0);

		int value = g_gameConfig.GetInt(key) % options.size();
		const int prevValue = value;

		nk_label(m_nctx, label.data(), nk_text_alignment::NK_TEXT_LEFT);
		nk_combobox(m_nctx, const_cast<const char**>(options.data()), static_cast<int>(options.size()), &value, m_buttonHeight, m_comboBoxSize);
		if (prevValue != value) {
			g_gameConfig.Set(key, value);
			return true;
		}
		return false;
	}

	bool StringSelectionSetting(GameConfigKeys key, const Vector<String>& options, const std::string_view& label)
	{
		String value = g_gameConfig.GetString(key);
		int selection = 0;

		const auto stringSearch = std::find(options.begin(), options.end(), value);
		if (stringSearch != options.end())
		{
			selection = static_cast<int>(stringSearch - options.begin());
		}

		const int prevSelection = selection;

		Vector<const char*> displayData;
		for (const String& s : options)
		{
			displayData.Add(s.data());
		}

		nk_label(m_nctx, label.data(), nk_text_alignment::NK_TEXT_LEFT);
		nk_combobox(m_nctx, displayData.data(), static_cast<int>(options.size()), &selection, m_buttonHeight, m_comboBoxSize);

		if (prevSelection != selection) {
			String newValue = options[selection];
			value = newValue;
			g_gameConfig.Set(key, value);
			return true;
		}
		return false;
	}
	
	bool IntSetting(GameConfigKeys key, const std::string_view& label, int min, int max, int step = 1, float perPixel = 1)
	{
		int value = g_gameConfig.GetInt(key);
		const int newValue = nk_propertyi_sdl_text(m_nctx, label.data(), min, value, max, step, perPixel);
		if (newValue != value) {
			g_gameConfig.Set(key, newValue);
			return true;
		}
		return false;
	}

	bool FloatSetting(GameConfigKeys key, const std::string_view& label, float min, float max, float step = 0.01f)
	{
		float value = g_gameConfig.GetFloat(key);
		const auto prevValue = value;

		// nuklear supports precision only up to 2 decimal places (wtf)
		if (step >= 0.01f)
		{
			float incPerPixel = step;
			if (incPerPixel >= step / 2) incPerPixel = step * Math::Round(incPerPixel / step);

			value = nk_propertyf_sdl_text(m_nctx, label.data(), min, value, max, step, incPerPixel);
		}
		else
		{
			nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_LEFT, label.data(), value);
			nk_slider_float(m_nctx, min, &value, max, step);
		}

		if (value != prevValue) {
			g_gameConfig.Set(key, value);
			return true;
		}

		return false;
	}

	bool PercentSetting(GameConfigKeys key, const std::string_view& label)
	{
		float value = g_gameConfig.GetFloat(key);
		const float prevValue = value;

		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_LEFT, label.data(), value * 100);
		nk_slider_float(m_nctx, 0, &value, 1, 0.005f);

		if (value != prevValue)
		{
			g_gameConfig.Set(key, value);
			return true;
		}
		else
		{
			return false;
		}
	}

public:
	inline void Init()
	{
		Load();
	}

	inline void Exit()
	{
		Save();
	}

	inline const String& GetName() const { return m_name; }
	
	void Render(const struct nk_rect& rect)
	{
		m_comboBoxSize.x = rect.x - 30;

		if (nk_begin(m_nctx, m_name.data(), rect, NK_WINDOW_NO_SCROLLBAR))
		{
			RenderContents();
			nk_end(m_nctx);
		}
	}

protected:
	nk_context* m_nctx = nullptr;
	String m_name;

	int m_buttonHeight = 30;
	struct nk_vec2 m_comboBoxSize = nk_vec2(1050, 250);
};

class SettingsPage_Input : public SettingsPage
{
public:
	SettingsPage_Input(nk_context* nctx) : SettingsPage(nctx, "Input") {}

protected:
	void Load() override
	{
		m_gamePads.clear();

		m_gamePadsStr = g_gameWindow->GetGamepadDeviceNames();
		for (const String& gamePadName : m_gamePadsStr)
		{
			m_gamePads.Add(gamePadName.data());
		}
	}
	
	void Save() override
	{
		if (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice) == InputDevice::Mouse)
		{
			g_gameConfig.SetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice, InputDevice::Keyboard);
		}
	}

	Vector<String> m_gamePadsStr;
	Vector<const char*> m_gamePads;

	String m_controllerButtonNames[8];
	String m_controllerLaserNames[2];

	const Vector<GameConfigKeys>* m_activeBTKeys = &m_keyboardKeys;
	const Vector<GameConfigKeys>* m_activeLaserKeys = &m_keyboardLaserKeys;
	bool m_useBTGamepad = false;
	bool m_useLaserGamepad = false;
	bool m_altBinds = false;

	const Vector<GameConfigKeys> m_keyboardKeys = {
		GameConfigKeys::Key_BTS,
		GameConfigKeys::Key_BT0,
		GameConfigKeys::Key_BT1,
		GameConfigKeys::Key_BT2,
		GameConfigKeys::Key_BT3,
		GameConfigKeys::Key_FX0,
		GameConfigKeys::Key_FX1,
		GameConfigKeys::Key_Back
	};
	const Vector<GameConfigKeys> m_altKeyboardKeys = {
		GameConfigKeys::Key_BTSAlt,
		GameConfigKeys::Key_BT0Alt,
		GameConfigKeys::Key_BT1Alt,
		GameConfigKeys::Key_BT2Alt,
		GameConfigKeys::Key_BT3Alt,
		GameConfigKeys::Key_FX0Alt,
		GameConfigKeys::Key_FX1Alt,
		GameConfigKeys::Key_BackAlt
	};

	const Vector<GameConfigKeys> m_keyboardLaserKeys = {
		GameConfigKeys::Key_Laser0Neg,
		GameConfigKeys::Key_Laser0Pos,
		GameConfigKeys::Key_Laser1Neg,
		GameConfigKeys::Key_Laser1Pos,
	};
	const Vector<GameConfigKeys> m_altKeyboardLaserKeys = {
		GameConfigKeys::Key_Laser0NegAlt,
		GameConfigKeys::Key_Laser0PosAlt,
		GameConfigKeys::Key_Laser1NegAlt,
		GameConfigKeys::Key_Laser1PosAlt,
	};

	const Vector<GameConfigKeys> m_controllerKeys = {
		GameConfigKeys::Controller_BTS,
		GameConfigKeys::Controller_BT0,
		GameConfigKeys::Controller_BT1,
		GameConfigKeys::Controller_BT2,
		GameConfigKeys::Controller_BT3,
		GameConfigKeys::Controller_FX0,
		GameConfigKeys::Controller_FX1,
		GameConfigKeys::Controller_Back
	};
	const Vector<GameConfigKeys> m_controllerLaserKeys = {
		GameConfigKeys::Controller_Laser0Axis,
		GameConfigKeys::Controller_Laser1Axis,
	};

	void RenderContents() override
	{
		UpdateInputKeyBindingStatus();
		UpdateControllerInputNames();

		RenderKeyBindings();

		LayoutRowDynamic(1);

		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, "_______________________");
		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, " ");

		if (nk_button_label(m_nctx, "Calibrate Laser Sensitivity")) OpenCalibrateSensitivity();

		GameConfigKeys laserSensKey;
		switch (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice))
		{
		case InputDevice::Controller:
			laserSensKey = GameConfigKeys::Controller_Sensitivity;
			break;
		case InputDevice::Mouse:
			laserSensKey = GameConfigKeys::Mouse_Sensitivity;
			break;
		case InputDevice::Keyboard:
		default:
			laserSensKey = GameConfigKeys::Key_Sensitivity;
			break;
		}

		FloatSetting(laserSensKey, "Laser Sensitivity (%g):", 0, 20, 0.001f);
		EnumSetting<Enum_ButtonComboModeSettings>(GameConfigKeys::UseBackCombo, "Use 3xBT+Start = Back:");
		EnumSetting<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice, "Button input mode:");
		EnumSetting<Enum_InputDevice>(GameConfigKeys::LaserInputDevice, "Laser input mode:");
		EnumSetting<Enum_LaserAxisOption>(GameConfigKeys::InvertLaserInput, "Invert laser input:");

		if (m_gamePads.size() > 0)
		{
			SelectionSetting(GameConfigKeys::Controller_DeviceID, m_gamePads, "Selected Controller:");
		}

		IntSetting(GameConfigKeys::GlobalOffset, "Global Offset", -1000, 1000);
		IntSetting(GameConfigKeys::InputOffset, "Input Offset", -1000, 1000);

		if (nk_button_label(m_nctx, "Calibrate offsets")) {
			CalibrationScreen* cscreen = new CalibrationScreen(m_nctx);
			g_transition->TransitionTo(cscreen);
		}

		FloatSetting(GameConfigKeys::SongSelSensMult, "Song Select Sensitivity Multiplier", 0.0f, 20.0f, 0.1f);
		IntSetting(GameConfigKeys::InputBounceGuard, "Button Bounce Guard:", 0, 100);

		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, " ");

		EnumSetting<Enum_AbortMethod>(GameConfigKeys::RestartPlayMethod, "Restart with F5:");
		if (g_gameConfig.GetEnum<Enum_AbortMethod>(GameConfigKeys::RestartPlayMethod) == AbortMethod::Hold)
		{
			IntSetting(GameConfigKeys::RestartPlayHoldDuration, "Restart Hold Duration (ms):", 250, 10000, 250);
		}

		EnumSetting<Enum_AbortMethod>(GameConfigKeys::ExitPlayMethod, "Exit gameplay with Back:");
		if (g_gameConfig.GetEnum<Enum_AbortMethod>(GameConfigKeys::ExitPlayMethod) == AbortMethod::Hold)
		{
			IntSetting(GameConfigKeys::ExitPlayHoldDuration, "Exit Hold Duration (ms):", 250, 10000, 250);
		}

		ToggleSetting(GameConfigKeys::DisableNonButtonInputsDuringPlay, "Disable non-buttons during gameplay");

		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, " ");

		if (nk_tree_push(m_nctx, NK_TREE_NODE, "Laser Assist", NK_MINIMIZED))
		{
			FloatSetting(GameConfigKeys::LaserAssistLevel, "Base Laser Assist", 0.0f, 10.0f, 0.1f);
			FloatSetting(GameConfigKeys::LaserPunish, "Base Laser Punish", 0.0f, 10.0f, 0.1f);
			FloatSetting(GameConfigKeys::LaserChangeTime, "Direction Change Duration (ms)", 0.0f, 1000.0f, 1.0f);
			FloatSetting(GameConfigKeys::LaserChangeExponent, "Direction Change Curve Exponent", 0.0f, 10.0f, 0.1f);

			nk_tree_pop(m_nctx);
		}
	}

private:
	inline void RenderKeyBindings()
	{
		LayoutRowDynamic(3);
		if (nk_button_label(m_nctx, m_controllerLaserNames[0].c_str())) OpenLeftLaserBind();
		if (nk_button_label(m_nctx, m_controllerButtonNames[0].c_str())) OpenButtonBind((*m_activeBTKeys)[0]);
		if (nk_button_label(m_nctx, m_controllerLaserNames[1].c_str())) OpenRightLaserBind();

		LayoutRowDynamic(4);
		if (nk_button_label(m_nctx, m_controllerButtonNames[1].c_str())) OpenButtonBind((*m_activeBTKeys)[1]);
		if (nk_button_label(m_nctx, m_controllerButtonNames[2].c_str())) OpenButtonBind((*m_activeBTKeys)[2]);
		if (nk_button_label(m_nctx, m_controllerButtonNames[3].c_str())) OpenButtonBind((*m_activeBTKeys)[3]);
		if (nk_button_label(m_nctx, m_controllerButtonNames[4].c_str())) OpenButtonBind((*m_activeBTKeys)[4]);

		LayoutRowDynamic(2);
		if (nk_button_label(m_nctx, m_controllerButtonNames[5].c_str())) OpenButtonBind((*m_activeBTKeys)[5]);
		if (nk_button_label(m_nctx, m_controllerButtonNames[6].c_str())) OpenButtonBind((*m_activeBTKeys)[6]);

		if (!m_useBTGamepad)
		{
			if (!nk_option_label(m_nctx, "Primary", m_altBinds ? 1 : 0)) m_altBinds = false;
			if (!nk_option_label(m_nctx, "Alternate", m_altBinds ? 0 : 1)) m_altBinds = true;
		}

		LayoutRowDynamic(1);
		nk_label(m_nctx, "Back:", nk_text_alignment::NK_TEXT_LEFT);
		if (nk_button_label(m_nctx, m_controllerButtonNames[7].c_str())) OpenButtonBind((*m_activeBTKeys)[7]);
	}

	inline void OpenLeftLaserBind()
	{
		OpenLaserBind(GameConfigKeys::Controller_Laser0Axis);
	}
	inline void OpenRightLaserBind()
	{
		OpenLaserBind(GameConfigKeys::Controller_Laser1Axis);
	}
	inline void OpenLaserBind(GameConfigKeys axis)
	{
		const InputDevice laserInputDevice = g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice);
		OpenButtonBind(axis, laserInputDevice == InputDevice::Controller);
	}

	inline void OpenButtonBind(GameConfigKeys key)
	{
		OpenButtonBind(key, m_useBTGamepad);
	}
	inline void OpenButtonBind(GameConfigKeys key, bool gamepad)
	{
		g_application->AddTickable(ButtonBindingScreen::Create(key, gamepad, g_gameConfig.GetInt(GameConfigKeys::Controller_DeviceID), m_altBinds));
	}

	inline void OpenCalibrateSensitivity()
	{
		LaserSensCalibrationScreen* sensScreen = LaserSensCalibrationScreen::Create();
		sensScreen->SensSet.Add(this, &SettingsPage_Input::SetSensitivity);
		g_application->AddTickable(sensScreen);
	}
	inline void SetSensitivity(float sens)
	{
		switch (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice))
		{
		case InputDevice::Controller:
			g_gameConfig.Set(GameConfigKeys::Controller_Sensitivity, sens);
			break;
		case InputDevice::Mouse:
			g_gameConfig.Set(GameConfigKeys::Mouse_Sensitivity, sens);
			break;
		case InputDevice::Keyboard:
		default:
			g_gameConfig.Set(GameConfigKeys::Key_Sensitivity, sens);
			break;
		}
	}

	inline void UpdateInputKeyBindingStatus()
	{
		m_useBTGamepad = g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice) == InputDevice::Controller;
		m_useLaserGamepad = g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice) == InputDevice::Controller;

		if (m_useBTGamepad) m_activeBTKeys = &m_controllerKeys;
		else if (m_altBinds) m_activeBTKeys = &m_altKeyboardKeys;
		else m_activeBTKeys = &m_keyboardKeys;

		if (m_altBinds) m_activeLaserKeys = &m_altKeyboardLaserKeys;
		else m_activeLaserKeys = &m_keyboardLaserKeys;
	}
	inline void UpdateControllerInputNames()
	{
		for (size_t i = 0; i < 8; i++)
		{
			if (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice) == InputDevice::Controller)
			{
				m_controllerButtonNames[i] = Utility::Sprintf("%d", g_gameConfig.GetInt(m_controllerKeys[i]));
			}
			else
			{
				m_controllerButtonNames[i] = GetKeyNameFromScancodeConfig(g_gameConfig.GetInt((*m_activeBTKeys)[i]));
			}
		}
		for (size_t i = 0; i < 2; i++)
		{
			if (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice) == InputDevice::Controller)
			{
				m_controllerLaserNames[i] = Utility::Sprintf("%d", g_gameConfig.GetInt(m_controllerLaserKeys[i]));
			}
			else
			{
				m_controllerLaserNames[i] = Utility::ConvertToUTF8(Utility::WSprintf( // wstring->string because regular Sprintf messes up(?????)
					L"%ls / %ls",
					Utility::ConvertToWString(GetKeyNameFromScancodeConfig(g_gameConfig.GetInt((*m_activeLaserKeys)[i * 2]))),
					Utility::ConvertToWString(GetKeyNameFromScancodeConfig(g_gameConfig.GetInt((*m_activeLaserKeys)[i * 2 + 1])))
				));
			}
		}
	}
};

class SettingsPage_Game : public SettingsPage
{
public:
	SettingsPage_Game(nk_context* nctx) : SettingsPage(nctx, "Game") {}

protected:
	void Load() override
	{
		m_hitWindow = HitWindow::FromConfig();

		m_songsPath.Load();
	}

	void Save() override
	{
		m_hitWindow.Validate();
		m_hitWindow.SaveConfig();

		m_songsPath.Save();
	}

	HitWindow m_hitWindow = HitWindow::NORMAL;

	TextSettingData m_songsPath = { GameConfigKeys::SongFolder };

protected:
	void RenderContents() override
	{
		LayoutRowDynamic(1);

		EnumSetting<Enum_SpeedMods>(GameConfigKeys::SpeedMod, "Speed mod:");
		FloatSetting(GameConfigKeys::HiSpeed, "HiSpeed", 0.25f, 20, 0.05f);
		FloatSetting(GameConfigKeys::ModSpeed, "ModSpeed", 50, 1500, 0.5f);
		ToggleSetting(GameConfigKeys::AutoSaveSpeed, "Save hispeed changes during gameplay");

		IntSetting(GameConfigKeys::LeadInTime, "Lead-in time (ms)", 250, 10000, 250);
		IntSetting(GameConfigKeys::PracticeLeadInTime, "(for practice mode)", 250, 10000, 250);

		ToggleSetting(GameConfigKeys::PracticeSetupNavEnabled, "Enable navigation inputs for the practice setup");
		ToggleSetting(GameConfigKeys::RevertToSetupAfterScoreScreen, "Revert to the practice setup after the score screen is shown");

		ToggleSetting(GameConfigKeys::SkipScore, "Skip score screen on manual exit");
		EnumSetting<Enum_AutoScoreScreenshotSettings>(GameConfigKeys::AutoScoreScreenshot, "Automatically capture score screenshots:");

		{
			nk_label(m_nctx, "Timing Window:", nk_text_alignment::NK_TEXT_LEFT);
			LayoutRowDynamic(3);

			const int hitWindowPerfect = nk_propertyi_sdl_text(m_nctx, "Crit", 0, m_hitWindow.perfect, HitWindow::NORMAL.perfect, 1, 1);
			if (hitWindowPerfect != m_hitWindow.perfect)
			{
				m_hitWindow.perfect = hitWindowPerfect;
				if (m_hitWindow.good < m_hitWindow.perfect)
					m_hitWindow.good = m_hitWindow.perfect;
				if (m_hitWindow.hold < m_hitWindow.perfect)
					m_hitWindow.hold = m_hitWindow.perfect;
			}

			const int hitWindowGood = nk_propertyi_sdl_text(m_nctx, "Near", 0, m_hitWindow.good, HitWindow::NORMAL.good, 1, 1);
			if (hitWindowGood != m_hitWindow.good)
			{
				m_hitWindow.good = hitWindowGood;
				if (m_hitWindow.good < m_hitWindow.perfect)
					m_hitWindow.perfect = m_hitWindow.good;
				if (m_hitWindow.hold < m_hitWindow.good)
					m_hitWindow.hold = m_hitWindow.good;
			}

			const int hitWindowHold = nk_propertyi_sdl_text(m_nctx, "Hold", 0, m_hitWindow.hold, HitWindow::NORMAL.hold, 1, 1);
			if (hitWindowHold != m_hitWindow.hold)
			{
				m_hitWindow.hold = hitWindowHold;
				if (m_hitWindow.hold < m_hitWindow.perfect)
					m_hitWindow.perfect = m_hitWindow.hold;
				if (m_hitWindow.hold < m_hitWindow.good)
					m_hitWindow.good = m_hitWindow.hold;
			}

			LayoutRowDynamic(2);

			if (nk_button_label(m_nctx, "Set to NORMAL (default)"))
			{
				m_hitWindow = HitWindow::NORMAL;
			}

			if (nk_button_label(m_nctx, "Set to HARD"))
			{
				m_hitWindow = HitWindow::HARD;
			}

			LayoutRowDynamic(1);
		}

		nk_label(m_nctx, "Songs folder path:", nk_text_alignment::NK_TEXT_LEFT);
		m_songsPath.Render(m_nctx);

		ToggleSetting(GameConfigKeys::TransferScoresOnChartUpdate, "Transfer scores on chart change");

		ToggleSetting(GameConfigKeys::AutoComputeSongOffset, "Auto-compute the song offset on first play");
	}
};

class SettingsPage_Display : public SettingsPage
{
public:
	SettingsPage_Display(nk_context* nctx) : SettingsPage(nctx, "Display") {}

protected:
	void Load() override
	{
		m_skins = Path::GetSubDirs(Path::Normalize(Path::Absolute("skins/")));

		m_laserColors[0] = g_gameConfig.GetFloat(GameConfigKeys::Laser0Color);
		m_laserColors[1] = g_gameConfig.GetFloat(GameConfigKeys::Laser1Color);
	}

	void Save() override
	{
		g_gameConfig.Set(GameConfigKeys::Laser0Color, m_laserColors[0]);
		g_gameConfig.Set(GameConfigKeys::Laser1Color, m_laserColors[1]);
	}
	
	Vector<String> m_skins;
	std::array<float, 2> m_laserColors = { 200.0f, 330.0f };

	void RenderContents() override
	{
		LayoutRowDynamic(1);
		ToggleSetting(GameConfigKeys::EnableHiddenSudden, "Enable Hidden / Sudden Mode");

		LayoutRowDynamic(2, 75);

		if (nk_group_begin(m_nctx, "Hidden", NK_WINDOW_NO_SCROLLBAR))
		{
			LayoutRowDynamic(1);
			FloatSetting(GameConfigKeys::HiddenCutoff, "Hidden Cutoff", 0.0f, 1.0f);
			FloatSetting(GameConfigKeys::HiddenFade, "Hidden Fade", 0.0f, 1.0f);
			nk_group_end(m_nctx);
		}

		if (nk_group_begin(m_nctx, "Sudden", NK_WINDOW_NO_SCROLLBAR))
		{
			LayoutRowDynamic(1);
			FloatSetting(GameConfigKeys::SuddenCutoff, "Sudden Cutoff", 0.0f, 1.0f);
			FloatSetting(GameConfigKeys::SuddenFade, "Sudden Fade", 0.0f, 1.0f);
			nk_group_end(m_nctx);
		}

		LayoutRowDynamic(1);
		ToggleSetting(GameConfigKeys::DisableBackgrounds, "Disable Song Backgrounds");
		FloatSetting(GameConfigKeys::DistantButtonScale, "Distant Button Scale", 1.0f, 5.0f);
		ToggleSetting(GameConfigKeys::ShowCover, "Show Track Cover");

		if (m_skins.size() > 0)
		{
			if (StringSelectionSetting(GameConfigKeys::Skin, m_skins, "Selected Skin:")) {
				// Window cursor
				Image cursorImg = ImageRes::Create(Path::Absolute("skins/" + g_gameConfig.GetString(GameConfigKeys::Skin) + "/textures/cursor.png"));
				g_gameWindow->SetCursor(cursorImg, Vector2i(5, 5));
			}
		}

		EnumSetting<Enum_ScoreDisplayModes>(GameConfigKeys::ScoreDisplayMode, "In-game score display is:");

		RenderLaserColorSetting();

		LayoutRowDynamic(1);
		ToggleSetting(GameConfigKeys::DisplayPracticeInfoInGame, "Show practice info during gameplay");
	}

private:
	const std::array<float, 4> m_laserColorPalette = { 330.f, 60.f, 100.f, 200.f };
	bool m_laserColorPaletteVisible = false;

	void RenderLaserColorSetting()
	{
		const nk_color leftColor = nk_hsv_f(m_laserColors[0] / 360, 1, 1);
		const nk_color rightColor = nk_hsv_f(m_laserColors[1] / 360, 1, 1);

		const int lcolInt = static_cast<int>(m_laserColors[0]);
		const int rcolInt = static_cast<int>(m_laserColors[1]);

		LayoutRowDynamic(1);
		nk_label(m_nctx, "Laser colors:", nk_text_alignment::NK_TEXT_LEFT);

		LayoutRowDynamic(2);

		// Color
		if (nk_button_color(m_nctx, leftColor)) m_laserColorPaletteVisible = !m_laserColorPaletteVisible;
		if (nk_button_color(m_nctx, rightColor)) m_laserColorPaletteVisible = !m_laserColorPaletteVisible;

		// Palette
		if (m_laserColorPaletteVisible)
		{
			LayoutRowDynamic(2 * static_cast<int>(m_laserColorPalette.size()));

			RenderLaserColorPalette(m_laserColors.data());
			RenderLaserColorPalette(m_laserColors.data() + 1);

			LayoutRowDynamic(2);
		}

		// Text
		{
			const int lcolIntNew = nk_propertyi_sdl_text(m_nctx, "LLaser Hue", 0, lcolInt, 360, 1, 1);
			if (lcolIntNew != lcolInt) m_laserColors[0] = static_cast<float>(lcolIntNew);

			const int rcolIntNew = nk_propertyi_sdl_text(m_nctx, "RLaser Hue", 0, rcolInt, 360, 1, 1);
			if (rcolIntNew != rcolInt) m_laserColors[1] = static_cast<float>(rcolIntNew);
		}

		// Slider
		{
			nk_slider_float(m_nctx, 0, m_laserColors.data(), 360, 0.1f);
			nk_slider_float(m_nctx, 0, m_laserColors.data() + 1, 360, 0.1f);
		}
	}

	void RenderLaserColorPalette(float* laserColor)
	{
		for (const float paletteHue : m_laserColorPalette)
		{
			const nk_color paletteColor = nk_hsv_f(paletteHue / 360, 1, 1);
			if (nk_button_color(m_nctx, paletteColor))
			{
				*laserColor = paletteHue;
			}
		}
	}
};

class SettingsPage_System : public SettingsPage
{
public:
	SettingsPage_System(nk_context* nctx) : SettingsPage(nctx, "System") {}

protected:
	void Load() override
	{
		m_channels = { "release", "master", "develop" };
		String channel = g_gameConfig.GetString(GameConfigKeys::UpdateChannel);

		if (!m_channels.Contains(channel))
		{
			m_channels.insert(m_channels.begin(), channel);
		}
	}

	void Save() override
	{
	}

	const Vector<const char*> m_aaModes = { "Off", "2x MSAA", "4x MSAA", "8x MSAA", "16x MSAA" };
	Vector<String> m_channels;

protected:
	void RenderContents() override
	{
		LayoutRowDynamic(1);

		PercentSetting(GameConfigKeys::MasterVolume, "Master Volume (%.1f%%):");
		ToggleSetting(GameConfigKeys::WindowedFullscreen, "Use windowed fullscreen");
		ToggleSetting(GameConfigKeys::ForcePortrait, "Force portrait rendering (don't use if already in portrait)");
		ToggleSetting(GameConfigKeys::VSync, "VSync");
		ToggleSetting(GameConfigKeys::ShowFps, "Show FPS");

		SelectionSetting(GameConfigKeys::AntiAliasing, m_aaModes, "Anti-aliasing (requires restart):");

#ifdef _WIN32
		ToggleSetting(GameConfigKeys::WASAPI_Exclusive, "WASAPI Exclusive Mode (requires restart)");
#endif // _WIN32

		ToggleSetting(GameConfigKeys::MuteUnfocused, "Mute the game when unfocused");
		ToggleSetting(GameConfigKeys::PrerenderEffects, "Pre-Render Song Effects (experimental)");
		ToggleSetting(GameConfigKeys::CheckForUpdates, "Check for updates on startup");

		if (m_channels.size() > 0)
		{
			StringSelectionSetting(GameConfigKeys::UpdateChannel, m_channels, "Update Channel:");
		}

		EnumSetting<Logger::Enum_Severity>(GameConfigKeys::LogLevel, "Logging level");
	}
};

class SettingsPage_Online : public SettingsPage
{
public:
	SettingsPage_Online(nk_context* nctx) : SettingsPage(nctx, "Online") {}
	
protected:
	void Load() override
	{
		m_multiplayerHost.Load();
		m_multiplayerPassword.Load();
		m_multiplayerUsername.Load();

		m_irBaseURL.Load();
		m_irToken.Load();
	}

	void Save() override
	{
		m_multiplayerHost.Save();
		m_multiplayerPassword.Save();
		m_multiplayerUsername.Save();

		m_irBaseURL.Save();
		m_irToken.Save();
	}

	TextSettingData m_multiplayerHost = { GameConfigKeys::MultiplayerHost };
	TextSettingData m_multiplayerPassword = { GameConfigKeys::MultiplayerPassword };
	TextSettingData m_multiplayerUsername = { GameConfigKeys::MultiplayerUsername };

	TextSettingData m_irBaseURL = { GameConfigKeys::IRBaseURL };
	TextSettingData m_irToken = { GameConfigKeys::IRToken };

	void RenderContents() override
	{
		LayoutRowDynamic(1);

		nk_label(m_nctx, "Multiplayer Server:", nk_text_alignment::NK_TEXT_LEFT);
		m_multiplayerHost.Render(m_nctx);

		nk_label(m_nctx, "Multiplayer Server Username:", nk_text_alignment::NK_TEXT_LEFT);
		m_multiplayerUsername.Render(m_nctx);

		nk_label(m_nctx, "Multiplayer Server Password:", nk_text_alignment::NK_TEXT_LEFT);
		m_multiplayerPassword.RenderPassword(m_nctx);

		nk_label(m_nctx, "IR Base URL:", nk_text_alignment::NK_TEXT_LEFT);
		m_irBaseURL.Render(m_nctx);

		nk_label(m_nctx, "IR Token:", nk_text_alignment::NK_TEXT_LEFT);
		m_irToken.RenderPassword(m_nctx);

		ToggleSetting(GameConfigKeys::IRLowBandwidth, "IR Low Bandwidth (disables sending replays)");
	}
};

class SettingsPage_Skin : public SettingsPage
{
public:
	SettingsPage_Skin(nk_context* nctx) : SettingsPage(nctx, "Skin") {}

protected:
	void Load() override {}
	void Save() override {}

	void RenderContents() override
	{
		LayoutRowDynamic(1);
	}
};

class SettingsScreen_Impl_New : public BasicNuklearGui
{
public:
	bool Init() override
	{
		BasicNuklearGui::Init();
		InitPages();
		
		return true;
	}

	~SettingsScreen_Impl_New() override
	{
		for (auto& page : m_pages)
		{
			page->Exit();
		}

		g_application->ApplySettings();
	}

	void Tick(float deltaTime) override
	{
		if (m_needsProfileReboot)
		{
			RefreshProfile();
			return;
		}

		BasicNuklearGui::Tick(deltaTime);
	}

	void Render(float deltaTime) override
	{
		if (IsSuspended())
		{
			return;
		}

		RenderPages();

		BasicNuklearGui::NKRender();
	}

	void OnKeyPressed(SDL_Scancode code) override
	{
		if (IsSuspended())
		{
			return;
		}

		if (code == SDL_SCANCODE_ESCAPE)
		{
			Exit();
		}
	}

	void OnSuspend() override
	{
	}

	void OnRestore() override
	{
		g_application->DiscordPresenceMenu("Settings");
	}

	void Exit()
	{
		for (auto& page : m_pages)
		{
			page->Exit();
		}

		g_gameWindow->OnAnyEvent.RemoveAll(this);

		if (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice) == InputDevice::Mouse)
		{
			g_gameConfig.SetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice, InputDevice::Keyboard);
		}

		if (g_gameConfig.GetBool(GameConfigKeys::CheckForUpdates))
		{
			g_application->CheckForUpdate();
		}

		g_input.Cleanup();
		g_input.Init(*g_gameWindow);

		g_application->RemoveTickable(this);
	}

private:
	Vector<String> m_profiles;
	String m_currentProfile;
	bool m_needsProfileReboot = false;

	void InitProfile()
	{
		m_currentProfile = g_gameConfig.GetString(GameConfigKeys::CurrentProfileName);
		m_profiles.push_back("Main");

		Vector<FileInfo> profiles = Files::ScanFiles(Path::Absolute("profiles/"), "cfg", NULL);

		for (const auto& file : profiles)
		{
			String profileName = "";
			String unused = Path::RemoveLast(file.fullPath, &profileName);
			profileName = profileName.substr(0, profileName.length() - 4); // Remove .cfg
			m_profiles.push_back(profileName);
		}
	}

	void RefreshProfile()
	{
		String newProfile = g_gameConfig.GetString(GameConfigKeys::CurrentProfileName);

		// Save old settings
		g_gameConfig.Set(GameConfigKeys::CurrentProfileName, m_currentProfile);
		Exit();

		g_application->ApplySettings();

		// Load in new settings
		g_application->ReloadConfig(newProfile);

		g_application->AddTickable(SettingsScreen::Create());
	}

private:
	Vector<std::unique_ptr<SettingsPage>> m_pages;
	size_t m_currPage = 0;

	struct nk_rect m_pageHeaderRegion;
	struct nk_rect m_pageContentRegion;

	void InitPages()
	{
		m_pages.clear();

		m_pages.emplace_back(std::make_unique<SettingsPage_Input>(m_nctx));
		m_pages.emplace_back(std::make_unique<SettingsPage_Game>(m_nctx));
		m_pages.emplace_back(std::make_unique<SettingsPage_Display>(m_nctx));
		m_pages.emplace_back(std::make_unique<SettingsPage_System>(m_nctx));
		m_pages.emplace_back(std::make_unique<SettingsPage_Online>(m_nctx));
		m_pages.emplace_back(std::make_unique<SettingsPage_Skin>(m_nctx));

		for (const auto& page : m_pages)
		{
			page->Init();
		}

		m_currPage = 0;
	}

	inline void UpdatePageRegions()
	{
		const float SETTINGS_DESIRED_CONTENTS_WIDTH = g_resolution.y / 1.4f;
		const float SETTINGS_DESIRED_HEADERS_WIDTH = 120.0f;

		const float SETTINGS_WIDTH = Math::Min(SETTINGS_DESIRED_CONTENTS_WIDTH + SETTINGS_DESIRED_HEADERS_WIDTH, g_resolution.x - 5.0f);
		const float SETTINGS_CONTENTS_WIDTH = Math::Max(SETTINGS_WIDTH * 0.75f, SETTINGS_WIDTH - SETTINGS_DESIRED_HEADERS_WIDTH);
		const float SETTINGS_HEADERS_WIDTH = SETTINGS_WIDTH - SETTINGS_CONTENTS_WIDTH;

		// Better to keep current layout if there's not enough space
		if (SETTINGS_CONTENTS_WIDTH < 10.0f || SETTINGS_HEADERS_WIDTH < 10.0f)
		{
			return;
		}

		const float SETTINGS_OFFSET_X = (g_resolution.x - SETTINGS_WIDTH) / 2;
		const float SETTINGS_CONTENTS_OFFSET_X = SETTINGS_OFFSET_X + SETTINGS_HEADERS_WIDTH;

		m_pageHeaderRegion = { SETTINGS_OFFSET_X, 0, SETTINGS_HEADERS_WIDTH, (float)g_resolution.y };
		m_pageContentRegion = { SETTINGS_CONTENTS_OFFSET_X, 0, SETTINGS_CONTENTS_WIDTH, (float)g_resolution.y };
	}

	inline void RenderPages()
	{
		UpdatePageRegions();
		RenderPageHeaders();
		RenderPageContents();
	}

	inline void RenderPageHeaders()
	{
		if (nk_begin(m_nctx, "Pages", m_pageHeaderRegion, NK_WINDOW_NO_SCROLLBAR))
		{
			nk_layout_row_dynamic(m_nctx, 50, 1);
			
			for (size_t i = 0; i < m_pages.size(); ++i)
			{
				const auto& page = m_pages[i];
				const String& name = page->GetName();

				if (nk_button_text(m_nctx, name.c_str(), static_cast<int>(name.size())))
				{
					m_currPage = i;
				}
			}

			if (nk_button_label(m_nctx, "Exit")) Exit();
			nk_end(m_nctx);
		}
	}

	inline void RenderPageContents()
	{
		if (m_currPage >= m_pages.size())
		{
			return;
		}

		m_pages[m_currPage]->Render(m_pageContentRegion);
	}
};

IApplicationTickable* SettingsScreen::Create()
{
	return new SettingsScreen_Impl_New();
}

class ButtonBindingScreen_Impl : public ButtonBindingScreen
{
private:
	Ref<Gamepad> m_gamepad;
	//Label* m_prompt;
	GameConfigKeys m_key;
	bool m_isGamepad;
	int m_gamepadIndex;
	bool m_completed = false;
	bool m_knobs = false;
	bool m_isAlt = false;
	Vector<float> m_gamepadAxes;

public:
	ButtonBindingScreen_Impl(GameConfigKeys key, bool gamepad, int controllerIndex, bool isAlt)
	{
		m_key = key;
		m_gamepadIndex = controllerIndex;
		m_isGamepad = gamepad;
		m_knobs = (key == GameConfigKeys::Controller_Laser0Axis || key == GameConfigKeys::Controller_Laser1Axis);
		m_isAlt = isAlt;
	}

	bool Init()
	{
		if (m_isGamepad)
		{
			m_gamepad = g_gameWindow->OpenGamepad(m_gamepadIndex);
			if (!m_gamepad)
			{
				Logf("Failed to open gamepad: %d", Logger::Severity::Error, m_gamepadIndex);
				g_gameWindow->ShowMessageBox("Warning", "Could not open selected gamepad.\nEnsure the controller is connected and in the correct mode (if applicable) and selected in the previous menu.", 1);
				return false;
			}
			if (m_knobs)
			{
				for (size_t i = 0; i < m_gamepad->NumAxes(); i++)
				{
					m_gamepadAxes.Add(m_gamepad->GetAxis(i));
				}
			}
			else
			{
				m_gamepad->OnButtonPressed.Add(this, &ButtonBindingScreen_Impl::OnButtonPressed);
			}
		}
		return true;
	}

	void Tick(float deltatime)
	{
		if (m_knobs && m_isGamepad)
		{
			for (uint8 i = 0; i < m_gamepad->NumAxes(); i++)
			{
				float delta = fabs(m_gamepad->GetAxis(i) - m_gamepadAxes[i]);
				if (delta > 0.3f)
				{
					g_gameConfig.Set(m_key, i);
					m_completed = true;
					break;
				}

			}
		}

		if (m_completed && m_gamepad)
		{
			m_gamepad->OnButtonPressed.RemoveAll(this);
			m_gamepad.reset();

			g_application->RemoveTickable(this);
		}
		else if (m_completed && !m_knobs)
		{
			g_application->RemoveTickable(this);
		}

	}

	void Render(float deltatime)
	{
		String prompt = "Press Key";

		if (m_knobs)
		{
			prompt = "Press Left Key";
			if (m_completed)
			{
				prompt = "Press Right Key";
			}
		}

		if (m_isGamepad)
		{
			prompt = "Press Button";
			m_gamepad = g_gameWindow->OpenGamepad(m_gamepadIndex);
			if (m_knobs)
			{
				prompt = "Turn Knob";
				for (size_t i = 0; i < m_gamepad->NumAxes(); i++)
				{
					m_gamepadAxes.Add(m_gamepad->GetAxis(i));
				}
			}
		}

		g_application->FastText(prompt, g_resolution.x / 2, g_resolution.y / 2, 40, NVGalign::NVG_ALIGN_CENTER | NVGalign::NVG_ALIGN_MIDDLE);
	}

	void OnButtonPressed(uint8 key)
	{
		if (!m_knobs)
		{
			g_gameConfig.Set(m_key, key);
			m_completed = true;
		}
	}

	virtual void OnKeyPressed(SDL_Scancode code)
	{
		if (!m_isGamepad && !m_knobs)
		{
			g_gameConfig.Set(m_key, code);
			m_completed = true; // Needs to be set because pressing right alt triggers two keypresses on the same frame.
		}
		else if (!m_isGamepad && m_knobs)
		{
			switch (m_key)
			{
			case GameConfigKeys::Controller_Laser0Axis:
				g_gameConfig.Set(m_completed ?
					m_isAlt ? GameConfigKeys::Key_Laser0PosAlt : GameConfigKeys::Key_Laser0Pos :
					m_isAlt ? GameConfigKeys::Key_Laser0NegAlt : GameConfigKeys::Key_Laser0Neg,
					code);
				break;
			case GameConfigKeys::Controller_Laser1Axis:
				g_gameConfig.Set(m_completed ?
					m_isAlt ? GameConfigKeys::Key_Laser1PosAlt : GameConfigKeys::Key_Laser1Pos :
					m_isAlt ? GameConfigKeys::Key_Laser1NegAlt : GameConfigKeys::Key_Laser1Neg,
					code);
				break;
			default:
				break;
			}

			if (!m_completed)
			{
				m_completed = true;
			}
			else
			{
				g_application->RemoveTickable(this);
			}
		}
	}

	virtual void OnSuspend()
	{
		//g_rootCanvas->Remove(m_canvas.As<GUIElementBase>());
	}
	virtual void OnRestore()
	{
		//Canvas::Slot* slot = g_rootCanvas->Add(m_canvas.As<GUIElementBase>());
		//slot->anchor = Anchors::Full;
	}
};

ButtonBindingScreen* ButtonBindingScreen::Create(GameConfigKeys key, bool gamepad, int controllerIndex, bool isAlternative)
{
	ButtonBindingScreen_Impl* impl = new ButtonBindingScreen_Impl(key, gamepad, controllerIndex, isAlternative);
	return impl;
}

class LaserSensCalibrationScreen_Impl : public LaserSensCalibrationScreen
{
private:
	Ref<Gamepad> m_gamepad;
	//Label* m_prompt;
	bool m_state = false;
	float m_delta = 0.f;
	float m_currentSetting = 0.f;
	bool m_firstStart = false;
public:
	LaserSensCalibrationScreen_Impl()
	{

	}

	~LaserSensCalibrationScreen_Impl()
	{
		g_input.OnButtonPressed.RemoveAll(this);
	}

	bool Init()
	{
		g_input.GetInputLaserDir(0); //poll because there might be something idk

		if (g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice) == InputDevice::Controller)
			m_currentSetting = g_gameConfig.GetFloat(GameConfigKeys::Controller_Sensitivity);
		else
			m_currentSetting = g_gameConfig.GetFloat(GameConfigKeys::Mouse_Sensitivity);

		g_input.OnButtonPressed.Add(this, &LaserSensCalibrationScreen_Impl::OnButtonPressed);
		return true;
	}

	void Tick(float deltatime)
	{
		m_delta += g_input.GetAbsoluteInputLaserDir(0);

	}

	void Render(float deltatime)
	{
		if (m_state)
		{
			float sens = 6.0 / m_delta;

			g_application->FastText("Turn left knob one revolution clockwise", g_resolution.x / 2, g_resolution.y / 2, 40, NVGalign::NVG_ALIGN_CENTER | NVGalign::NVG_ALIGN_MIDDLE);
			g_application->FastText("then press start.", g_resolution.x / 2, g_resolution.y / 2 + 45, 40, NVGalign::NVG_ALIGN_CENTER | NVGalign::NVG_ALIGN_MIDDLE);
			g_application->FastText(Utility::Sprintf("Current Sens: %.2f", sens), g_resolution.x / 2, g_resolution.y / 2 + 90, 40, NVGalign::NVG_ALIGN_CENTER | NVGalign::NVG_ALIGN_MIDDLE);

		}
		else
		{
			m_delta = 0;
			g_application->FastText("Press start twice", g_resolution.x / 2, g_resolution.y / 2, 40, NVGalign::NVG_ALIGN_CENTER | NVGalign::NVG_ALIGN_MIDDLE);
		}
	}

	void OnButtonPressed(Input::Button button)
	{
		if (button == Input::Button::BT_S)
		{
			if (m_firstStart)
			{
				if (m_state)
				{
					// calc sens and then call delagate
					SensSet.Call(6.0 / m_delta);
					g_application->RemoveTickable(this);
				}
				else
				{
					m_delta = 0;
					m_state = !m_state;
				}
			}
			else
			{
				m_firstStart = true;
			}
		}
	}

	virtual void OnKeyPressed(SDL_Scancode code)
	{
		if (code == SDL_SCANCODE_ESCAPE)
			g_application->RemoveTickable(this);
	}

	virtual void OnSuspend()
	{
		//g_rootCanvas->Remove(m_canvas.As<GUIElementBase>());
	}
	virtual void OnRestore()
	{
		//Canvas::Slot* slot = g_rootCanvas->Add(m_canvas.As<GUIElementBase>());
		//slot->anchor = Anchors::Full;
	}
};

LaserSensCalibrationScreen* LaserSensCalibrationScreen::Create()
{
	LaserSensCalibrationScreen* impl = new LaserSensCalibrationScreen_Impl();
	return impl;
}


bool SkinSettingsScreen::Init()
{
	m_allSkins = Path::GetSubDirs(Path::Normalize(Path::Absolute("skins/")));
	return true;
}

bool SkinSettingsScreen::StringSelectionSetting(String key, String label, SkinSetting& setting)
{
	float w = Math::Min(g_resolution.y / 1.4, g_resolution.x - 5.0);

	String value = m_skinConfig->GetString(key);
	int selection;
	String* options = setting.selectionSetting.options;
	auto stringSearch = std::find(options, options + setting.selectionSetting.numOptions, value);
	if (stringSearch != options + setting.selectionSetting.numOptions)
		selection = (stringSearch - options);
	else
		selection = 0;
	auto prevSelection = selection;
	Vector<const char*> displayData;
	for (int i = 0; i < setting.selectionSetting.numOptions; i++)
	{
		displayData.Add(*setting.selectionSetting.options[i]);
	}

	nk_label(m_nctx, *label, nk_text_alignment::NK_TEXT_LEFT);
	nk_combobox(m_nctx, displayData.data(), setting.selectionSetting.numOptions, &selection, 30, nk_vec2(w - 30, 250));
	if (prevSelection != selection) {
		value = options[selection];
		m_skinConfig->Set(key, value);
		return true;
	}
	return false;
}

bool SkinSettingsScreen::MainConfigStringSelectionSetting(GameConfigKeys key, Vector<String> options, String label)
{
	String value = g_gameConfig.GetString(key);
	int selection;
	auto stringSearch = std::find(options.begin(), options.end(), value);
	if (stringSearch != options.end())
		selection = stringSearch - options.begin();
	else
		selection = 0;
	int prevSelection = selection;
	Vector<const char*> displayData;
	for (String& s : options)
	{
		displayData.Add(*s);
	}

	nk_label(m_nctx, *label, nk_text_alignment::NK_TEXT_LEFT);
	nk_combobox(m_nctx, displayData.data(), options.size(), &selection, 30, nk_vec2(1050, 250));
	if (prevSelection != selection) {
		value = options[selection];
		g_gameConfig.Set(key, value);
		return true;
	}
	return false;
}

void SkinSettingsScreen::Exit()
{
	g_application->RemoveTickable(this);
}

bool SkinSettingsScreen::IntSetting(String key, String label, int min, int max, int step, int perpixel)
{
	int value = m_skinConfig->GetInt(key);
	auto prevValue = value;
	value = nk_propertyi_sdl_text(m_nctx, *label, min, value, max, step, perpixel);
	if (prevValue != value) {
		m_skinConfig->Set(key, value);
		return true;
	}
	return false;
}

bool SkinSettingsScreen::FloatSetting(String key, String label, float min, float max, float step)
{
	float value = m_skinConfig->GetFloat(key);
	auto prevValue = value;

	nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_LEFT, *label, value);
	nk_slider_float(m_nctx, min, &value, max, step);
	if (prevValue != value) {
		m_skinConfig->Set(key, value);
		return true;
	}
	return false;
}

bool SkinSettingsScreen::PercentSetting(String key, String label)
{
	float value = m_skinConfig->GetFloat(key);
	auto prevValue = value;

	nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_LEFT, *label, value * 100);
	nk_slider_float(m_nctx, 0, &value, 1, 0.005);
	if (prevValue != value) {
		m_skinConfig->Set(key, value);
		return true;
	}
	return false;
}

bool SkinSettingsScreen::TextSetting(String key, String label, bool secret)
{
	String value = m_skinConfig->GetString(key);
	char display[1024];
	strcpy(display, value.c_str());
	int len = value.length();

	if (secret) //https://github.com/vurtun/nuklear/issues/587#issuecomment-354421477
	{
		char buf[1024];
		int old_len = len;
		for (int i = 0; i < len; i++)
			buf[i] = '*';

		nk_sdl_text(nk_edit_string(m_nctx, NK_EDIT_FIELD, buf, &len, 64, nk_filter_default));
		if (old_len < len)
		{
			memcpy(&display[old_len], &buf[old_len], (nk_size)(len - old_len));
		}
	}
	else
	{
		nk_label(m_nctx, *label, nk_text_alignment::NK_TEXT_LEFT);
		nk_sdl_text(nk_edit_string(m_nctx, NK_EDIT_FIELD, display, &len, 1024, nk_filter_default));
	}
	auto newValue = String(display, len);
	if (newValue != value) {
		m_skinConfig->Set(key, newValue);
		return true;
	}
	return false;
}

bool SkinSettingsScreen::ColorSetting(String key, String label)
{
	float w = Math::Min(g_resolution.y / 1.4, g_resolution.x - 5.0) - 100;
	Color value = m_skinConfig->GetColor(key);
	nk_label(m_nctx, *label, nk_text_alignment::NK_TEXT_LEFT);
	float r, g, b, a;
	r = value.x;
	g = value.y;
	b = value.z;
	a = value.w;
	nk_colorf nkCol = { r,g,b,a };
	if (nk_combo_begin_color(m_nctx, nk_rgb_cf(nkCol), nk_vec2(200, 400))) {
		enum color_mode { COL_RGB, COL_HSV };
		nk_layout_row_dynamic(m_nctx, 120, 1);
		nkCol = nk_color_picker(m_nctx, nkCol, NK_RGBA);

		nk_layout_row_dynamic(m_nctx, 25, 2);
		m_hsvMap[key] = nk_option_label(m_nctx, "RGB", m_hsvMap[key] ? 1 : 0) == 1;
		m_hsvMap[key] = nk_option_label(m_nctx, "HSV", m_hsvMap[key] ? 0 : 1) == 0;

		nk_layout_row_dynamic(m_nctx, 25, 1);
		if (!m_hsvMap[key]) {
			nkCol.r = nk_propertyf_sdl_text(m_nctx, "#R:", 0, nkCol.r, 1.0f, 0.01f, 0.005f);
			nkCol.g = nk_propertyf_sdl_text(m_nctx, "#G:", 0, nkCol.g, 1.0f, 0.01f, 0.005f);
			nkCol.b = nk_propertyf_sdl_text(m_nctx, "#B:", 0, nkCol.b, 1.0f, 0.01f, 0.005f);
			nkCol.a = nk_propertyf_sdl_text(m_nctx, "#A:", 0, nkCol.a, 1.0f, 0.01f, 0.005f);
		}
		else {
			float hsva[4];
			nk_colorf_hsva_fv(hsva, nkCol);
			hsva[0] = nk_propertyf_sdl_text(m_nctx, "#H:", 0, hsva[0], 1.0f, 0.01f, 0.05f);
			hsva[1] = nk_propertyf_sdl_text(m_nctx, "#S:", 0, hsva[1], 1.0f, 0.01f, 0.05f);
			hsva[2] = nk_propertyf_sdl_text(m_nctx, "#V:", 0, hsva[2], 1.0f, 0.01f, 0.05f);
			hsva[3] = nk_propertyf_sdl_text(m_nctx, "#A:", 0, hsva[3], 1.0f, 0.01f, 0.05f);
			nkCol = nk_hsva_colorfv(hsva);
		}
		nk_combo_end(m_nctx);
	}
	nk_layout_row_dynamic(m_nctx, 30, 1);

	Color newValue = Color(nkCol.r, nkCol.g, nkCol.b, nkCol.a);
	if (newValue != value) {
		m_skinConfig->Set(key, newValue);
		return true;
	}
	return false;
}

bool SkinSettingsScreen::ToggleSetting(String key, String label)
{
	int value = m_skinConfig->GetBool(key) ? 0 : 1;
	auto prevValue = value;
	nk_checkbox_label(m_nctx, *label, &value);
	if (prevValue != value) {
		m_skinConfig->Set(key, value == 0);
		return true;
	}
	return false;
}

SkinSettingsScreen::SkinSettingsScreen(String skin, nk_context* ctx)
{
	m_nctx = ctx;
	m_skin = skin;
	if (skin == g_application->GetCurrentSkin())
	{
		m_skinConfig = g_skinConfig;
	}
	else
	{
		m_skinConfig = new SkinConfig(skin);
	}
}

SkinSettingsScreen::~SkinSettingsScreen()
{
	if (m_skinConfig != g_skinConfig && m_skinConfig)
	{
		delete m_skinConfig;
		m_skinConfig = nullptr;
	}
}

void SkinSettingsScreen::Tick(float deltatime)
{

}

void SkinSettingsScreen::Render(float deltaTime)
{
	float w = Math::Min(g_resolution.y / 1.4, g_resolution.x - 5.0);
	float x = g_resolution.x / 2 - w / 2;
	if (nk_begin(m_nctx, *Utility::Sprintf("%s Settings", m_skin), nk_rect(x, 0, w, g_resolution.y), 0))
	{
		nk_layout_row_dynamic(m_nctx, 30, 1);

		if (m_allSkins.size() > 0)
		{
			if (MainConfigStringSelectionSetting(GameConfigKeys::Skin, m_allSkins, "Selected Skin:"))
			{
				// Window cursor
				Image cursorImg = ImageRes::Create(Path::Absolute("skins/" + g_gameConfig.GetString(GameConfigKeys::Skin) + "/textures/cursor.png"));
				g_gameWindow->SetCursor(cursorImg, Vector2i(5, 5));
				Exit();
			}
		}

		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, "%s Skin Settings", *m_skin);
		nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, "_______________________");
		for (auto s : m_skinConfig->GetSettings())
		{
			if (s.type == SkinSetting::Type::Boolean)
			{
				ToggleSetting(s.key, s.label);
			}
			else if (s.type == SkinSetting::Type::Selection)
			{
				StringSelectionSetting(s.key, s.label, s);
			}
			else if (s.type == SkinSetting::Type::Float)
			{
				FloatSetting(s.key, s.label + " (%.2f):", s.floatSetting.min, s.floatSetting.max);
			}
			else if (s.type == SkinSetting::Type::Integer)
			{
				IntSetting(s.key, s.label, s.intSetting.min, s.intSetting.max);
			}
			else if (s.type == SkinSetting::Type::Label)
			{
				nk_label(m_nctx, *s.key, nk_text_alignment::NK_TEXT_LEFT);
			}
			else if (s.type == SkinSetting::Type::Separator)
			{
				nk_labelf(m_nctx, nk_text_alignment::NK_TEXT_CENTERED, "_______________________");
			}
			else if (s.type == SkinSetting::Type::Text)
			{
				TextSetting(s.key, s.label, s.textSetting.secret);
			}
			else if (s.type == SkinSetting::Type::Color)
			{
				ColorSetting(s.key, s.label);
			}
		}
		if (nk_button_label(m_nctx, "Exit")) Exit();
		nk_end(m_nctx);
	}
	nk_sdl_render(NK_ANTI_ALIASING_ON, MAX_VERTEX_MEMORY, MAX_ELEMENT_MEMORY);
}
