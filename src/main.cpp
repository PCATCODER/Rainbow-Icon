#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/ProfilePage.hpp>
#include <Geode/modify/GJGarageLayer.hpp>
#include <algorithm>
#include <climits>
#include <cmath>

using namespace geode::prelude;

namespace {
	float g_hue = 0.f;
	float g_syncTimer = 0.f;
	int g_lastPaletteId = -1;
	std::vector<WeakRef<SimplePlayer>> g_tracked;

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

	void trackPlayer(SimplePlayer* sp) {
		if (sp) g_tracked.push_back(WeakRef<SimplePlayer>(sp));
	}

	void collectPlayers(CCNode* node) {
		if (!node) return;
		if (auto sp = typeinfo_cast<SimplePlayer*>(node)) {
			trackPlayer(sp);
		}
		if (auto kids = node->getChildren()) {
			for (auto kid : CCArrayExt<CCNode*>(kids)) {
				collectPlayers(kid);
			}
		}
	}

	// Rainbow color 2 + forced glow on an icon preview (profile page, icon kit)
	void paintSimplePlayer(SimplePlayer* sp, ccColor3B col) {
		sp->setSecondColor(col);
		sp->enableCustomGlowColor(col);
		sp->setGlowOutline(col);

		bool robot = sp->m_robotSprite && sp->m_robotSprite->isVisible();
		bool spider = sp->m_spiderSprite && sp->m_spiderSprite->isVisible();

		if (robot) {
			sp->m_robotSprite->showGlow();
			sp->m_robotSprite->updateGlowColor(col, false);
		} else if (spider) {
			sp->m_spiderSprite->showGlow();
			sp->m_spiderSprite->updateGlowColor(col, false);
		} else if (sp->m_outlineSprite) {
			sp->m_outlineSprite->setVisible(true);
			sp->m_outlineSprite->setColor(col);
		}
	}

	void restoreOriginalColors() {
		auto mod = Mod::get();
		auto gm = GameManager::get();
		gm->setPlayerColor2(mod->getSavedValue<int>("orig-color2"));
		setGlowId(gm, mod->getSavedValue<int>("orig-glow-color"));
		gm->setPlayerGlow(mod->getSavedValue<bool>("orig-glow"));
		mod->setSavedValue("has-orig", false);
		g_lastPaletteId = -1;
	}

	void tick(float dt) {
		auto mod = Mod::get();
		float speed = static_cast<float>(mod->getSettingValue<double>("speed"));
		g_hue = std::fmod(g_hue + dt * speed, 1.f);
		ccColor3B col = hueToRGB(g_hue);

		// Icon previews: profile page + icon kit
		std::erase_if(g_tracked, [](auto& w) { return !w.lock(); });
		for (auto& w : g_tracked) {
			if (auto sp = w.lock())
