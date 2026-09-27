package dev.vegascraft.client.pip;

import dev.vegascraft.client.PipBoyPage;
import dev.vegascraft.item.NvIconNames;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.core.component.DataComponents;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.util.FormattedCharSequence;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;

/**
 * A page of the Pip-Boy in its amber: a header, a body and tabs along the bottom, laid out in the
 * rectangle of Minecraft's frame that is warped onto the Pip-Boy's screen (PipBoyPage.rect). Pages
 * override {@link #body}, {@link #click} and {@link #tabs}; lists scroll with the mouse wheel.
 */
public abstract class PipScreen extends Screen {
	public static final int AMBER = 0xFFFFB642, DIM = 0xFF8A6223, FAINT = 0x60FFB642, DARK = 0xF0100C04, PANEL = 0xC0201808, SELECTED = 0x60FFB642;
	public static final int ROW = 12;
	private static final Map<String, ItemStack> ICONS = new HashMap<>();

	protected int x0, y0, x1, y1;   // the Pip-Boy's screen, in GUI pixels
	protected int tab;
	protected int scroll;
	protected int scrollMax;		// the most the page's list can scroll (set while drawing)
	private int mouseX, mouseY;

	protected PipScreen(String title) {
		super(Component.literal(title));
	}

	@Override
	protected void init() {
		int[] r = PipBoyPage.rect(this.width, this.height);
		this.x0 = r[0];
		this.y0 = r[1];
		this.x1 = r[2];
		this.y1 = r[3];
	}

	@Override
	public boolean isPauseScreen() {
		return false;
	}

	/** The tabs along the bottom (none: no row). */
	protected List<String> tabs() {
		return List.of();
	}

	/** The page can hand over to New Vegas' own screen of this menu (an NV button in the header). */
	protected boolean hasNativeView() {
		return true;
	}

	/** The header when the full one doesn't fit. */
	protected String headerShort() {
		return header();
	}

	/** The header's right side. */
	protected String header() {
		return "";
	}

	/** Draws the page's body inside (x, y, w, h). */
	protected abstract void body(GuiGraphicsExtractor g, int x, int y, int w, int h, int mouseX, int mouseY);

	/** A click inside the body; false to let the page's frame handle it. */
	protected boolean click(int x, int y, int w, int h, double mx, double my, int button) {
		return false;
	}

	protected int bodyX() {
		return this.x0 + 4;
	}

	protected int bodyY() {
		return this.y0 + 15;
	}

	protected int bodyW() {
		return this.x1 - this.x0 - 8;
	}

	protected int bodyH() {
		return this.y1 - this.y0 - 15 - (tabs().isEmpty() ? 4 : 17);
	}

	@Override
	public void extractRenderState(GuiGraphicsExtractor g, int mx, int my, float delta) {
		this.mouseX = mx;
		this.mouseY = my;
		g.fill(this.x0, this.y0, this.x1, this.y1, DARK);
		g.outline(this.x0, this.y0, this.x1 - this.x0, this.y1 - this.y0, DIM);
		g.text(this.font, this.title.getString(), this.x0 + 5, this.y0 + 3, AMBER, false);
		String header = header();
		int right = this.x1 - 5;
		if (hasNativeView()) {
			boolean over = mx >= this.x1 - 21 && my < this.y0 + 14 && my >= this.y0;
			g.outline(this.x1 - 20, this.y0 + 2, 17, 10, over ? AMBER : DIM);
			g.text(this.font, "NV", this.x1 - 17, this.y0 + 3, over ? AMBER : DIM, false);
			right = this.x1 - 25;
		}
		int room = right - (this.x0 + 8 + this.font.width(this.title.getString()));
		if (this.font.width(header) > room) {
			header = headerShort();
		}
		if (this.font.width(header) > room) {
			header = this.font.plainSubstrByWidth(header, room);
		}
		g.text(this.font, header, right - this.font.width(header), this.y0 + 3, AMBER, false);
		g.fill(this.x0 + 3, this.y0 + 13, this.x1 - 3, this.y0 + 14, DIM);
		body(g, bodyX(), bodyY(), bodyW(), bodyH(), mx, my);
		List<String> tabs = tabs();
		if (!tabs.isEmpty()) {
			int y = this.y1 - 15;
			g.fill(this.x0 + 3, y - 1, this.x1 - 3, y, DIM);
			int[] spans = tabSpans(tabs);
			for (int i = 0; i < tabs.size(); i++) {
				int x = spans[i], w = spans[tabs.size() + i];
				if (i == this.tab) {
					g.outline(x - 2, y + 1, w + 4, 12, AMBER);
				}
				g.text(this.font, tabs.get(i), x, y + 3, i == this.tab ? AMBER : DIM, false);
			}
		}
	}

