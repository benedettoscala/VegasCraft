package dev.vegascraft.client.pip;

import static dev.vegascraft.link.Proto.EV_PIPBOY_RADIO;
import static dev.vegascraft.link.Proto.EV_PIPBOY_TRACK;
import static dev.vegascraft.link.Proto.EV_PIPBOY_TRAVEL;

import dev.vegascraft.client.PipBoyPage;
import dev.vegascraft.link.SkyLink;
import dev.vegascraft.pip.PipData;
import dev.vegascraft.client.icons.NvIcons;
import java.util.List;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.resources.Identifier;
import net.minecraft.network.chat.Component;
import net.minecraft.util.FormattedCharSequence;

/** The Pip-Boy's DATA menu: Local Map, World Map, Quests, Misc (notes) and Radio. */
public final class DataScreen extends PipScreen {
	private static final List<String> TABS = List.of("Local Map", "World Map", "Quests", "Misc", "Radio");
	private static final int LOCAL_MAP = 0, WORLD_MAP = 1, QUESTS = 2, NOTES = 3, RADIO = 4;
	private static int remembered = QUESTS;
	private int selected;
	private int textScroll;
	private int bodyX, bodyW, bodyY, bodyH;   // the last drawn body, for the wheel and the map
	private static final int MAP_SIZE = 1024;
	private static final String[] MARKER_ICONS = { "undiscovered", "city", "settlement", "encampment", "natural_landmark", "cave", "factory", "monument",
		"military", "office", "ruins_town", "ruins_urban", "ruins_sewer", "metro", "vault" };
	private float zoom = 2.0F, centreU = 0.5F, centreV = 0.5F;   // the world map's view (centre as fractions of the image)
	private boolean centred;
	private int markerRef;      // the selected marker

	public DataScreen() {
		super("DATA");
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
		this.textScroll = 0;
	}

	@Override
	protected String header() {
		PipData.Stats s = PipData.stats();
		return s == null ? "" : "LVL " + s.level() + "   HP " + Math.round(s.hp()) + "/" + Math.round(s.hpMax());
	}

	@Override
	protected void body(GuiGraphicsExtractor g, int x, int y, int w, int h, int mx, int my) {
		this.bodyX = x;
		this.bodyW = w;
		this.bodyY = y;
		this.bodyH = h;
		switch (this.tab) {
			case WORLD_MAP -> worldMap(g, x, y, w, h, mx, my);
			case QUESTS -> quests(g, x, y, w, h);
			case NOTES -> notes(g, x, y, w, h);
			case RADIO -> radio(g, x, y, w, h);
			default -> localMap(g, x, y, w, mx, my);
		}
	}

	private int rows(int h) {
		return Math.max(1, h / ROW);
	}

	private int first(int count, int h) {
		this.scrollMax = Math.max(0, count - rows(h));
		return Math.min(this.scroll, this.scrollMax);
	}

	private void listRow(GuiGraphicsExtractor g, String label, int x, int y, int listW, boolean chosen, int color) {
		if (chosen) {
			g.fill(x, y, x + listW, y + ROW, SELECTED);
		}
		text(g, fit(label, listW - 6), x + 3, y + 2, chosen ? AMBER : color);
	}

	/** Word-wrapped text from line `skip`; returns how many lines it has in all. */
	private int paragraph(GuiGraphicsExtractor g, String s, int x, int y, int w, int h, int skip, int color) {
		List<FormattedCharSequence> lines = this.font.split(Component.literal(s), w);
		int fit = Math.max(1, h / 10);
		for (int i = skip; i < lines.size() && i - skip < fit; i++) {
			g.text(this.font, lines.get(i), x, y + (i - skip) * 10, color, false);
		}
		return lines.size();
	}

	// ---- Quests ----

