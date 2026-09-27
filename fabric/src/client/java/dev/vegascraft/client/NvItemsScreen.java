package dev.vegascraft.client;

import static dev.vegascraft.link.Proto.*;

import dev.vegascraft.item.NvInventory;
import dev.vegascraft.item.NvItems;
import dev.vegascraft.link.SkyLink;
import dev.vegascraft.net.SkyNet;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import com.mojang.blaze3d.platform.InputConstants;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.network.chat.Component;
import net.minecraft.world.item.ItemStack;

/**
 * The Pip-Boy's New Vegas page: every item New Vegas holds, by category, with its Pip-Boy image,
 * count and numbers. Apparel is worn or taken off, aid used, anything dropped. As in Minecraft's
 * inventory, a click picks an item up and a click on a hotbar slot puts its link there (a click on
 * a link picks it up again; New Vegas' hotkeys 1-8 still own their slots). Drawn in the
 * Pip-Boy's amber over its screen (PipBoyPage); the Minecraft tab goes back to crafting.
 */
public final class NvItemsScreen extends Screen {
	private enum Category {
		WEAPONS("Weapons"), APPAREL("Apparel"), AID("Aid"), MISC("Misc"), AMMO("Ammo");

		final String label;

		Category(String label) {
			this.label = label;
		}

		boolean holds(NvItems.Taken t) {
			return switch (this) {
				case WEAPONS -> t.kind() == ITEM_WEAPON;
				case APPAREL -> t.kind() == ITEM_ARMOR;
				case AID -> t.kind() == ITEM_AID;
				case AMMO -> t.kind() == ITEM_AMMO;
				case MISC -> t.kind() == ITEM_MISC || t.kind() == ITEM_KEY || t.kind() == ITEM_BOOK;
			};
		}
	}

	private static final int AMBER = 0xFFFFB642, DIM = 0xFF8A6223, DARK = 0xF0100C04, PANEL = 0xC0201808, SELECTED = 0x60FFB642;
	private static final int ROW = 18, SLOT = 18;
	private static final ItemStack CRAFTING = new ItemStack(net.minecraft.world.item.Items.CRAFTING_TABLE);
	private static Category category = Category.WEAPONS;
	private static int selected;  // form id
	private final Map<Integer, ItemStack> icons = new HashMap<>();
	private int x0, y0, x1, y1;   // the Pip-Boy screen, GUI pixels
	private int scroll;
	private int carried;          // form id on the cursor, waiting for a hotbar slot (0: none)

