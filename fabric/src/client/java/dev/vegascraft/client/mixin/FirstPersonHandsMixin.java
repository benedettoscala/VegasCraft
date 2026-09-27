package dev.vegascraft.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.vegascraft.client.NvArmPose;
import dev.vegascraft.client.render.AvatarExporter;
import dev.vegascraft.client.SkyClient;
import dev.vegascraft.item.NvItems;
import net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.state.level.FirstPersonHandsAndItemsRenderState;
import net.minecraft.client.renderer.state.level.PlayerRenderState;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * A New Vegas weapon is drawn by New Vegas (its real model and animations on the native player's
 * skeleton): Minecraft draws only its two arms, on that skeleton's bones (NvArmPose), and no
 * off-hand item.
 */
@Mixin(FirstPersonHandsAndItemsRenderer.class)
public abstract class FirstPersonHandsMixin {
	@Shadow
	private void renderPlayerHand(PoseStack pose, SubmitNodeCollector collector, int light, HumanoidArm arm, PlayerRenderState player) {
	}

	@Inject(method = "submitArmWithItem", at = @At("HEAD"), cancellable = true)
	private void vegascraft$armsOnNvWeapon(PlayerRenderState player, FirstPersonHandsAndItemsRenderState hands, float partialTick, float pitch,
		InteractionHand hand, float swing, ItemStack stack, float equip, PoseStack pose, SubmitNodeCollector collector, int light, CallbackInfo ci) {
		if (!SkyClient.linked()) {
			return;
		}
		boolean pipBoy = hand == InteractionHand.MAIN_HAND ? NvArmPose.update() && NvArmPose.pipBoy() : NvArmPose.pipBoy();
		if (!pipBoy && !NvItems.isNvWielded(hands.mainHandItem)) {
			if (hand == InteractionHand.MAIN_HAND) AvatarExporter.clearHands();
			return;
		}
		ci.cancel();
		if (!pipBoy && !NvArmPose.showArms()) {
			// The arms are hidden by default while a New Vegas weapon is held (config/vegascraft_arms.txt: showArms 1).
			if (hand == InteractionHand.MAIN_HAND) AvatarExporter.clearHands();
			return;
		}
		if (hand != InteractionHand.MAIN_HAND || player.avatarRenderState == null || player.avatarRenderState.isInvisible || !(pipBoy || NvArmPose.update())) {
			return;
		}
		SubmitNodeCollector armCollector = pipBoy ? collector : AvatarExporter.beginHands();
		if (pipBoy) AvatarExporter.clearHands();
		for (HumanoidArm arm : HumanoidArm.values()) {
			pose.pushPose();
			if (!pipBoy) {
				// The vanilla hand stack starts with inverse camera rotation: remove it.
				var camera = net.minecraft.client.Minecraft.getInstance().gameRenderer.gameRenderState().levelRenderState.cameraRenderState;
				pose.last().pose().set(new org.joml.Matrix4f(camera.viewRotationMatrix).mul(pose.last().pose()));
			}
			NvArmPose.apply(pose, arm);
			renderPlayerHand(pose, armCollector, light, arm, player);
			pose.popPose();
		}
		if (!pipBoy) AvatarExporter.endHands();
		if (pipBoy) {
			// New Vegas' Pip-Boy is up: a Minecraft one around its screen (New Vegas draws the
			// screen and its menus; the overlay leaves that quad open).
			pose.pushPose();
			NvArmPose.applyPipBoy(pose);
			dev.vegascraft.client.PipBoyModel.draw(net.minecraft.client.Minecraft.getInstance(), pose, collector);
			pose.popPose();
		}
	}
}
