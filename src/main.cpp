#include <Geode/Geode.hpp>
#include <Geode/modify/GameLevelManager.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/ProfilePage.hpp>
#include <Geode/modify/GJGarageLayer.hpp>
#include <algorithm>
#include <climits>
#include <cmath>

using namespace geode::prelude;

namespace {
	struct Config {
		bool color1 = false;
		bool color2 = true;
		bool glow = true;
		bool previews = true;
		bool regularTrail = true;
		bool waveTrail = true;
		bool shipStreak = true;
		bool dashFire = true;
		bool shipFire = true;
		bool swingFire = true;
		bool groundParticles = true;
		bool ghostTrail = true;
		bool progressBar = true;
		bool showRainbowPalette = true;
		int rainbowId = 106;
		float hue1Offset = 0.5f;
	};

	Config g_cfg;
	float g_hue = 0.f;
	float g_syncTimer = 0.f;
	int g_lastPaletteId = -1;
	std::vector<WeakRef<SimplePlayer>> g_tracked;

	// "Rainbow palette color": a stand-in color that marks icons to be drawn as rainbow
	constexpr ccColor3B RAINBOW_MARK = {255, 1, 3};
	struct RainbowSlots {
		bool first = false;
		bool second = false;
		bool glow = false;
	};
	std::vector<std::pair<WeakRef<SimplePlayer>, RainbowSlots>> g_rainbowPlayers;
	bool g_scanPending = false;
	int g_scanDelay = 0;
	bool g_useRainbowId = false;

	void readConfig() {
		auto m = Mod::get();
		g_cfg.color1 = m->getSettingValue<bool>("color-1");
		g_cfg.color2 = m->getSettingValue<bool>("color-2");
		g_cfg.glow = m->getSettingValue<bool>("glow");
		g_cfg.previews = m->getSettingValue<bool>("previews");
		g_cfg.regularTrail = m->getSettingValue<bool>("regular-trail");
		g_cfg.waveTrail = m->getSettingValue<bool>("wave-trail");
		g_cfg.shipStreak = m->getSettingValue<bool>("ship-streak");
		g_cfg.dashFire = m->getSettingValue<bool>("dash-fire");
		g_cfg.shipFire = m->getSettingValue<bool>("ship-fire");
		g_cfg.swingFire = m->getSettingValue<bool>("swing-fire");
		g_cfg.groundParticles = m->getSettingValue<bool>("ground-particles");
		g_cfg.ghostTrail = m->getSettingValue<bool>("ghost-trail");
		g_cfg.progressBar = m->getSettingValue<bool>("progress-bar");
		g_cfg.showRainbowPalette = m->getSettingValue<bool>("show-rainbow-palette");
		g_cfg.rainbowId = static_cast<int>(m->getSettingValue<int64_t>("rainbow-id"));
		g_cfg.hue1Offset = static_cast<float>(m->getSettingValue<double>("color-1-offset"));
	}

	// Sets the saved glow color id, whichever way this Geode version allows
	template <class GM>
	void setGlowId(GM* gm, int id) {
		if constexpr (requires { gm->m_playerGlowColor; }) {
			gm->m_playerGlowColor = id;
		} else if constexpr (requires { gm->setPlayerColor3(id); }) {
			gm->setPlayerColor3(id);
		}
	}

	ccColor3B hueToRGB(float h) {
		h = std::fmod(h, 1.f);
		if (h < 0.f) h += 1.f;
		float r = std::fabs(h * 6.f - 3.f) - 1.f;
		float g = 2.f - std::fabs(h * 6.f - 2.f);
		float b = 2.f - std::fabs(h * 6.f - 4.f);
		auto cl = [](float v) { return std::clamp(v, 0.f, 1.f); };
		return ccColor3B{
			static_cast<GLubyte>(cl(r) * 255.f),
			static_cast<GLubyte>(cl(g) * 255.f),
			static_cast<GLubyte>(cl(b) * 255.f)
		};
	}

	int nearestPaletteId(ccColor3B c) {
		auto gm = GameManager::get();
		int best = 0;
		int bestDist = INT_MAX;
		for (int i = 0; i < 107; i++) {
			if (i == g_cfg.rainbowId) continue;
			auto p = gm->colorForIdx(i);
			int dr = int(p.r) - int(c.r);
			int dg = int(p.g) - int(c.g);
			int db = int(p.b) - int(c.b);
			int d = dr * dr + dg * dg + db * db;
			if (d < bestDist) {
				bestDist = d;
				best = i;
			}
		}
		return best;
	}

	// Which palette number gets saved on the profile upload
	int pickId(ccColor3B c) {
		return g_useRainbowId ? g_cfg.rainbowId : nearestPaletteId(c);
	}

	bool isMark(ccColor3B c) {
		return c.r == RAINBOW_MARK.r && c.g == RAINBOW_MARK.g && c.b == RAINBOW_MARK.b;
	}

	void scanForMarked(CCNode* node) {
		if (!node) return;
		if (auto sp = typeinfo_cast<SimplePlayer*>(node)) {
			RainbowSlots slots;
			slots.first = sp->m_firstLayer && isMark(sp->m_firstLayer->getColor());
			slots.second = sp->m_secondLayer && isMark(sp->m_secondLayer->getColor());
			slots.glow = sp->m_outlineSprite && isMark(sp->m_outlineSprite->getColor());
			if (slots.first || slots.second || slots.glow) {
				bool found = false;
				for (auto& e : g_rainbowPlayers) {
					if (e.first.lock().data() == sp) {
						e.second.first = e.second.first || slots.first;
						e.second.second =