	public NvItemsScreen() {
		super(Component.literal("New Vegas"));
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

	private List<NvItems.Taken> list() {
		List<NvItems.Taken> out = new ArrayList<>();
		for (NvItems.Taken t : NvInventory.items()) {
			if (category.holds(t)) {
				out.add(t);
			}
		}
		out.sort(Comparator.comparing((NvItems.Taken t) -> t.name().toLowerCase(java.util.Locale.ROOT)));
		return out;
	}

	private ItemStack icon(NvItems.Taken t) {
		return this.icons.computeIfAbsent(t.formId(), f -> NvItems.create(t, 1, -1));
	}

	// Layout: tabs on top, the list on the left, details on the right, the hotbar at the bottom.
	private int tabsBottom() {
		return this.y0 + 14;
	}

	private int hotbarTop() {
		return this.y1 - SLOT - 4;
	}

	private int listRight() {
		return this.x0 + (this.x1 - this.x0) * 11 / 20;
	}

	private int visibleRows() {
		return Math.max(1, (hotbarTop() - 4 - tabsBottom() - 2) / ROW);
	}

	private int hotbarLeft() {
		return (this.x0 + this.x1) / 2 - SLOT * 9 / 2;
	}

	@Override
	public void extractRenderState(GuiGraphicsExtractor g, int mouseX, int mouseY, float delta) {
		g.fill(this.x0, this.y0, this.x1, this.y1, DARK);
		g.outline(this.x0, this.y0, this.x1 - this.x0, this.y1 - this.y0, DIM);
		// Tabs, and the way back to Minecraft's page.
		int x = this.x0 + 4;
		for (Category c : Category.values()) {
			int w = this.font.width(c.label) + 8;
			if (c == category) {
				g.outline(x, this.y0 + 2, w, 11, AMBER);
			}
			g.text(this.font, c.label, x + 4, this.y0 + 4, c == category ? AMBER : DIM, false);
			x += w + 2;
		}
		// Minecraft's page (inventory and crafting): a crafting table in the top right corner.
		boolean overCraft = mouseX >= this.x1 - 17 && mouseY >= this.y0 && mouseY < tabsBottom();
		g.outline(this.x1 - 16, this.y0 + 1, 14, 12, overCraft ? AMBER : DIM);
		g.pose().pushMatrix();
		g.pose().translate(this.x1 - 15, this.y0 + 1.5F);
		g.pose().scale(0.75F, 0.7F);
		g.item(CRAFTING, 0, 0);
		g.pose().popMatrix();
		// The list.
		List<NvItems.Taken> items = list();
		int rows = visibleRows();
		this.scroll = Math.max(0, Math.min(this.scroll, items.size() - rows));
		int top = tabsBottom() + 2;
		NvItems.Taken chosen = null;
		for (NvItems.Taken t : items) {
			if (t.formId() == selected) {
				chosen = t;
			}
		}
		if (chosen == null && !items.isEmpty()) {
			chosen = items.get(0);
			selected = chosen.formId();
		}
		g.enableScissor(this.x0 + 2, top, listRight(), top + rows * ROW);
		for (int i = 0; i < rows && this.scroll + i < items.size(); i++) {
			NvItems.Taken t = items.get(this.scroll + i);
			int y = top + i * ROW;
			if (t.formId() == selected) {
				g.fill(this.x0 + 2, y, listRight(), y + ROW, SELECTED);
			}
			g.item(icon(t), this.x0 + 4, y + 1);
			String mark = t.equipped() ? "■ " : "";
			String count = t.count() > 1 ? " (" + t.count() + ")" : "";
			int room = listRight() - this.x0 - 28 - this.font.width(mark + count);
			String name = t.name();
			if (this.font.width(name) > room) {
				name = this.font.plainSubstrByWidth(name, Math.max(0, room - this.font.width(".."))).stripTrailing() + "..";
			}
			g.text(this.font, mark + name + count, this.x0 + 23, y + 5, AMBER, false);
		}
		g.disableScissor();
		if (items.size() > rows) {
			int barTop = top, barH = rows * ROW, knob = Math.max(6, barH * rows / items.size());
			int knobY = barTop + (barH - knob) * this.scroll / Math.max(1, items.size() - rows);
			g.fill(listRight() - 2, knobY, listRight(), knobY + knob, DIM);
		}
		if (items.isEmpty()) {
			g.text(this.font, "Nothing here", this.x0 + 8, top + 6, DIM, false);
		}
		// Details and actions.
		int dx = listRight() + 6, dy = top;
		g.fill(listRight() + 2, top, this.x1 - 3, hotbarTop() - 4, PANEL);
		if (chosen != null) {
			g.pose().pushMatrix();
			g.pose().translate(dx, dy + 2);
			g.pose().scale(2.0F, 2.0F);
			g.item(icon(chosen), 0, 0);
			g.pose().popMatrix();
			int tx = dx + 36, ty = dy + 4;
			for (String line : wrap(chosen.name(), this.x1 - tx - 6)) {
				g.text(this.font, line, tx, ty, AMBER, false);
				ty += 10;
			}
			ty = Math.max(ty, dy + 38);
			List<String> stats = new ArrayList<>();
			if (chosen.kind() == ITEM_WEAPON && chosen.damage() > 0) {
				stats.add("DAM " + Math.round(chosen.damage()));
			}
			stats.add("VAL " + chosen.value());
			stats.add(String.format("WG %.1f", chosen.weight()));
			stats.add("COUNT " + chosen.count());
			if (chosen.equipped()) {
				stats.add("Equipped");
			}
			if (chosen.hotkey() >= 0) {
				stats.add("Hotkey " + (chosen.hotkey() + 1));
			}
			stats.add("Click, then a slot");
			stats.add("1-8: NV hotkey");
			for (String s : stats) {
				g.text(this.font, s, dx + 2, ty, DIM, false);
				ty += 10;
			}
			int by = hotbarTop() - 4 - 14;
			for (Action a : actions(chosen)) {
				int w = this.font.width(a.label) + 8;
				boolean hover = mouseX >= dx && mouseX < dx + w && mouseY >= by && mouseY < by + 12;
				g.outline(dx, by, w, 12, hover ? AMBER : DIM);
				g.text(this.font, a.label, dx + 4, by + 2, hover ? AMBER : DIM, false);
				dx += w + 3;
			}
		}
		// The hotbar: drop targets for links.
		int hx = hotbarLeft(), hy = hotbarTop();
		var player = this.minecraft.player;
		for (int i = 0; i < 9; i++) {
			int sx = hx + i * SLOT;
			boolean target = this.carried != 0 && mouseX >= sx && mouseX < sx + SLOT && mouseY >= hy && mouseY < hy + SLOT;
			g.outline(sx, hy, SLOT, SLOT, target ? AMBER : DIM);
			if (player != null) {
				ItemStack stack = player.getInventory().getItem(i);
				g.item(stack, sx + 1, hy + 1);
				g.itemDecorations(this.font, stack, sx + 1, hy + 1);
			}
		}
		if (this.carried != 0) {
			for (NvItems.Taken t : NvInventory.items()) {
				if (t.formId() == this.carried) {
					g.item(icon(t), mouseX - 8, mouseY - 8);
				}
			}
		}
	}

	private List<String> wrap(String text, int width) {
		List<String> lines = new ArrayList<>();
		String rest = text;
		while (!rest.isEmpty() && lines.size() < 3) {
			String part = this.font.plainSubstrByWidth(rest, Math.max(10, width));
			if (part.length() < rest.length()) {
				int space = part.lastIndexOf(' ');
				if (space > 0) {
					part = part.substring(0, space);
				}
			}
			lines.add(part);
			rest = rest.substring(part.length()).trim();
		}
		return lines;
	}

	private record Action(String label, Runnable run) {
	}

	private List<Action> actions(NvItems.Taken t) {
		List<Action> out = new ArrayList<>();
		if (t.kind() == ITEM_ARMOR) {
			out.add(new Action(t.equipped() ? "Take off" : "Wear", () -> SkyLink.pushEvent(EV_NV_EQUIP, t.formId(), t.equipped() ? 0.0F : 1.0F, 0.0F, 0.0F, 0.0F, 0)));
		}
		if (t.kind() == ITEM_AID) {
			out.add(new Action("Use", () -> SkyLink.pushEvent(EV_NV_USE, t.formId(), 1.0F, 0.0F, 0.0F, 0.0F, 0)));
		}
		out.add(new Action("Drop", () -> SkyLink.pushEvent(EV_NV_DROP, t.formId(), 1.0F, 0.0F, 0.0F, 0.0F, 0)));
		if (t.count() > 1) {
			out.add(new Action("All", () -> SkyLink.pushEvent(EV_NV_DROP, t.formId(), t.count(), 0.0F, 0.0F, 0.0F, 0)));
		}
		return out;
	}

	@Override
	public boolean mouseClicked(MouseButtonEvent event, boolean doubleClick) {
		double mx = event.x(), my = event.y();
		boolean right = event.button() == InputConstants.MOUSE_BUTTON_RIGHT;
		// Tabs.
		if (my >= this.y0 && my < tabsBottom()) {
			if (mx >= this.x1 - 17) {
				PipBoyPage.showMinecraft(this.minecraft);
				return true;
			}
			int x = this.x0 + 4;
			for (Category c : Category.values()) {
				int w = this.font.width(c.label) + 8;
				if (mx >= x && mx < x + w) {
					category = c;
					this.scroll = 0;
					return true;
				}
				x += w + 2;
			}
			return true;
		}
		// The list: a click selects the item and picks it up (a right click only selects).
		List<NvItems.Taken> items = list();
		int top = tabsBottom() + 2;
		if (mx >= this.x0 && mx < listRight() && my >= top && my < top + visibleRows() * ROW) {
			int i = this.scroll + (int) ((my - top) / ROW);
			if (i >= 0 && i < items.size()) {
				selected = items.get(i).formId();
				this.carried = right ? 0 : selected;
			}
			return true;
		}
		// The hotbar: the item picked up goes into the slot; a link in it is picked up instead
		// (a right click takes it away).
		int hx = hotbarLeft(), hy = hotbarTop();
		if (my >= hy && my < hy + SLOT && mx >= hx && mx < hx + SLOT * 9) {
			int slot = (int) ((mx - hx) / SLOT);
			if (this.carried != 0 && !right) {
				sendLink(slot, this.carried);
				this.carried = 0;
			} else if (this.carried != 0) {
				this.carried = 0;
			} else {
				var player = this.minecraft.player;
				var tag = player == null ? null : NvItems.data(player.getInventory().getItem(slot));
				if (tag != null) {
					if (!right) {
						this.carried = tag.getIntOr("id", 0);
					}
					sendLink(slot, 0);
				}
			}
			return true;
		}
		// Actions.
		NvItems.Taken chosen = null;
		for (NvItems.Taken t : items) {
			if (t.formId() == selected) {
				chosen = t;
			}
		}
		if (chosen != null) {
			int dx = listRight() + 6, by = hotbarTop() - 4 - 14;
			for (Action a : actions(chosen)) {
				int w = this.font.width(a.label) + 8;
				if (mx >= dx && mx < dx + w && my >= by && my < by + 12) {
					a.run().run();
					this.carried = 0;
					return true;
				}
				dx += w + 3;
			}
		}
		this.carried = 0;  // a click elsewhere puts the item back
		return true;
	}

	/** 1-8 on the chosen item: New Vegas' own hotkey for it (what the Pip-Boy's list does with its number keys). */
	@Override
	public boolean keyPressed(KeyEvent event) {
		int key = event.key();
		if (key >= InputConstants.KEY_1 && key <= InputConstants.KEY_8 && selected != 0) {
			for (NvItems.Taken t : list()) {
				if (t.formId() == selected) {
					SkyLink.pushEvent(EV_NV_HOTKEY, t.formId(), key - InputConstants.KEY_1 + 1, 0.0F, 0.0F, 0.0F, 0);
					return true;
				}
			}
		}
		return super.keyPressed(event);
	}

	@Override
	public boolean mouseScrolled(double x, double y, double horizontal, double vertical) {
		this.scroll -= (int) Math.signum(vertical);
		return true;
	}

	private void sendLink(int slot, int form) {
		if (ClientPlayNetworking.canSend(SkyNet.NvLink.TYPE)) {
			ClientPlayNetworking.send(new SkyNet.NvLink(slot, form));
		}
	}
}
