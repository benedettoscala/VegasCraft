package dev.vegascraft.client;

import dev.vegascraft.client.mixin.AbstractContainerScreenAccessor;
import dev.vegascraft.client.pip.DataScreen;
import dev.vegascraft.client.pip.StatsScreen;
import dev.vegascraft.link.Proto;
import dev.vegascraft.link.SkyLink;
import dev.vegascraft.net.SkyNet;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.fabricmc.fabric.api.client.screen.v1.ScreenEvents;
import net.fabricmc.fabric.api.client.screen.v1.Screens;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.AbstractWidget;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.inventory.CraftingScreen;
import net.minecraft.network.chat.Component;

/**
 * The Pip-Boy's pages (Tab opens the Pip-Boy, F1 F2 F3 are its STATS, ITEMS and DATA buttons), laid over
 * the Pip-Boy's screen. STATS and DATA are pages of New Vegas' data (dev.vegascraft.client.pip); ITEMS is
 * New Vegas' items (NvItemsScreen) with Minecraft's inventory and 3x3 crafting a click away (a crafting
 * table that needs no block, opened by the server: SkyNet.PipBoyCrafting). New Vegas tells where its
 * screen is (kInPipBoyPage) and which menu is open (kInPipBoyState).
 */
public final class PipBoyPage {
	private static volatile boolean active;
	private static boolean minecraftTab;            // ITEMS shows Minecraft's crafting
	private static int shown = -1;                  // the menu a screen was opened for
	private static float left, top, right, bottom;  // the Pip-Boy screen, as fractions of the window

	private PipBoyPage() {
	}

	public static boolean active() {
		return active;
	}

	public static void register() {
		ScreenEvents.AFTER_INIT.register((client, screen, width, height) -> {
			if (active && screen instanceof CraftingScreen crafting) {
				place(crafting, width, height);
				var access = (AbstractContainerScreenAccessor) crafting;
				int x = access.vegascraft$leftPos() + access.vegascraft$imageWidth() - 72, y = access.vegascraft$topPos() + 3;
				Screens.getWidgets(crafting).add(Button.builder(Component.literal("< New Vegas"), b -> showNewVegas(client)).bounds(x, y, 68, 12).build());
			}
		});
	}

	public static void open(Minecraft minecraft, float l, float t, float r, float b) {
		left = l;
		top = t;
		right = r;
		bottom = b;
		active = true;
		shown = -1;
		showSection(minecraft);
	}

	/** kInPipBoyState: the menu may have changed. */
	public static void sectionChanged(Minecraft minecraft) {
		if (active && shown != PipBoyModel.section()) {
			showSection(minecraft);
		}
	}

	public static void close(Minecraft minecraft) {
		boolean was = active;
		active = false;
		shown = -1;
		if (!was || minecraft.player == null) {
			return;
		}
		Screen screen = minecraft.gui.screen();
		if (screen instanceof CraftingScreen) {
			minecraft.player.closeContainer();
		} else if (isPage(screen)) {
			minecraft.gui.setScreen(null);
		}
	}

	private static boolean isPage(Screen screen) {
		return screen instanceof NvItemsScreen || screen instanceof StatsScreen || screen instanceof DataScreen;
	}

	private static void showSection(Minecraft minecraft) {
		int section = PipBoyModel.section();
		shown = section;
		if (minecraft.gui.screen() instanceof CraftingScreen && minecraft.player != null) {
			// Closing the crafting table hands its grid back to the inventory.
			minecraft.player.connection.send(new net.minecraft.network.protocol.game.ServerboundContainerClosePacket(minecraft.player.containerMenu.containerId));
			minecraft.player.containerMenu = minecraft.player.inventoryMenu;
		}
		switch (section) {
			case PipBoyModel.STATS -> minecraft.gui.setScreen(new StatsScreen());
			case PipBoyModel.DATA -> minecraft.gui.setScreen(new DataScreen());
			default -> {
				if (minecraftTab) {
					showMinecraft(minecraft);
				} else {
					minecraft.gui.setScreen(new NvItemsScreen());
				}
			}
		}
	}

	/** F4: New Vegas' items and Minecraft's inventory and crafting. */
	public static void toggleTab(Minecraft minecraft) {
		if (!active || PipBoyModel.section() != PipBoyModel.ITEMS) {
			return;
		}
		if (minecraft.gui.screen() instanceof CraftingScreen) {
			showNewVegas(minecraft);
		} else {
			showMinecraft(minecraft);
		}
	}

	public static void showMinecraft(Minecraft minecraft) {
		minecraftTab = true;
		if (minecraft.player != null && ClientPlayNetworking.canSend(SkyNet.PipBoyCrafting.TYPE)) {
			ClientPlayNetworking.send(SkyNet.PipBoyCrafting.INSTANCE);
		}
	}

	public static void showNewVegas(Minecraft minecraft) {
		minecraftTab = false;
		showSection(minecraft);
	}

	/** New Vegas' own screen for the open menu (what the pages don't have yet). */
	public static void showNative() {
		SkyLink.pushEvent(Proto.EV_PIPBOY_NATIVE, 0, PipBoyModel.section(), 0.0F, 0.0F, 0.0F, 0);
	}

	/** The Pip-Boy's screen in GUI pixels: left, top, right, bottom. */
	public static int[] rect(int width, int height) {
		return new int[] { Math.round(left * width), Math.round(top * height), Math.round(right * width), Math.round(bottom * height) };
	}

	/** Centres the crafting panel (and its buttons) on the Pip-Boy's screen. */
	private static void place(CraftingScreen screen, int width, int height) {
		var access = (AbstractContainerScreenAccessor) screen;
		if (right <= left || bottom <= top) {
			return;
		}
		int cx = Math.round((left + right) * 0.5F * width), cy = Math.round((top + bottom) * 0.5F * height);
		int x = cx - access.vegascraft$imageWidth() / 2, y = cy - access.vegascraft$imageHeight() / 2;
		int dx = x - access.vegascraft$leftPos(), dy = y - access.vegascraft$topPos();
		access.vegascraft$setLeftPos(x);
		access.vegascraft$setTopPos(y);
		for (AbstractWidget widget : Screens.getWidgets(screen)) {
			widget.setX(widget.getX() + dx);
			widget.setY(widget.getY() + dy);
		}
	}

	/** Screen backgrounds (blur, dimming) would hide the Pip-Boy around the page. */
	public static boolean hidesBackground(Screen screen) {
		return active && (screen instanceof CraftingScreen || isPage(screen));
	}
}
