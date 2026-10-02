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
		float hue1Offset = 0.5f;
	};

	Config g_cfg;
	float g_hue = 0.f;
	float g_syncTimer = 0.f;
	int g_lastPaletteId = -1;
	std::vector<WeakRef<SimplePlayer>> g_tracked;

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

	// Icon previews (profile page, icon kit): only touches what is switched on
	void paintSimplePlayer(SimplePlayer* sp, ccColor3B c1, ccColor3B c2) {
		if (g_cfg.color1) sp->setColor(c1);
		if (g_cfg.color2) sp->setSecondColor(c2);

		if (g_cfg.glow) {
			sp->enableCustomGlowColor(c2);
			sp->setGlowOutline(c2);

			bool robot = sp->m_robotSprite && sp->m_robotSprite->isVisible();
			bool spider = sp->m_spiderSprite && sp->m_spiderSprite->isVisible();

			if (robot) {
				sp->m_robotSprite->showGlow();
				sp->m_robotSprite->updateGlowColor(c2, false);
			} else if (spider) {
				sp->m_spiderSprite->showGlow();
				sp->m_spiderSprite->updateGlowColor(c2, false);
			} else if (sp->m_outlineSprite) {
				sp->m_outlineSprite->setVisible(true);
				sp->m_outlineSprite->setColor(c2);
			}
		}
	}

	void paintParticles(CCParticleSystem* p, ccColor3B c) {
		if (!p) return;
		auto s = p->getStartColor();
		s.r = c.r / 255.f;
		s.g = c.g / 255.f;
		s.b = c.b / 255.f;
		p->setStartColor(s);
		auto e = p->getEndColor();
		e.r = c.r / 255.f;
		e.g = c.g / 255.f;
		e.b = c.b / 255.f;
		p->setEndColor(e);
	}

	void paintSprite(CCSprite* s, ccColor3B c) {
		if (s) s->setColor(c);
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
		readConfig();

		float speed = static_cast<float>(mod->getSettingValue<double>("speed"));
		g_hue = std::fmod(g_hue + dt * speed, 1.f);
		ccColor3B c1 = hueToRGB(g_hue + g_cfg.hue1Offset);
		ccColor3B c2 = hueToRGB(g_hue);

		// Icon previews: profile page + icon kit
		std::erase_if(g_tracked, [](auto& w) { return !w.lock(); });
		if (g_cfg.previews) {
			for (auto& w : g_tracked) {
				if (auto sp = w.lock()) {
					paintSimplePlayer(sp, c1, c2);
				}
			}
		}

		// Optional: push nearest palette color into saved icon colors
		bool sync = mod->getSettingValue<bool>("palette-sync");
		bool hasOrig = mod->getSavedValue<bool>("has-orig", false);
		auto gm = GameManager::get();

		if (sync) {
			if (!hasOrig) {
				mod->setSavedValue("orig-color2", gm->getPlayerColor2());
				mod->setSavedValue("orig-glow-color", gm->getPlayerGlowColor());
				mod->setSavedValue("orig-glow", gm->getPlayerGlow());
				mod->setSavedValue("has-orig", true);
			}
			g_syncTimer += dt;
			if (g_syncTimer >= 0.1f) {
				g_syncTimer = 0.f;
				int id = nearestPaletteId(c2);
				if (id != g_lastPaletteId) {
					g_lastPaletteId = id;
					gm->setPlayerColor2(id);
					setGlowId(gm, id);
					gm->setPlayerGlow(true);
				}
			}
		} else if (hasOrig) {
			// setting was turned off: put the player's real colors back
			restoreOriginalColors();
		}
	}
}

// Heartbeat: runs every frame everywhere (menus, profile page, garage, levels)
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

// Icon kit: grab the big preview icon
class $modify(RainbowGarage, GJGarageLayer) {
	bool init() {
		if (!GJGarageLayer::init()) return false;
		trackPlayer(this->m_playerObject);
		return true;
	}
};

// In-level: recolor your own player objects, only the parts switched on
class $modify(RainbowPlayer, PlayerObject) {
	void update(float dt) {
		PlayerObject::update(dt);
		auto pl = PlayLayer::get();
		if (!pl) return;
		if (this != pl->m_player1 && this != pl->m_player2) return;

		ccColor3B c1 = hueToRGB(g_hue + g_cfg.hue1Offset);
		ccColor3B c2 = hueToRGB(g_hue);

		if (g_cfg.color1) this->setColor(c1);
		if (g_cfg.color2) this->setSecondColor(c2);

		if (g_cfg.glow) {
			// robot and spider have their own glow sprites, so never force their flags
			bool special = this->m_isRobot || this->m_isSpider;
			if (!special && !this->m_hasGlow) {
				this->m_hasGlow = true;
				this->updatePlayerGlow();
			}
			if (this->m_hasGlow) {
				this->enableCustomGlowColor(c2);
				if (!special) this->updateGlowColor();
			}
		}

		// Trails
		if (g_cfg.regularTrail && this->m_regularTrail) {
			this->m_regularTrail->tintWithColor(c2);
		}
		if (g_cfg.shipStreak && this->m_shipStreak) {
			this->m_shipStreak->tintWithColor(c2);
		}
		if (g_cfg.waveTrail && this->m_waveTrail) {
			this->m_waveTrail->setColor(c2);
		}
		if (g_cfg.ghostTrail && this->m_ghostTrail) {
			this->m_ghostTrail->m_color = c2;
		}

		// Fire and particles
		if (g_cfg.dashFire) {
			paintSprite(this->m_dashFireSprite, c2);
			paintParticles(this->m_dashParticles, c2);
		}
		if (g_cfg.shipFire) {
			paintParticles(this->m_trailingParticles, c2);
			paintParticles(this->m_shipClickParticles, c2);
			paintParticles(this->m_ufoClickParticles, c2);
			paintParticles(this->m_vehicleGroundParticles, c2);
		}
		if (g_cfg.swingFire) {
			paintSprite(this->m_swingFireTop, c2);
			paintSprite(this->m_swingFireMiddle, c2);
			paintSprite(this->m_swingFireBottom, c2);
			paintSprite(this->m_robotFire, c2);
			paintParticles(this->m_robotBurstParticles, c2);
			paintParticles(this->m_swingBurstParticles1, c2);
			paintParticles(this->m_swingBurstParticles2, c2);
		}
		if (g_cfg.groundParticles) {
			paintParticles(this->m_playerGroundParticles, c2);
			paintParticles(this->m_landParticles0, c2);
			paintParticles(this->m_landParticles1, c2);
		}
	}
};

// If a previous session ended with palette-sync on, put colors back on load
$on_mod(Loaded) {
	auto mod = Mod::get();
	if (!mod->getSettingValue<bool>("palette-sync") && mod->getSavedValue<bool>("has-orig", false)) {
		restoreOriginalColors();
	}
}