	private void quests(GuiGraphicsExtractor g, int x, int y, int w, int h) {
		PipData.Quests data = PipData.quests();
		if (data == null || data.quests().isEmpty()) {
			text(g, data == null ? "Waiting for New Vegas..." : "No active quests.", x + 4, y + 6, DIM);
			return;
		}
		List<PipData.Quest> list = data.quests();
		this.selected = Math.min(this.selected, list.size() - 1);
		int listW = w * 9 / 20, first = first(list.size(), h);
		for (int i = 0; i < rows(h) && first + i < list.size(); i++) {
			PipData.Quest q = list.get(first + i);
			listRow(g, (q.tracked() ? "> " : "") + q.name(), x, y + i * ROW, listW, first + i == this.selected, q.tracked() ? AMBER : DIM);
		}
		PipData.Quest chosen = list.get(this.selected);
		int dx = x + listW + 6, dw = w - listW - 8;
		g.fill(x + listW + 3, y, x + w, y + h, PANEL);
		text(g, fit(chosen.name(), dw), dx, y + 2, AMBER);
		int oy = y + 15;
		for (PipData.Objective o : chosen.objectives()) {
			int color = o.done() ? DIM : AMBER;
			String mark = o.done() ? "[x] " : "[ ] ";
			List<FormattedCharSequence> lines = this.font.split(Component.literal(mark + o.text()), dw);
			for (FormattedCharSequence line : lines) {
				if (oy > y + h - 24) {
					break;
				}
				g.text(this.font, line, dx, oy, color, false);
				oy += 10;
			}
			oy += 2;
		}
		int bx = x + w - 62, by = y + h - 16;
		boolean over = mouseX() >= bx && mouseX() < bx + 58 && mouseY() >= by && mouseY() < by + 13;
		g.outline(bx, by, 58, 13, chosen.tracked() ? DIM : over ? AMBER : DIM);
		text(g, chosen.tracked() ? "Tracked" : "Track", bx + (chosen.tracked() ? 10 : 15), by + 3, chosen.tracked() ? DIM : AMBER);
	}

	// ---- Local map ----

	/** New Vegas draws the local map from its own render of the cell; this page hands over to that screen. */
	private void localMap(GuiGraphicsExtractor g, int x, int y, int w, int mx, int my) {
		wrapped(g, "New Vegas draws the local map from its own render of the cell: use its own screen for it.", x + 4, y + 6, w - 8, DIM, 4);
		int bx = x + w / 2 - 60, by = y + 56;
		boolean over = mx >= bx && mx < bx + 120 && my >= by && my < by + 16;
		g.outline(bx, by, 120, 16, over ? AMBER : DIM);
		text(g, "Open NV screen", bx + 20, by + 4, over ? AMBER : DIM);
	}

	// ---- Notes ----

	private void notes(GuiGraphicsExtractor g, int x, int y, int w, int h) {
		PipData.Notes data = PipData.notes();
		if (data == null || data.notes().isEmpty()) {
			text(g, data == null ? "Waiting for New Vegas..." : "No notes.", x + 4, y + 6, DIM);
			return;
		}
		List<PipData.Note> list = data.notes();
		this.selected = Math.min(this.selected, list.size() - 1);
		int listW = w * 9 / 20, first = first(list.size(), h);
		for (int i = 0; i < rows(h) && first + i < list.size(); i++) {
			PipData.Note n = list.get(first + i);
			listRow(g, n.name(), x, y + i * ROW, listW, first + i == this.selected, n.read() ? DIM : AMBER);
		}
		PipData.Note chosen = list.get(this.selected);
		int dx = x + listW + 6, dw = w - listW - 12;
		g.fill(x + listW + 3, y, x + w, y + h, PANEL);
		text(g, fit(chosen.name(), dw), dx, y + 2, AMBER);
		if (chosen.text().isEmpty()) {
			text(g, "(no text: use NV)", dx, y + 16, DIM);
			return;
		}
		int total = paragraph(g, chosen.text(), dx, y + 15, dw, h - 17, this.textScroll, DIM);
		int fit = Math.max(1, (h - 17) / 10);
		if (total > fit) {
			this.textScroll = Math.min(this.textScroll, total - fit);
			int knob = Math.max(6, (h - 17) * fit / total);
			int ky = y + 15 + (h - 17 - knob) * this.textScroll / Math.max(1, total - fit);
			g.fill(x + w - 3, ky, x + w - 1, ky + knob, DIM);
		}
	}

