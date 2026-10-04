#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/CharacterColorPage.hpp>
#include <Geode/modify/GameLevelManager.hpp>
#include <algorithm>
#include <climits>
#include <cmath>

using namespace geode::prelude;

namespace {
	struct Config {
		bool show = true;
		int rainbowId = 111;
		bool uploadSlot = false;
		float hue1Offset = 0.5f;
		bool regularTrail = true;
		bool waveTrail = true;
		bool shipStreak = true;
		bool ghostTrail = true;
		bool dashFire = true;
		bool shipFire = true;
		bool swingFire = true;
		bool groundParticles = true;
		bool progressBar = true;
	};

	Config g_cfg;
	float g_hue = 0.f;
	bool g_own1 = false;
	bool g_own2 = false;
	bool g_ownGlow = false;

	// Stand-in color the game gets when it asks for the Rainbow slot.
	// The heartbeat finds icons wearing it and animates them.
	constexpr ccColor3B RAINBOW_MARK = {255, 1, 3};
	struct RainbowSlots {
		bool first = false;
		bool second = false;
		bool glow = false;
	};
	std::vector<std::pair<WeakRef<SimplePlayer>, RainbowSlots>> g_rainbowPlayers;
	WeakRef<CCSprite> g_swatch;
	bool g_scanPending = false;
	int g_scanDelay = 0;

	void readConfig() {
		auto m = Mod::get();
		g_cfg.show = m->getSettingValue<bool>("show-rainbow-palette");
		g_cfg.rainbowId = static_cast<int>(m->getSettingValue<int64_t>("rainbow-id"));
		g_cfg.uploadSlot = m->getSettingValue<bool>("upload-slot");
		g_cfg.hue1Offset = static_cast<float>(m->getSettingValue<double>("color-1-offset"));
		g_cfg.regularTrail = m->getSettingValue<bool>("regular-trail");
		g_cfg.waveTrail = m->getSettingValue<bool>("wave-trail");
		g_cfg.shipStreak = m->getSettingValue<bool>("ship-streak");
		g_cfg.ghostTrail = m->getSettingValue<bool>("ghost-trail");
		g_cfg.dashFire = m->getSettingValue<bool>("dash-fire");
		g_cfg.shipFire = m->getSettingValue<bool>("ship-fire");
		g_cfg.swingFire = m->getSettingValue<bool>("swing-fire");
		g_cfg.groundParticles = m->getSettingValue<bool>("ground-particles");
		g_cfg.progressBar = m->getSettingValue<bool>("progress-bar");
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

	// Closest real palette color to a color (skips unknown slots past the palette end)
	int nearestPaletteId(ccColor3B c) {
		auto gm = GameManager::get();
		ccColor3B unknown = gm->colorForIdx(99999);
		int best = 0;
		int bestDist = INT_MAX;
		for (int i = 0; i < 111; i++) {
			auto p = gm->colorForIdx(i);
			if (i >= 100 && p.r == unknown.r && p.g == unknown.g && p.b == unknown.b) continue;
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

	bool isMark(ccColor3B c) {
		return c.r == RAINBOW_MARK.r && c.g == RAINBOW_MARK.g && c.b == RAINBOW_MARK.b;
	}

	// Finds every icon (icon kit, profile page, comments, ...) wearing the marker color
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
						e.second.second = e.second.second || slots.second;
						e.second.glow = e.second.glow || slots.glow;
						found = true;
						break;
					}
				}
				if (!found) g_rainbowPlayers.push_back({WeakRef<SimplePlayer>(sp), slots});
			}
		}
		if (auto kids = node->getChildren()) {
			for (auto kid : CCArrayExt<CCNode*>(kids)) {
				scanForMarked(kid);
			}
		}
	}

	// Rainbow on an icon preview: first/second color and the glow outline
	void paintIcon(SimplePlayer* sp, bool first, bool second, bool glow, ccColor3B c1, ccColor3B c2) {
		if (first) sp->setColor(c1);
		if (second) sp->setSecondColor(c2);
		if (!glow) return;

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

	// Gentle trail tint: only sets the trail's color for NEW points. It never touches the
	// trail's shape, texture or point buffers, so your selected trail design stays as is
	// and it will not fight with other trail mods.
	void tintStreak(CCMotionStreak* s, ccColor3B c) {
		if (s) s->setColor(c);
	}

	void tick(float dt) {
		auto mod = Mod::get();
		readConfig();

		float speed = static_cast<float>(mod->getSettingValue<double>("speed"));
		g_hue = std::fmod(g_hue + dt * speed, 1.f);
		ccColor3B c1 = hueToRGB(g_hue + g_cfg.hue1Offset);
		ccColor3B c2 = hueToRGB(g_hue);

		// Is my own icon wearing the Rainbow slot? (my saved colors are never changed)
		auto gm = GameManager::get();
		int rid = g_cfg.rainbowId;
		g_own1 = g_cfg.show && gm->getPlayerColor() == rid;
		g_own2 = g_cfg.show && gm->getPlayerColor2() == rid;
		g_ownGlow = g_cfg.show && gm->getPlayerGlow() && gm->getPlayerGlowColor() == rid;

		// Level progress bar
		if (g_cfg.progressBar && (g_own1 || g_own2 || g_ownGlow)) {
			if (auto pl = PlayLayer::get()) {
				if (pl->m_progressFill) pl->m_progressFill->setColor(c2);
			}
		}

		// The Rainbow swatch in the icon kit color grid
		if (auto s = g_swatch.lock()) s->setColor(c2);

		// Everyone's icons (with this mod) wearing the Rainbow slot
		if (g_scanPending) {
			g_scanPending = false;
			g_scanDelay = 3;
		}
		if (g_scanDelay > 0 && --g_scanDelay == 0) {
			if (auto scene = CCDirector::get()->getRunningScene()) scanForMarked(scene);
		}
		std::erase_if(g_rainbowPlayers, [](auto& e) { return !e.first.lock(); });
		for (auto& e : g_rainbowPlayers) {
			if (auto sp = e.first.lock()) {
				paintIcon(sp, e.second.first, e.second.second, e.second.glow, c1, c2);
			}
		}
	}
}

