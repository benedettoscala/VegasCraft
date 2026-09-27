package dev.vegascraft.client.pip;

import dev.vegascraft.pip.PipData;
import java.util.List;
import net.minecraft.client.gui.GuiGraphicsExtractor;

/** The Pip-Boy's STATS menu: Status, S.P.E.C.I.A.L., Skills, Perks and General. */
public final class StatsScreen extends PipScreen {
	private static final List<String> TABS = List.of("Status", "S.P.E.C.I.A.L.", "Skills", "Perks", "General");
	private static final String[] LIMBS = { "Head", "Torso", "Left Arm", "Right Arm", "Left Leg", "Right Leg", "Brain" };
	private static final String[] MISC = { "Quests Completed", "Locations Discovered", "People Killed", "Creatures Killed", "Locks Picked", "Computers Hacked",
		"Stimpaks Taken", "Rad-X Taken", "RadAway Taken", "Chems Taken", "Times Addicted", "Mines Disarmed", "Speech Successes", "Pockets Picked",
		"Pants Exploded", "Books Read", "Health Recovered From Stimpaks", "Weapons Created", "Health Recovered From Food", "Water Consumed",
		"Sandman Kills", "Paralyzing Punches", "Robots Disabled", "Times Slept", "Corpses Eaten", "Mysterious Stranger Visits", "Doctor Bags Used",
		"Challenges Completed", "Miss Fortune Occurrences", "Disintegrations", "Have Limbs Crippled", "Speech Failures", "Items Crafted",
		"Weapon Modifications", "Items Repaired", "Total Things Killed", "Dismembered Limbs", "Caravan Games Won", "Caravan Games Lost",
		"Barter Amount Traded", "Roulette Games Played", "Blackjack Games Played", "Slots Games Played" };
	private static int remembered;
	private int selected;

	public StatsScreen() {
		super("STATS");
		this.tab = remembered;
	}

	@Override
	protected List<String> tabs() {
		return TABS;
	}

	@Override
	protected void tabChanged() {
		remembered = this.tab;
		this.selected = 0;
	}

	@Override
	protected String header() {
		PipData.Stats s = PipData.stats();
		if (s == null) {
			return "";
		}
		return "LVL " + s.level() + "   HP " + Math.round(s.hp()) + "/" + Math.round(s.hpMax()) + "   AP " + Math.round(s.ap()) + "/" + Math.round(s.apMax())
			+ "   XP " + s.xp() + "/" + s.xpNext();
	}

	@Override
	protected String headerShort() {
		PipData.Stats s = PipData.stats();
		return s == null ? "" : "LVL " + s.level() + "  HP " + Math.round(s.hp()) + "/" + Math.round(s.hpMax()) + "  XP " + s.xp();
	}

	@Override
	protected void body(GuiGraphicsExtractor g, int x, int y, int w, int h, int mx, int my) {
		PipData.Stats s = PipData.stats();
		if (s == null) {
			text(g, "Waiting for New Vegas...", x + 4, y + 6, DIM);
			return;
		}
		switch (this.tab) {
			case 0 -> status(g, s, x, y, w, h);
			case 1 -> statList(g, s.special(), x, y, w, h, 12, false);
			case 2 -> statList(g, s.skills(), x, y, w, h, 12, true);
			case 3 -> perks(g, s, x, y, w, h);
			default -> general(g, s, x, y, w, h);
		}
	}