	// ---- Radio ----

	private void radio(GuiGraphicsExtractor g, int x, int y, int w, int h) {
		PipData.Radio data = PipData.radio();
		if (data == null) {
			text(g, "Waiting for New Vegas...", x + 4, y + 6, DIM);
			return;
		}
		text(g, "Pip-Boy radio: " + (data.enabled() ? "ON" : "OFF"), x + 3, y + 2, AMBER);
		if (data.enabled()) {
			boolean over = mouseX() >= x + w - 62 && mouseX() < x + w - 2 && mouseY() >= y && mouseY() < y + 13;
			g.outline(x + w - 62, y, 60, 13, over ? AMBER : DIM);
			text(g, "Turn off", x + w - 52, y + 3, over ? AMBER : DIM);
		}
		this.scrollMax = Math.max(0, data.stations().size() - (h - 30) / ROW);
		int ry = y + 16, first = Math.min(this.scroll, this.scrollMax);
		for (int i = first; i < data.stations().size() && ry + ROW <= y + h - 14; i++, ry += ROW) {
			PipData.Station st = data.stations().get(i);
			boolean hover = st.inRange() && mouseX() >= x && mouseX() < x + w && mouseY() >= ry && mouseY() < ry + ROW;
			if (st.playing() || hover) {
				g.fill(x, ry, x + w, ry + ROW, SELECTED);
			}
			text(g, fit((st.playing() ? "> " : "") + st.name(), w - 90), x + 3, ry + 2, st.inRange() ? AMBER : DIM);
			textRight(g, st.playing() ? "playing" : st.inRange() ? "in range" : "out of range", x + w - 6, ry + 2, st.inRange() ? AMBER : DIM);
		}
		text(g, "Click a station in range to tune it.", x + 3, y + h - 11, DIM);
	}


	// ---- World map ----

	/** The image pixels per GUI pixel of the map at the current zoom. */
	private float mapScale() {
		return Math.min(this.bodyW, this.bodyH - 14) * this.zoom / MAP_SIZE;
	}

	private float[] toScreen(PipData.WorldMap m, float wx, float wy) {
		float u = (wx - m.west()) / (m.east() - m.west()), v = (m.north() - wy) / (m.north() - m.south());
		float scale = mapScale(), mh = this.bodyH - 14;
		return new float[] { this.bodyX + this.bodyW * 0.5F + (u - this.centreU) * MAP_SIZE * scale, this.bodyY + mh * 0.5F + (v - this.centreV) * MAP_SIZE * scale };
	}

	private void clampView() {
		float scale = mapScale(), halfU = this.bodyW * 0.5F / (MAP_SIZE * scale), halfV = (this.bodyH - 14) * 0.5F / (MAP_SIZE * scale);
		this.centreU = halfU >= 0.5F ? 0.5F : Math.max(halfU, Math.min(1.0F - halfU, this.centreU));
		this.centreV = halfV >= 0.5F ? 0.5F : Math.max(halfV, Math.min(1.0F - halfV, this.centreV));
	}