// Heartbeat: runs every frame everywhere (menus, icon kit, profile page, levels)
class $modify(RainbowScheduler, CCScheduler) {
	void update(float dt) {
		CCScheduler::update(dt);
		tick(dt);
	}
};

// The Rainbow slot: hand back the marker color, and count it as unlocked
class $modify(RainbowPalette, GameManager) {
	ccColor3B colorForIdx(int index) {
		if (g_cfg.show && index == g_cfg.rainbowId) {
			g_scanPending = true;
			return RAINBOW_MARK;
		}
		return GameManager::colorForIdx(index);
	}

	bool isColorUnlocked(int id, UnlockType type) {
		if (g_cfg.show && id == g_cfg.rainbowId) return true;
		return GameManager::isColorUnlocked(id, type);
	}
};

// Icon kit color grid: add a clickable Rainbow swatch after the last color
class $modify(RainbowColorPage, CharacterColorPage) {
	void createColorMenu() {
		CharacterColorPage::createColorMenu();
		if (!g_cfg.show || !m_colorButtons) return;

		auto first = typeinfo_cast<CCMenuItemSpriteExtra*>(m_colorButtons->objectForKey(0));
		if (!first) return;
		auto menu = typeinfo_cast<CCMenu*>(first->getParent());
		auto ref = typeinfo_cast<CCSprite*>(first->getNormalImage());
		if (!menu || !ref) return;

		CCSprite* spr = nullptr;
		if (typeinfo_cast<ColorChannelSprite*>(ref)) {
			spr = ColorChannelSprite::create();
		} else {
			spr = CCSprite::createWithSpriteFrame(ref->displayFrame());
		}
		if (!spr) return;
		spr->setScale(ref->getScale());
		spr->setColor(hueToRGB(g_hue));

		auto btn = CCMenuItemSpriteExtra::create(spr, nullptr, this, menu_selector(CharacterColorPage::onPlayerColor));
		btn->setTag(g_cfg.rainbowId);
		btn->setPosition(this->offsetForIndex(static_cast<int>(m_colorButtons->count())));
		menu->addChild(btn);
		m_colorButtons->setObject(btn, g_cfg.rainbowId);
		g_swatch = spr;
	}

	int colorForIndex(int index) {
		if (g_cfg.show && index == g_cfg.rainbowId) return index;
		return CharacterColorPage::colorForIndex(index);
	}
};

// Profile upload: other players see the NEAREST REAL PALETTE COLOR to the current rainbow
// (a normal color everyone's game knows). Your real saved colors are put straight back.
class $modify(RainbowUpload, GameLevelManager) {
	void updateUserScore() {
		if (!g_cfg.show || g_cfg.uploadSlot || (!g_own1 && !g_own2 && !g_ownGlow)) {
			GameLevelManager::updateUserScore();
			return;
		}
		auto gm = GameManager::get();
		int o1 = gm->getPlayerColor();
		int o2 = gm->getPlayerColor2();
		int og = gm->getPlayerGlowColor();
		ccColor3B c1 = hueToRGB(g_hue + g_cfg.hue1Offset);
		ccColor3B c2 = hueToRGB(g_hue);

		if (g_own1) gm->setPlayerColor(nearestPaletteId(c1));
		if (g_own2) gm->setPlayerColor2(nearestPaletteId(c2));
		if (g_ownGlow) setGlowId(gm, nearestPaletteId(c2));

		GameLevelManager::updateUserScore();

		gm->setPlayerColor(o1);
		gm->setPlayerColor2(o2);
		setGlowId(gm, og);
	}
};

// In a level: animate your own icon, trails and fire
class $modify(RainbowPlayer, PlayerObject) {
	void update(float dt) {
		PlayerObject::update(dt);
		if (!g_own1 && !g_own2 && !g_ownGlow) return;
		auto pl = PlayLayer::get();
		if (!pl) return;
		if (this != pl->m_player1 && this != pl->m_player2) return;

		ccColor3B c1 = hueToRGB(g_hue + g_cfg.hue1Offset);
		ccColor3B c2 = hueToRGB(g_hue);

		if (g_own1) this->setColor(c1);
		if (g_own2) this->setSecondColor(c2);

		if (g_ownGlow) {
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
		if (g_cfg.regularTrail) tintStreak(this->m_regularTrail, c2);
		if (g_cfg.shipStreak) tintStreak(this->m_shipStreak, c2);
		if (g_cfg.waveTrail && this->m_waveTrail) this->m_waveTrail->setColor(c2);
		if (g_cfg.ghostTrail && this->m_ghostTrail) this->m_ghostTrail->m_color = c2;

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