	private void status(GuiGraphicsExtractor g, PipData.Stats s, int x, int y, int w, int h) {
		int half = w / 2;
		text(g, "CONDITION", x + 2, y + 1, AMBER);
		int ly = y + 13;
		for (int i = 0; i < LIMBS.length; i++) {
			float base = Math.max(1.0F, s.limbBase()[i]);
			float fraction = s.limbCur()[i] / base;
			text(g, LIMBS[i], x + 2, ly, fraction < 0.5F ? 0xFFFF6A42 : DIM);
			bar(g, x + 52, ly, half - 82, 8, fraction, fraction < 0.5F ? 0xFFFF6A42 : AMBER);
			textRight(g, Math.round(fraction * 100) + "%", x + half - 4, ly, AMBER);
			ly += 13;
		}
		int rx = x + half + 4;
		text(g, "BODY", rx, y + 1, AMBER);
		int ry = y + 13;
		ry = pair(g, "Radiation", String.valueOf(Math.round(s.rads())), rx, ry, w - half - 6, s.rads() > 200 ? 0xFFFF6A42 : AMBER);
		ry = pair(g, "Threshold", String.valueOf(Math.round(s.dt())), rx, ry, w - half - 6, AMBER);
		ry = pair(g, "Resistance", Math.round(s.dr()) + "%", rx, ry, w - half - 6, AMBER);
		ry = pair(g, "Weight", Math.round(s.weight()) + "/" + Math.round(s.carry()), rx, ry, w - half - 6, s.weight() > s.carry() ? 0xFFFF6A42 : AMBER);
		ry = pair(g, "Karma", karma(s.karma()), rx, ry, w - half - 6, AMBER);
		if (s.hardcore()) {
			ry = pair(g, "Thirst", Math.round(s.thirst() / 10.0F) + "%", rx, ry, w - half - 6, AMBER);
			ry = pair(g, "Hunger", Math.round(s.hunger() / 10.0F) + "%", rx, ry, w - half - 6, AMBER);
			pair(g, "Sleep", Math.round(s.sleep() / 10.0F) + "%", rx, ry, w - half - 6, AMBER);
		}
		int next = Math.max(1, s.xpNext() - s.xpThis());
		float into = Math.max(0, s.xp() - s.xpThis());
		text(g, "XP to level " + (s.level() + 1), x + 2, y + h - 24, DIM);
		bar(g, x + 2, y + h - 12, w - 4, 8, into / next, AMBER);
	}

	private int pair(GuiGraphicsExtractor g, String label, String value, int x, int y, int w, int color) {
		text(g, label, x, y, DIM);
		textRight(g, value, x + w, y, color);
		return y + 13;
	}

	private static String karma(float v) {
		if (v >= 750) {
			return "Angel of the Wastes";
		} else if (v >= 250) {
			return "Good";
		} else if (v > -250) {
			return "Neutral";
		} else if (v > -750) {
			return "Evil";
		}
		return "Demon of the Wastes";
	}

	/** A list on the left (value at the right, the icon at the left), the selected one's text on the right. */
	private void statList(GuiGraphicsExtractor g, List<PipData.Stat> stats, int x, int y, int w, int h, int rowHeight, boolean scrollable) {
		int listW = w * 7 / 15;
		int rows = Math.max(1, h / rowHeight);
		this.scrollMax = Math.max(0, stats.size() - rows);
		int first = Math.min(this.scroll, this.scrollMax);
		for (int i = 0; i < rows && first + i < stats.size(); i++) {
			PipData.Stat st = stats.get(first + i);
			int ry = y + i * rowHeight;
			if (first + i == this.selected) {
				g.fill(x, ry, x + listW, ry + rowHeight, SELECTED);
			}
			drawIcon(g, st.icon(), x + 1, ry - 2, 0.9F);
			text(g, fit(st.name(), listW - 36), x + 18, ry + 2, first + i == this.selected ? AMBER : DIM);
			String v = Math.round(st.cur()) + (Math.round(st.cur()) != Math.round(st.base()) ? "" : "");
			textRight(g, v, x + listW - 3, ry + 2, st.cur() < st.base() ? 0xFFFF6A42 : st.cur() > st.base() ? 0xFF8CFF7A : AMBER);
		}
		PipData.Stat chosen = stats.get(Math.min(this.selected, stats.size() - 1));
		int dx = x + listW + 6, dw = w - listW - 8;
		g.fill(x + listW + 3, y, x + w, y + h, PANEL);
		drawIcon(g, chosen.icon(), dx + dw / 2 - 16, y + 2, 2.0F);
		text(g, chosen.name() + "  " + Math.round(chosen.cur()), dx, y + 38, AMBER);
		if (Math.round(chosen.base()) != Math.round(chosen.cur())) {
			text(g, "Base " + Math.round(chosen.base()), dx, y + 49, DIM);
		}
		wrapped(g, chosen.desc(), dx, y + 62, dw, DIM, Math.max(1, (h - 62) / 10));
	}