	private void worldMap(GuiGraphicsExtractor g, int x, int y, int w, int h, int mx, int my) {
		PipData.WorldMap m = PipData.map();
		if (m == null || m.texture().isEmpty()) {
			text(g, m == null ? "Waiting for New Vegas..." : "No world map here. Use NV.", x + 4, y + 6, DIM);
			return;
		}
		if (!this.centred) {
			this.centred = true;
			this.centreU = (m.playerX() - m.west()) / (m.east() - m.west());
			this.centreV = (m.north() - m.playerY()) / (m.north() - m.south());
		}
		clampView();
		int mh = h - 14;
		float scale = mapScale();
		// The part of the image on screen.
		float ox = this.centreU * MAP_SIZE - w * 0.5F / scale, oy = this.centreV * MAP_SIZE - mh * 0.5F / scale;
		float u0 = Math.max(0, ox), v0 = Math.max(0, oy), u1 = Math.min(MAP_SIZE, ox + w / scale), v1 = Math.min(MAP_SIZE, oy + mh / scale);
		g.enableScissor(x, y, x + w, y + mh);
		if (u1 > u0 && v1 > v0) {
			Identifier tex = Identifier.parse("vegascraft:textures/gui/map/" + NvIcons.mapId(m.texture()) + ".png");
			int dx = Math.round(x + (u0 - ox) * scale), dy = Math.round(y + (v0 - oy) * scale);
			int dw = Math.round((u1 - u0) * scale), dh = Math.round((v1 - v0) * scale);
			g.blit(RenderPipelines.GUI_TEXTURED, tex, dx, dy, u0, v0, dw, dh, Math.round(u1 - u0), Math.round(v1 - v0), MAP_SIZE, MAP_SIZE);
		}
		PipData.Marker hover = null;
		for (PipData.Marker k : m.markers()) {
			float[] p = toScreen(m, k.x(), k.y());
			if (p[0] < x - 6 || p[0] > x + w + 6 || p[1] < y - 6 || p[1] > y + mh + 6) {
				continue;
			}
			String icon = "interface\\icons\\world map\\icon_map_" + MARKER_ICONS[k.type() >= 0 && k.type() < MARKER_ICONS.length ? k.type() : 2] + ".dds";
			drawIcon(g, icon, Math.round(p[0]) - 5, Math.round(p[1]) - 6, 0.7F);
			if (k.ref() == this.markerRef) {
				g.outline(Math.round(p[0]) - 6, Math.round(p[1]) - 6, 12, 12, AMBER);
			}
			if (Math.abs(mx - p[0]) < 6 && Math.abs(my - p[1]) < 6) {
				hover = k;
			}
		}
		// The player: a bright square with a line toward where it looks (north is up; New Vegas' heading runs clockwise from north).
		float[] pp = toScreen(m, m.playerX(), m.playerY());
		for (int i = 1; i <= 5; i++) {
			int px = Math.round(pp[0] + (float) Math.sin(m.heading()) * i * 1.6F), py = Math.round(pp[1] - (float) Math.cos(m.heading()) * i * 1.6F);
			g.fill(px, py, px + 1, py + 1, 0xFFFFFFFF);
		}
		g.fill(Math.round(pp[0]) - 1, Math.round(pp[1]) - 1, Math.round(pp[0]) + 2, Math.round(pp[1]) + 2, 0xFFFFFFFF);
		g.disableScissor();
		g.outline(x, y, w, mh, DIM);
		// The strip below: the marker under the cursor or the chosen one, and its Travel button.
		PipData.Marker shown = hover;
		if (shown == null) {
			for (PipData.Marker k : m.markers()) {
				if (k.ref() == this.markerRef) {
					shown = k;
				}
			}
		}
		if (shown != null) {
			text(g, fit(shown.name(), w - 70), x + 3, y + h - 11, AMBER);
			if (shown.ref() == this.markerRef && shown.canTravel()) {
				int bx = x + w - 62, by = y + h - 13;
				boolean over = mx >= bx && mx < bx + 60 && my >= by && my < by + 13;
				g.outline(bx, by, 60, 13, over ? AMBER : DIM);
				text(g, "Travel", bx + 14, by + 3, AMBER);
			}
		} else {
			text(g, "Wheel zooms, drag moves, click a marker.", x + 3, y + h - 11, DIM);
		}
	}