	/** Where the tab labels sit: x of each, then the width of each. */
	protected int[] tabSpans(List<String> tabs) {
		int n = tabs.size();
		int total = 0;
		for (String t : tabs) {
			total += this.font.width(t);
		}
		int gap = Math.max(6, (this.x1 - this.x0 - 12 - total) / Math.max(1, n - 1));
		int[] out = new int[2 * n];
		int x = this.x0 + 8;
		for (int i = 0; i < n; i++) {
			int w = this.font.width(tabs.get(i));
			out[i] = x;
			out[n + i] = w;
			x += w + gap;
		}
		return out;
	}

	@Override
	public boolean mouseClicked(MouseButtonEvent event, boolean doubleClick) {
		double mx = event.x(), my = event.y();
		if (hasNativeView() && mx >= this.x1 - 21 && my < this.y0 + 14 && my >= this.y0) {
			PipBoyPage.showNative();
			return true;
		}
		List<String> tabs = tabs();
		if (!tabs.isEmpty() && my >= this.y1 - 15) {
			int[] spans = tabSpans(tabs);
			for (int i = 0; i < tabs.size(); i++) {
				if (mx >= spans[i] - 4 && mx < spans[i] + spans[tabs.size() + i] + 4) {
					this.tab = i;
					this.scroll = 0;
					tabChanged();
					return true;
				}
			}
			return true;
		}
		if (mx >= bodyX() && mx < bodyX() + bodyW() && my >= bodyY() && my < bodyY() + bodyH()) {
			click(bodyX(), bodyY(), bodyW(), bodyH(), mx, my, event.button());
		}
		return true;
	}

	protected void tabChanged() {
	}

	@Override
	public boolean mouseScrolled(double x, double y, double horizontal, double vertical) {
		this.scroll = Math.max(0, Math.min(this.scrollMax, this.scroll - (int) Math.signum(vertical)));
		return true;
	}

	// ---- drawing helpers ----

	/** The text cut to `width` pixels, with ".." where it was shortened. */
	protected String fit(String s, int width) {
		if (this.font.width(s) <= width) {
			return s;
		}
		return this.font.plainSubstrByWidth(s, Math.max(0, width - this.font.width(".."))).stripTrailing() + "..";
	}

	protected void text(GuiGraphicsExtractor g, String s, int x, int y, int color) {
		g.text(this.font, s, x, y, color, false);
	}

	protected void textRight(GuiGraphicsExtractor g, String s, int right, int y, int color) {
		g.text(this.font, s, right - this.font.width(s), y, color, false);
	}

	/** Word-wrapped text; returns the y after the last line. */
	protected int wrapped(GuiGraphicsExtractor g, String s, int x, int y, int w, int color, int maxLines) {
		int lines = 0;
		List<FormattedCharSequence> all = this.font.split(Component.literal(s), w);
		for (FormattedCharSequence line : all) {
			if (lines++ >= maxLines) {
				break;
			}
			g.text(this.font, line, x, y, color, false);
			if (lines == maxLines && all.size() > maxLines) {
				g.text(this.font, "..", x + this.font.width(line) + 2, y, color, false);
			}
			y += 10;
		}
		return y;
	}

	/** A bar: the fraction of its width filled, with a thin frame. */
	protected void bar(GuiGraphicsExtractor g, int x, int y, int w, int h, float fraction, int color) {
		g.outline(x, y, w, h, DIM);
		int fill = Math.round((w - 2) * Math.max(0.0F, Math.min(1.0F, fraction)));
		if (fill > 0) {
			g.fill(x + 1, y + 1, x + 1 + fill, y + h - 1, color);
		}
	}

	/** The Pip-Boy image of an item path, as an item stack (null when the icon pack doesn't have it). */
	protected ItemStack icon(String path) {
		String model = NvIconNames.model(path);
		if (model == null) {
			return ItemStack.EMPTY;
		}
		return ICONS.computeIfAbsent(model, m -> {
			ItemStack s = new ItemStack(Items.STICK);
			s.set(DataComponents.ITEM_MODEL, Identifier.parse(m));
			return s;
		});
	}

	/** An icon at (x, y), `scale` times 16 pixels. */
	protected void drawIcon(GuiGraphicsExtractor g, String path, int x, int y, float scale) {
		ItemStack s = icon(path);
		if (s.isEmpty()) {
			return;
		}
		g.pose().pushMatrix();
		g.pose().translate(x, y);
		g.pose().scale(scale, scale);
		g.item(s, 0, 0);
		g.pose().popMatrix();
	}

	protected int mouseX() {
		return this.mouseX;
	}

	protected int mouseY() {
		return this.mouseY;
	}
}