	private void perks(GuiGraphicsExtractor g, PipData.Stats s, int x, int y, int w, int h) {
		List<PipData.Perk> perks = s.perks();
		if (perks.isEmpty()) {
			text(g, "No perks yet", x + 4, y + 6, DIM);
			return;
		}
		int listW = w / 2, rows = Math.max(1, h / ROW);
		this.scrollMax = Math.max(0, perks.size() - rows);
		int first = Math.min(this.scroll, this.scrollMax);
		for (int i = 0; i < rows && first + i < perks.size(); i++) {
			int ry = y + i * ROW;
			if (first + i == this.selected) {
				g.fill(x, ry, x + listW, ry + ROW, SELECTED);
			}
			PipData.Perk perk = perks.get(first + i);
			text(g, fit(perk.name() + (perk.ranks() > 1 ? " (" + perk.rank() + ")" : ""), listW - 6), x + 3, ry + 2,
				first + i == this.selected ? AMBER : DIM);
		}
		PipData.Perk chosen = perks.get(Math.min(this.selected, perks.size() - 1));
		int dx = x + listW + 6, dw = w - listW - 8;
		g.fill(x + listW + 3, y, x + w, y + h, PANEL);
		drawIcon(g, chosen.icon(), dx + dw / 2 - 16, y + 2, 2.0F);
		text(g, chosen.name(), dx, y + 38, AMBER);
		if (chosen.ranks() > 1) {
			text(g, "Rank " + chosen.rank() + " of " + chosen.ranks(), dx, y + 49, DIM);
		}
		wrapped(g, chosen.desc(), dx, y + 62, dw, DIM, Math.max(1, (h - 62) / 10));
	}

	private void general(GuiGraphicsExtractor g, PipData.Stats s, int x, int y, int w, int h) {
		int rows = Math.max(1, h / ROW);
		int[] misc = s.misc();
		this.scrollMax = Math.max(0, Math.min(misc.length, MISC.length) - rows);
		int first = Math.min(this.scroll, this.scrollMax);
		for (int i = 0; i < rows && first + i < misc.length && first + i < MISC.length; i++) {
			int ry = y + i * ROW;
			text(g, MISC[first + i], x + 3, ry + 2, DIM);
			textRight(g, String.valueOf(misc[first + i]), x + w - 6, ry + 2, AMBER);
		}
		if (misc.length > rows) {
			int knob = Math.max(6, h * rows / misc.length);
			int ky = y + (h - knob) * first / Math.max(1, misc.length - rows);
			g.fill(x + w - 3, ky, x + w - 1, ky + knob, DIM);
		}
	}

	@Override
	protected boolean click(int x, int y, int w, int h, double mx, double my, int button) {
		if (this.tab == 1 || this.tab == 2 || this.tab == 3) {
			int listW = this.tab == 3 ? w / 2 : w * 7 / 15;
			if (mx < x + listW) {
				PipData.Stats s = PipData.stats();
				int count = s == null ? 0 : this.tab == 1 ? s.special().size() : this.tab == 2 ? s.skills().size() : s.perks().size();
				int rows = Math.max(1, h / (this.tab == 3 ? ROW : 12));
				int first = Math.min(this.scroll, Math.max(0, count - rows));
				int i = first + (int) ((my - y) / (this.tab == 3 ? ROW : 12));
				if (i >= 0 && i < count) {
					this.selected = i;
				}
			}
		}
		return true;
	}
}