	private boolean mapClick(double mx, double my) {
		PipData.WorldMap m = PipData.map();
		if (m == null || m.texture().isEmpty()) {
			return true;
		}
		if (my >= this.bodyY + this.bodyH - 14) {
			if (this.markerRef != 0 && mx >= this.bodyX + this.bodyW - 62) {
				for (PipData.Marker k : m.markers()) {
					if (k.ref() == this.markerRef && k.canTravel()) {
						SkyLink.pushEvent(EV_PIPBOY_TRAVEL, k.ref(), 0.0F, 0.0F, 0.0F, 0.0F, 0);
					}
				}
			}
			return true;
		}
		PipData.Marker nearest = null;
		double best = 36;
		for (PipData.Marker k : m.markers()) {
			float[] p = toScreen(m, k.x(), k.y());
			double d = (mx - p[0]) * (mx - p[0]) + (my - p[1]) * (my - p[1]);
			if (d < best) {
				best = d;
				nearest = k;
			}
		}
		this.markerRef = nearest == null ? 0 : nearest.ref();
		return true;
	}

	@Override
	public boolean mouseDragged(MouseButtonEvent event, double dragX, double dragY) {
		if (this.tab == WORLD_MAP && PipData.map() != null) {
			float scale = mapScale();
			this.centreU -= (float) dragX / (MAP_SIZE * scale);
			this.centreV -= (float) dragY / (MAP_SIZE * scale);
			clampView();
			return true;
		}
		return super.mouseDragged(event, dragX, dragY);
	}

	@Override
	protected boolean click(int x, int y, int w, int h, double mx, double my, int button) {
		if (this.tab == WORLD_MAP) {
			return mapClick(mx, my);
		}
		if (this.tab == RADIO) {
			PipData.Radio radio = PipData.radio();
			if (radio == null) {
				return true;
			}
			if (radio.enabled() && my < y + 13 && mx >= x + w - 62) {
				SkyLink.pushEvent(EV_PIPBOY_RADIO, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0);
				return true;
			}
			int first = Math.min(this.scroll, Math.max(0, radio.stations().size() - (h - 30) / ROW));
			int i = first + (int) ((my - (y + 16)) / ROW);
			if (my >= y + 16 && i >= 0 && i < radio.stations().size() && radio.stations().get(i).inRange()) {
				SkyLink.pushEvent(EV_PIPBOY_RADIO, radio.stations().get(i).ref(), 1.0F, 0.0F, 0.0F, 0.0F, 0);
			}
			return true;
		}
		if (this.tab == LOCAL_MAP) {
			int bx = x + w / 2 - 60, by = y + 56;
			if (mx >= bx && mx < bx + 120 && my >= by && my < by + 16) {
				PipBoyPage.showNative();
			}
			return true;
		}
		int listW = w * 9 / 20;
		if (this.tab == QUESTS || this.tab == NOTES) {
			PipData.Quests qs = PipData.quests();
			PipData.Notes ns = PipData.notes();
			int count = this.tab == QUESTS ? (qs == null ? 0 : qs.quests().size()) : (ns == null ? 0 : ns.notes().size());
			if (mx < x + listW) {
				int i = first(count, h) + (int) ((my - y) / ROW);
				if (i >= 0 && i < count) {
					this.selected = i;
					this.textScroll = 0;
				}
			} else if (this.tab == QUESTS && qs != null && !qs.quests().isEmpty() && mx >= x + w - 62 && my >= y + h - 16) {
				PipData.Quest q = qs.quests().get(Math.min(this.selected, qs.quests().size() - 1));
				if (!q.tracked()) {
					SkyLink.pushEvent(EV_PIPBOY_TRACK, q.form(), 0.0F, 0.0F, 0.0F, 0.0F, 0);
				}
			}
		}
		return true;
	}

	@Override
	public boolean mouseScrolled(double x, double y, double horizontal, double vertical) {
		if (this.tab == WORLD_MAP) {
			this.zoom = Math.max(1.0F, Math.min(8.0F, this.zoom * (vertical > 0 ? 1.25F : 0.8F)));
			clampView();
			return true;
		}
		if (this.tab == NOTES && x >= this.bodyX + this.bodyW * 9 / 20) {
			this.textScroll = Math.max(0, this.textScroll - (int) Math.signum(vertical) * 3);
			return true;
		}
		return super.mouseScrolled(x, y, horizontal, vertical);
	}
}
