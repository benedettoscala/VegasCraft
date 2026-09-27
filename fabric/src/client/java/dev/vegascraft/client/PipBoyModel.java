package dev.vegascraft.client;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.math.Axis;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.client.renderer.texture.OverlayTexture;
import net.minecraft.core.component.DataComponents;
import net.minecraft.resources.Identifier;
import net.minecraft.util.LightCoordsUtil;
import net.minecraft.world.item.ItemDisplayContext;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;

/**
 * The Pip-Boy, Minecraft style (models made by tools/gen_pipboy.py): the casing, three buttons
 * (STATS, ITEMS, DATA; the open menu's is lit, a pressed one sinks) and a knob that turns with the
 * mouse wheel. New Vegas tells which menu is open and what is pressed (kInPipBoyState).
 */
public final class PipBoyModel {
	public static final int STATS = 0, ITEMS = 1, DATA = 2;
	private static final String[] BUTTONS = { "stats", "items", "data" };
	private static final float PRESS = 0.9F / 16.0F;               // how far a pressed button sinks
	private static final float KNOB_X = 1.4F / 16.0F - 0.5F, KNOB_Y = 2.4F / 16.0F - 0.5F;  // pivot, in block units  // pivot, in block units
	private static final float DEGREES_PER_TICK = 22.5F;

	private static volatile int section = ITEMS;
	private static volatile int pressed;
	private static volatile int knobTicks;

	private static final ItemStackRenderState BODY = new ItemStackRenderState();
	private static final ItemStackRenderState KNOB = new ItemStackRenderState();
	private static final ItemStackRenderState[] OFF = { new ItemStackRenderState(), new ItemStackRenderState(), new ItemStackRenderState() };
	private static final ItemStackRenderState[] ON = { new ItemStackRenderState(), new ItemStackRenderState(), new ItemStackRenderState() };
	private static ItemStack bodyStack, knobStack;
	private static final ItemStack[] OFF_STACKS = new ItemStack[3], ON_STACKS = new ItemStack[3];

	private PipBoyModel() {
	}

	/** kInPipBoyState: the menu open, the buttons held (bit per button) and the knob's position. */
	public static void set(int newSection, int pressedMask, int ticks) {
		section = Math.max(0, Math.min(2, newSection));
		pressed = pressedMask;
		knobTicks = ticks;
	}

	public static int section() {
		return section;
	}

	private static ItemStack stack(String model) {
		ItemStack s = new ItemStack(Items.STICK);
		s.set(DataComponents.ITEM_MODEL, Identifier.fromNamespaceAndPath("vegascraft", model));
		return s;
	}

	private static void resolve(Minecraft minecraft, ItemStackRenderState state, ItemStack stack) {
		minecraft.getItemModelResolver().updateForTopItem(state, stack, ItemDisplayContext.NONE, minecraft.level, null, 0);
	}

	/** Draws every part; the pose is already on the Pip-Boy's screen (NvArmPose.applyPipBoy). */
	public static void draw(Minecraft minecraft, PoseStack pose, SubmitNodeCollector collector) {
		if (bodyStack == null) {
			bodyStack = stack("pipboy");
			knobStack = stack("pipboy_knob");
			for (int i = 0; i < 3; i++) {
				OFF_STACKS[i] = stack("pipboy_btn_" + BUTTONS[i]);
				ON_STACKS[i] = stack("pipboy_btn_" + BUTTONS[i] + "_lit");
			}
		}
		// Lit like the Pip-Boy's own screen light, whatever the hour in New Vegas.
		final int light = LightCoordsUtil.FULL_BRIGHT;
		resolve(minecraft, BODY, bodyStack);
		BODY.submit(pose, collector, light, OverlayTexture.NO_OVERLAY, 0);
		int open = section, held = pressed;
		for (int i = 0; i < 3; i++) {
			ItemStackRenderState state = i == open ? ON[i] : OFF[i];
			resolve(minecraft, state, i == open ? ON_STACKS[i] : OFF_STACKS[i]);
			pose.pushPose();
			if ((held & (1 << i)) != 0) {
				pose.translate(0.0F, 0.0F, -PRESS);
			}
			state.submit(pose, collector, light, OverlayTexture.NO_OVERLAY, 0);
			pose.popPose();
		}
		resolve(minecraft, KNOB, knobStack);
		pose.pushPose();
		pose.translate(KNOB_X, KNOB_Y, 0.0F);
		pose.rotateDegrees(Axis.ZP, knobTicks * DEGREES_PER_TICK);
		pose.translate(-KNOB_X, -KNOB_Y, 0.0F);
		KNOB.submit(pose, collector, light, OverlayTexture.NO_OVERLAY, 0);
		pose.popPose();
	}
}
