#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/ProfilePage.hpp>
#include <cmath>
#include <climits>

using namespace geode::prelude;

namespace {
	float g_hue = 0.f;             // 0..1
	float g_syncTimer = 0.f;
	int g_lastPaletteId = -1;
	std::vector<WeakRef<SimplePlayer>> g_tracked;

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
			if (d < bestDist) { bestDist = d; best = i; }
		}
		return best;
	}

	void collectPlayers(CCNode* node) {
		if (!node) return;
		if (auto sp = typeinfo_cast<SimplePlayer*>(node)) {
			g_tracked.push_back(WeakRef<SimplePlayer>(sp));
		}
		if (auto kids = node->getChildren()) {
			for (auto kid : CCArrayExt<CCNode*>(kids)) {
				collectPlayers(kid);
			}
		}
	}

	void restorePaletteColors() {
		auto mod = Mod::get();
		if (!mod->hasSavedValue("orig-color2")) return;
		auto gm = GameManager::get();
		gm->setPlayerColor2(mod->getSavedValue<int>("orig-color2"));
		gm->setPlayerGlowColor(mod->getSavedValue<int>("orig-glow-color"));
		gm->setPlayerGlow(mod->getSavedValue<bool>("orig-glow"));
		mod->setSavedValue("orig-color2", 0);
		// clear marker by saving a sentinel
		mod->setSavedValue("has-orig", false);
		g_lastPaletteId = -1;
	}

	void tick(float dt) {
		auto mod = Mod::get();
		float speed = static_cast<float>(mod->getSettingValue<double>("speed"));
		g_hue = std::fmod(g_hue + dt * speed, 1.f);
		ccColor3B col = hueToRGB(g_hue);

		// Profile page icons (your own account only)
		std::erase_if(g_tracked, [](auto& w) { return !w.lock(); });
		for (auto& w : g_tracked) {
			if (auto sp = w.lock()) {
				sp->setSecondColor(col);
				sp->setGlowOutline(col);
			}
		}

		// Optional: push nearest palette color into saved icon colors
		bool sync = mod->getSettingValue<bool>("palette-sync");
		auto gm = GameManager::get();
		if (sync) {
			if (!mod->getSavedValue<bool>("has-orig", false)) {
				mod->setSavedValue("orig-color2", gm->getPlayerColor2());
				mod->setSavedValue("orig-glow-color", gm->getPlayerGlowColor());
				mod->setSavedValue("orig-glow", gm->getPlayerGlow());
				mod->setSavedValue("has-orig", true);
			}
			g_syncTimer += dt;
			if (g_syncTimer >= 0.1f) {
				g_syncTimer = 0.f;
				int id = nearestPaletteId(col);
				if (id != g_lastPaletteId) {
					g_lastPaletteId = id;
					gm->setPlayerColor2(id);
					gm->setPlayerGlowColor(id);
					gm->setPlayerGlow(true);
				}
			}
		} else if (mod->getSavedValue<bool>("has-orig", false)) {
			// setting was turned off: put the player's real colors back
			auto gm2 = GameManager::get();
			gm2->setPlayerColor2(mod->getSavedValue<int>("orig-color2"));
			gm2->setPlayerGlowColor(mod->getSavedValue<int>("orig-glow-color"));
			gm2->setPlayerGlow(mod->getSavedValue<bool>("orig-glow"));
			mod->setSavedValue("has-orig", false);
			g_lastPaletteId = -1;
		}
	}
}

// Heartbeat: runs every frame everywhere (menus, profile page, levels)
class $modify(RainbowScheduler, CCScheduler) {
	void update(float dt) {
		CCScheduler::update(dt);
		tick(dt);
	}
};

// Account page: grab the icons when your own profile loads
class $modify(RainbowProfile, ProfilePage) {
	void loadPageFromUserInfo(GJUserScore* score) {
		ProfilePage::loadPageFromUserInfo(score);
		if (score && score->m_accountID == GJAccountManager::get()->m_accountID) {
			collectPlayers(this);
		}
	}
};

// In-level: recolor your own player objects every frame
class $modify(RainbowPlayer, PlayerObject) {
	void update(float dt) {
		PlayerObject::update(dt);
		auto pl = PlayLayer::get();
		if (!pl) return;
		if (this != pl->m_player1 && this != pl->m_player2) return;

		ccColor3B col = hueToRGB(g_hue);
		this->setSecondColor(col);
		this->m_hasGlow = true;
		this->enableCustomGlowColor(col);
	}
};

// If a previous session crashed with palette-sync on, put colors back on load
$on_mod(Loaded) {
	if (!Mod::get()->getSettingValue<bool>("palette-sync") &&
		Mod::get()->getSavedValue<bool>("has-orig", false)) {
		restorePaletteColors();
	}
}
